/*
 * Zyphar Dependency Graph Pass
 * Builds and queries module dependency relationships
 */

#include "kernel/yosys.h"
#include "kernel/zyphar_deps.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ZypharDepsPass : public Pass {
    ZypharDepsPass() : Pass("zyphar_deps", "build and query module dependency graph") { }

    void help() override
    {
        log("\n");
        log("    zyphar_deps [options]\n");
        log("\n");
        log("Build and query the module dependency graph for incremental synthesis.\n");
        log("\n");
        log("    -build\n");
        log("        Build the dependency graph from current design\n");
        log("\n");
        log("    -show\n");
        log("        Display the dependency graph\n");
        log("\n");
        log("    -json\n");
        log("        Output dependency graph as JSON\n");
        log("\n");
        log("    -affected <module>\n");
        log("        Show all modules affected if <module> changes\n");
        log("\n");
        log("    -order\n");
        log("        Show topological synthesis order\n");
        log("\n");
        log("    -store\n");
        log("        Store graph in design scratchpad\n");
        log("\n");
        log("    -load\n");
        log("        Load graph from design scratchpad\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing ZYPHAR_DEPS pass.\n");

        bool do_build = false;
        bool do_show = false;
        bool do_json = false;
        bool do_order = false;
        bool do_store = false;
        bool do_load = false;
        std::string affected_module;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-build") {
                do_build = true;
                continue;
            }
            if (args[argidx] == "-show") {
                do_show = true;
                continue;
            }
            if (args[argidx] == "-json") {
                do_json = true;
                continue;
            }
            if (args[argidx] == "-order") {
                do_order = true;
                continue;
            }
            if (args[argidx] == "-store") {
                do_store = true;
                continue;
            }
            if (args[argidx] == "-load") {
                do_load = true;
                continue;
            }
            if (args[argidx] == "-affected" && argidx + 1 < args.size()) {
                affected_module = args[++argidx];
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        // Default action: build and show
        if (!do_build && !do_show && !do_json && !do_order && !do_store && !do_load && affected_module.empty()) {
            do_build = true;
            do_show = true;
        }

        if (do_load) {
            log("Loading dependency graph from scratchpad...\n");
            if (zyphar_deps.load_from_scratchpad(design)) {
                log("Loaded graph with %zu modules.\n", zyphar_deps.module_count());
            } else {
                log_warning("No dependency graph found in scratchpad.\n");
            }
        }

        if (do_build) {
            log("Building dependency graph...\n");
            zyphar_deps.build_from_design(design);
            log("Built graph with %zu modules.\n", zyphar_deps.module_count());
        }

        if (!zyphar_deps.is_valid()) {
            log_error("Dependency graph not built. Use -build first.\n");
        }

        if (do_show) {
            zyphar_deps.log_graph();
        }

        if (do_json) {
            log("\n%s\n", zyphar_deps.to_json().c_str());
        }

        if (do_order) {
            log("\nSynthesis order (dependencies first):\n");
            auto order = zyphar_deps.get_topological_order();
            for (size_t i = 0; i < order.size(); i++) {
                log("  %zu. %s\n", i + 1, log_id(order[i]));
            }
            log("\n");
        }

        if (!affected_module.empty()) {
            RTLIL::IdString mod_id("\\"+affected_module);
            log("\nModules affected if %s changes:\n", affected_module.c_str());

            std::set<RTLIL::IdString> changed;
            changed.insert(mod_id);
            auto affected = zyphar_deps.get_affected_modules(changed);

            for (auto &m : affected) {
                log("  - %s%s\n", log_id(m), (m == mod_id ? " (changed)" : ""));
            }
            log("\nTotal: %zu modules need re-synthesis.\n\n", affected.size());
        }

        if (do_store) {
            log("Storing dependency graph in scratchpad...\n");
            zyphar_deps.store_in_scratchpad(design);
            log("Done.\n");
        }
    }
} ZypharDepsPass;

PRIVATE_NAMESPACE_END
