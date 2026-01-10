/*
 * Zyphar Hash Test Pass
 * Tests the content hashing infrastructure for incremental synthesis
 */

#include "kernel/yosys.h"
#include "kernel/rtlil.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ZypharHashTestPass : public Pass {
    ZypharHashTestPass() : Pass("zyphar_hash_test", "test content hashing for incremental synthesis") { }

    void help() override
    {
        log("\n");
        log("    zyphar_hash_test\n");
        log("\n");
        log("This command tests the Zyphar incremental synthesis hash infrastructure.\n");
        log("It prints the content hash for each module and demonstrates hash invalidation.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing ZYPHAR_HASH_TEST pass.\n");

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            break;
        }
        extra_args(args, argidx, design);

        log("\n");
        log("=== Zyphar Content Hash Test ===\n");
        log("\n");

        // Test 1: Compute hashes for all modules
        log("Test 1: Computing content hashes for all modules\n");
        std::map<RTLIL::IdString, uint64_t> initial_hashes;

        for (auto module : design->modules()) {
            uint64_t hash = module->get_content_hash();
            initial_hashes[module->name] = hash;
            log("  Module %-20s hash: 0x%016llx\n",
                log_id(module->name), (unsigned long long)hash);
        }
        log("\n");

        // Test 2: Verify hash is cached (should return same value)
        log("Test 2: Verifying hash caching (second call should be instant)\n");
        for (auto module : design->modules()) {
            uint64_t hash1 = module->get_content_hash();
            uint64_t hash2 = module->get_content_hash();
            if (hash1 == hash2) {
                log("  Module %-20s PASS (hash stable: 0x%016llx)\n",
                    log_id(module->name), (unsigned long long)hash1);
            } else {
                log("  Module %-20s FAIL (hash changed: 0x%016llx -> 0x%016llx)\n",
                    log_id(module->name), (unsigned long long)hash1, (unsigned long long)hash2);
            }
        }
        log("\n");

        // Test 3: Add a wire and verify hash changes
        log("Test 3: Testing hash invalidation on wire addition\n");
        for (auto module : design->modules()) {
            uint64_t hash_before = module->get_content_hash();

            // Add a test wire
            RTLIL::Wire *test_wire = module->addWire("\\zyphar_test_wire", 8);

            // Hash should have been invalidated
            uint64_t hash_after = module->get_content_hash();

            // Remove the test wire
            module->remove({test_wire});

            // Hash should change again
            uint64_t hash_restored = module->get_content_hash();

            if (hash_before != hash_after && hash_before == hash_restored) {
                log("  Module %-20s PASS (hash changed after add, restored after remove)\n",
                    log_id(module->name));
                log("    Before:   0x%016llx\n", (unsigned long long)hash_before);
                log("    After:    0x%016llx\n", (unsigned long long)hash_after);
                log("    Restored: 0x%016llx\n", (unsigned long long)hash_restored);
            } else {
                log("  Module %-20s FAIL\n", log_id(module->name));
                log("    Before:   0x%016llx\n", (unsigned long long)hash_before);
                log("    After:    0x%016llx\n", (unsigned long long)hash_after);
                log("    Restored: 0x%016llx\n", (unsigned long long)hash_restored);
            }
        }
        log("\n");

        // Test 4: Verify content_matches()
        log("Test 4: Testing content_matches() API\n");
        for (auto module : design->modules()) {
            uint64_t current_hash = module->get_content_hash();
            bool matches = module->content_matches(current_hash);
            bool no_match = !module->content_matches(current_hash + 1);

            if (matches && no_match) {
                log("  Module %-20s PASS (content_matches works correctly)\n",
                    log_id(module->name));
            } else {
                log("  Module %-20s FAIL (content_matches: same=%d, different=%d)\n",
                    log_id(module->name), matches, no_match);
            }
        }
        log("\n");

        log("=== Hash Test Complete ===\n");
        log("\n");
    }
} ZypharHashTestPass;

PRIVATE_NAMESPACE_END
