/*
 * Zyphar Cache Control Pass
 * Manages the module cache for incremental synthesis
 */

#include "kernel/yosys.h"
#include "kernel/zyphar_cache.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ZypharCachePass : public Pass {
    ZypharCachePass() : Pass("zyphar_cache", "manage module cache for incremental synthesis") { }

    void help() override
    {
        log("\n");
        log("    zyphar_cache [options]\n");
        log("\n");
        log("Manage the Zyphar module cache for incremental synthesis.\n");
        log("\n");
        log("    -init [dir]\n");
        log("        Initialize cache (default: ~/.cache/zyphar)\n");
        log("\n");
        log("    -status\n");
        log("        Show cache statistics\n");
        log("\n");
        log("    -list\n");
        log("        List all cached entries\n");
        log("\n");
        log("    -clear\n");
        log("        Clear all cached entries\n");
        log("\n");
        log("    -save\n");
        log("        Save cache to disk\n");
        log("\n");
        log("    -invalidate <module>\n");
        log("        Invalidate all cached versions of a module\n");
        log("\n");
        log("    -store <module> <pass_seq>\n");
        log("        Store current state of module in cache\n");
        log("\n");
        log("    -restore <module>\n");
        log("        Restore module from cache (if available)\n");
        log("\n");
        log("    -max_entries <n>\n");
        log("        Set maximum number of cache entries (default: 1000)\n");
        log("\n");
        log("    -max_size <mb>\n");
        log("        Set maximum cache size in megabytes (default: 500)\n");
        log("\n");
        log("    -max_age <days>\n");
        log("        Set maximum cache entry age in days (default: 30)\n");
        log("\n");
        log("    -evict\n");
        log("        Force cache eviction based on current limits\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing ZYPHAR_CACHE pass.\n");

        bool do_init = false;
        bool do_status = false;
        bool do_list = false;
        bool do_clear = false;
        bool do_save = false;
        bool do_evict = false;
        std::string init_dir;
        std::string invalidate_module;
        std::string store_module;
        std::string store_pass_seq;
        std::string restore_module;
        int max_entries = -1;
        int max_size_mb = -1;
        int max_age_days = -1;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-init") {
                do_init = true;
                if (argidx + 1 < args.size() && args[argidx + 1][0] != '-') {
                    init_dir = args[++argidx];
                }
                continue;
            }
            if (args[argidx] == "-status") {
                do_status = true;
                continue;
            }
            if (args[argidx] == "-list") {
                do_list = true;
                continue;
            }
            if (args[argidx] == "-clear") {
                do_clear = true;
                continue;
            }
            if (args[argidx] == "-save") {
                do_save = true;
                continue;
            }
            if (args[argidx] == "-invalidate" && argidx + 1 < args.size()) {
                invalidate_module = args[++argidx];
                continue;
            }
            if (args[argidx] == "-store" && argidx + 2 < args.size()) {
                store_module = args[++argidx];
                store_pass_seq = args[++argidx];
                continue;
            }
            if (args[argidx] == "-restore" && argidx + 1 < args.size()) {
                restore_module = args[++argidx];
                continue;
            }
            if (args[argidx] == "-max_entries" && argidx + 1 < args.size()) {
                max_entries = std::stoi(args[++argidx]);
                continue;
            }
            if (args[argidx] == "-max_size" && argidx + 1 < args.size()) {
                max_size_mb = std::stoi(args[++argidx]);
                continue;
            }
            if (args[argidx] == "-max_age" && argidx + 1 < args.size()) {
                max_age_days = std::stoi(args[++argidx]);
                continue;
            }
            if (args[argidx] == "-evict") {
                do_evict = true;
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        // Default action: init and status
        if (!do_init && !do_status && !do_list && !do_clear && !do_save &&
            invalidate_module.empty() && store_module.empty() && restore_module.empty()) {
            do_init = true;
            do_status = true;
        }

        if (do_init) {
            if (!zyphar_cache.is_initialized()) {
                zyphar_cache.init(init_dir);
            } else {
                log("Cache already initialized at: %s\n", zyphar_cache.get_cache_dir().c_str());
            }
        }

        if (!zyphar_cache.is_initialized()) {
            if (do_status || do_list || do_clear || do_save || do_evict ||
                !invalidate_module.empty() || !store_module.empty() || !restore_module.empty() ||
                max_entries >= 0 || max_size_mb >= 0 || max_age_days >= 0) {
                log_error("Cache not initialized. Use -init first.\n");
            }
            return;
        }

        // Configure cache limits
        if (max_entries >= 0) {
            zyphar_cache.set_max_entries(static_cast<size_t>(max_entries));
            log("Set max cache entries to %d\n", max_entries);
        }
        if (max_size_mb >= 0) {
            zyphar_cache.set_max_size_bytes(static_cast<size_t>(max_size_mb) * 1024 * 1024);
            log("Set max cache size to %d MB\n", max_size_mb);
        }
        if (max_age_days >= 0) {
            zyphar_cache.set_max_age_days(max_age_days);
            log("Set max cache age to %d days\n", max_age_days);
        }

        if (do_evict) {
            log("Running cache eviction...\n");
            size_t before = zyphar_cache.entry_count();
            zyphar_cache.evict_if_needed();
            size_t after = zyphar_cache.entry_count();
            log("Eviction complete: %zu -> %zu entries\n", before, after);
        }

        if (do_clear) {
            log("Clearing cache...\n");
            zyphar_cache.clear();
            log("Cache cleared.\n");
        }

        if (!invalidate_module.empty()) {
            zyphar_cache.invalidate(invalidate_module);
        }

        if (!store_module.empty()) {
            RTLIL::Module *module = design->module("\\" + store_module);
            if (!module) {
                module = design->module(RTLIL::IdString(store_module));
            }

            if (!module) {
                log_error("Module not found: %s\n", store_module.c_str());
            }

            uint64_t hash = module->get_content_hash();
            zyphar_cache.put(store_module, hash, store_pass_seq, module, design);
        }

        if (!restore_module.empty()) {
            // Find module to get its hash (must exist in design)
            RTLIL::Module *module = design->module("\\" + restore_module);
            if (!module) {
                module = design->module(RTLIL::IdString(restore_module));
            }

            if (module) {
                uint64_t hash = module->get_content_hash();
                if (zyphar_cache.restore(restore_module, hash, "", design)) {
                    log("Restored module %s from cache.\n", restore_module.c_str());
                } else {
                    log("Module %s not found in cache.\n", restore_module.c_str());
                }
            } else {
                log_warning("Module %s not in design, cannot determine hash for lookup.\n",
                           restore_module.c_str());
            }
        }

        if (do_save) {
            zyphar_cache.save_to_disk();
        }

        if (do_status) {
            zyphar_cache.log_stats();
        }

        if (do_list) {
            zyphar_cache.log_entries();
        }
    }
} ZypharCachePass;

PRIVATE_NAMESPACE_END
