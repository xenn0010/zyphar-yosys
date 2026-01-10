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
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing ZYPHAR_CACHE pass.\n");

        bool do_init = false;
        bool do_status = false;
        bool do_list = false;
        bool do_clear = false;
        bool do_save = false;
        std::string init_dir;
        std::string invalidate_module;
        std::string store_module;
        std::string store_pass_seq;
        std::string restore_module;

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
            if (do_status || do_list || do_clear || do_save ||
                !invalidate_module.empty() || !store_module.empty() || !restore_module.empty()) {
                log_error("Cache not initialized. Use -init first.\n");
            }
            return;
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
