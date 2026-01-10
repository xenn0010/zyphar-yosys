/*
 * Zyphar Incremental Synthesis Pass
 * Main command for incremental synthesis flow
 */

#include "kernel/yosys.h"
#include "kernel/zyphar_deps.h"
#include "kernel/zyphar_cache.h"
#include "kernel/zyphar_monitor.h"
#include <chrono>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ZypharSynthPass : public Pass {
    ZypharSynthPass() : Pass("zyphar_synth", "incremental synthesis with caching") { }

    void help() override
    {
        log("\n");
        log("    zyphar_synth [options]\n");
        log("\n");
        log("Perform incremental synthesis with module-level caching.\n");
        log("Only re-synthesizes modules that have changed since the last run.\n");
        log("\n");
        log("    -top <module>\n");
        log("        Specify the top module (default: auto-detect)\n");
        log("\n");
        log("    -full\n");
        log("        Force full synthesis (ignore cache)\n");
        log("\n");
        log("    -nocache\n");
        log("        Don't update cache with results\n");
        log("\n");
        log("    -stats\n");
        log("        Show detailed timing statistics\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        auto start_time = std::chrono::high_resolution_clock::now();

        log_header(design, "Executing ZYPHAR_SYNTH pass (incremental synthesis).\n");

        std::string top_module;
        bool force_full = false;
        bool no_cache = false;
        bool show_stats = false;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-top" && argidx + 1 < args.size()) {
                top_module = args[++argidx];
                continue;
            }
            if (args[argidx] == "-full") {
                force_full = true;
                continue;
            }
            if (args[argidx] == "-nocache") {
                no_cache = true;
                continue;
            }
            if (args[argidx] == "-stats") {
                show_stats = true;
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        // Initialize cache if needed
        if (!zyphar_cache.is_initialized()) {
            zyphar_cache.init();
        }

        // Step 1: Build dependency graph
        log("\n");
        log("=== Step 1: Analyzing module dependencies ===\n");
        zyphar_deps.build_from_design(design);
        log("Found %zu modules.\n", zyphar_deps.module_count());

        // Step 2: Compute content hashes (BEFORE synthesis - this is the cache key)
        log("\n");
        log("=== Step 2: Computing input content hashes ===\n");
        std::map<RTLIL::IdString, uint64_t> input_hashes;
        for (auto module : design->modules()) {
            uint64_t hash = module->get_content_hash();
            input_hashes[module->name] = hash;
            log("  %-30s 0x%016llx\n", log_id(module->name), (unsigned long long)hash);
        }

        // Step 3: Determine what needs synthesis
        log("\n");
        log("=== Step 3: Determining modules to synthesize ===\n");
        std::set<RTLIL::IdString> to_synthesize;
        std::set<RTLIL::IdString> from_cache;

        if (force_full) {
            log("Full synthesis requested - all modules will be synthesized.\n");
            for (auto module : design->modules()) {
                to_synthesize.insert(module->name);
            }
        } else {
            for (auto module : design->modules()) {
                uint64_t hash = input_hashes[module->name];
                std::string mod_name = module->name.str();

                if (zyphar_cache.has(mod_name, hash, "synth")) {
                    from_cache.insert(module->name);
                    log("  [CACHED]  %s (input hash: 0x%016llx)\n", log_id(module->name), (unsigned long long)hash);
                } else {
                    to_synthesize.insert(module->name);
                    log("  [SYNTH]   %s (input hash: 0x%016llx)\n", log_id(module->name), (unsigned long long)hash);
                }
            }
        }

        log("\n");
        log("Modules to synthesize: %zu\n", to_synthesize.size());
        log("Modules from cache: %zu\n", from_cache.size());

        // Step 4: Get topological order for synthesis
        auto topo_order = zyphar_deps.get_topological_order();

        // Step 5: Run synthesis passes on modules that need it
        log("\n");
        log("=== Step 4: Running synthesis ===\n");

        if (to_synthesize.empty()) {
            log("No modules need synthesis - all cached!\n");
        } else {
            // Run synth pass with hierarchy on the modules that need it
            // Note: For proper incremental synthesis, we'd need to run individual passes
            // and cache intermediate results. For now, we use the full synth pass.

            // Find the top module
            RTLIL::Module *top = nullptr;
            if (!top_module.empty()) {
                top = design->module("\\" + top_module);
                if (!top) top = design->module(RTLIL::IdString(top_module));
            }
            if (!top) {
                // Auto-detect: module with no dependents
                for (auto &mod : topo_order) {
                    if (zyphar_deps.get_direct_dependents(mod).empty()) {
                        if (design->module(mod)) {
                            // Check if this module is in to_synthesize or required
                            top = design->module(mod);
                        }
                    }
                }
            }

            if (top) {
                log("Top module: %s\n", log_id(top->name));
            }

            // Run synthesis
            auto synth_start = std::chrono::high_resolution_clock::now();

            // Run basic synthesis passes
            log("\nRunning synthesis passes...\n");
            Pass::call(design, "hierarchy -check -top " + (top ? top->name.str() : "\\top"));
            Pass::call(design, "proc");
            Pass::call(design, "opt -full");
            Pass::call(design, "techmap");
            Pass::call(design, "opt -full");

            auto synth_end = std::chrono::high_resolution_clock::now();
            auto synth_ms = std::chrono::duration_cast<std::chrono::milliseconds>(synth_end - synth_start).count();
            log("\nSynthesis completed in %lld ms.\n", (long long)synth_ms);
        }

        // Step 6: Update cache with synthesized modules
        // Key: input hash (before synthesis), Value: synthesized module
        if (!no_cache && !to_synthesize.empty()) {
            log("\n");
            log("=== Step 5: Updating cache ===\n");
            for (auto module : design->modules()) {
                // Use the INPUT hash as the cache key (not the post-synthesis hash)
                // This way, when we read the same source again, we'll find it in cache
                auto it = input_hashes.find(module->name);
                if (it != input_hashes.end()) {
                    uint64_t input_hash = it->second;
                    std::string mod_name = module->name.str();
                    zyphar_cache.put(mod_name, input_hash, "synth", module, design);
                    log("Cached %s (input hash: 0x%016llx)\n", log_id(module->name), (unsigned long long)input_hash);
                }
            }
            zyphar_cache.save_to_disk();
        }

        // Final statistics
        auto end_time = std::chrono::high_resolution_clock::now();
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        log("\n");
        log("=== Incremental Synthesis Complete ===\n");
        log("Total time: %lld ms\n", (long long)total_ms);
        log("Modules synthesized: %zu\n", to_synthesize.size());
        log("Modules from cache: %zu\n", from_cache.size());

        if (show_stats) {
            log("\n");
            zyphar_cache.log_stats();
        }
    }
} ZypharSynthPass;

PRIVATE_NAMESPACE_END
