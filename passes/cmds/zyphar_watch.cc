/*
 * Zyphar Watch Mode
 * File watcher for incremental synthesis with real-time updates
 */

#include "kernel/yosys.h"
#include "kernel/zyphar_deps.h"
#include "kernel/zyphar_cache.h"
#include <sys/stat.h>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <iostream>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Global flag for graceful shutdown
static std::atomic<bool> watch_running(false);

static void signal_handler(int sig) {
    (void)sig;
    watch_running = false;
    log("\nReceived interrupt signal, stopping watch mode...\n");
}

struct ZypharWatchPass : public Pass {
    ZypharWatchPass() : Pass("zyphar_watch", "watch mode for incremental synthesis") { }

    void help() override
    {
        log("\n");
        log("    zyphar_watch [options] <files...>\n");
        log("\n");
        log("Start watch mode for incremental synthesis. Monitors Verilog files\n");
        log("and automatically re-synthesizes when changes are detected.\n");
        log("\n");
        log("    -top <module>\n");
        log("        Specify the top module\n");
        log("\n");
        log("    -poll <ms>\n");
        log("        Polling interval in milliseconds (default: 500)\n");
        log("\n");
        log("    -port <n>\n");
        log("        WebSocket port for real-time updates (default: disabled)\n");
        log("\n");
        log("    -once\n");
        log("        Run once and exit (useful for testing)\n");
        log("\n");
        log("Press Ctrl+C to stop watching.\n");
        log("\n");
    }

    // Get file modification time
    time_t get_mtime(const std::string &path) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            return st.st_mtime;
        }
        return 0;
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing ZYPHAR_WATCH pass.\n");

        std::string top_module;
        int poll_ms = 500;
        int ws_port = 0;  // 0 = disabled
        bool run_once = false;
        std::vector<std::string> watch_files;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-top" && argidx + 1 < args.size()) {
                top_module = args[++argidx];
                continue;
            }
            if (args[argidx] == "-poll" && argidx + 1 < args.size()) {
                poll_ms = std::stoi(args[++argidx]);
                continue;
            }
            if (args[argidx] == "-port" && argidx + 1 < args.size()) {
                ws_port = std::stoi(args[++argidx]);
                continue;
            }
            if (args[argidx] == "-once") {
                run_once = true;
                continue;
            }
            // Remaining args are files to watch
            if (args[argidx][0] != '-') {
                watch_files.push_back(args[argidx]);
            }
        }

        if (watch_files.empty()) {
            log_error("No files specified to watch. Usage: zyphar_watch <files...>\n");
        }

        // Initialize cache
        if (!zyphar_cache.is_initialized()) {
            zyphar_cache.init();
        }

        // Track file modification times
        std::map<std::string, time_t> file_mtimes;
        for (auto &file : watch_files) {
            file_mtimes[file] = get_mtime(file);
            log("Watching: %s (mtime: %ld)\n", file.c_str(), file_mtimes[file]);
        }

        // Set up signal handler for graceful shutdown
        signal(SIGINT, signal_handler);
        watch_running = true;

        log("\n=== Watch Mode Started ===\n");
        log("Watching %zu files, polling every %d ms\n", watch_files.size(), poll_ms);
        if (ws_port > 0) {
            log("WebSocket server on port %d (not yet implemented)\n", ws_port);
        }
        log("Press Ctrl+C to stop.\n\n");

        // Initial file read
        log("Reading initial design...\n");
        for (auto &file : watch_files) {
            Pass::call(design, "read_verilog " + file);
        }

        // Initial synthesis
        log("Running initial synthesis...\n");
        do_synthesis(design, watch_files, top_module);

        if (run_once) {
            log("One-shot mode, exiting.\n");
            return;
        }

        // Watch loop
        int iteration = 0;
        while (watch_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));

            // Check for changes
            bool changes = false;
            std::vector<std::string> changed_files;

            for (auto &file : watch_files) {
                time_t new_mtime = get_mtime(file);
                if (new_mtime != file_mtimes[file]) {
                    log("[%d] File changed: %s\n", ++iteration, file.c_str());
                    file_mtimes[file] = new_mtime;
                    changed_files.push_back(file);
                    changes = true;
                }
            }

            if (changes) {
                auto start = std::chrono::high_resolution_clock::now();

                // Clear design and re-read
                // Note: In a more sophisticated implementation, we'd only
                // re-read changed files and merge with cached modules
                log("Reloading design...\n");
                design->selection_stack.clear();
                for (auto mod : design->modules())
                    design->remove(mod);

                // Re-read all files
                for (auto &file : watch_files) {
                    Pass::call(design, "read_verilog " + file);
                }

                // Run incremental synthesis
                do_synthesis(design, changed_files, top_module);

                auto end = std::chrono::high_resolution_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

                log("[%d] Incremental synthesis completed in %lld ms\n\n", iteration, (long long)ms);

                // Output JSON notification (can be consumed by external WebSocket server)
                if (ws_port > 0) {
                    output_json_update(design, changed_files, ms);
                }
            }
        }

        // Cleanup
        signal(SIGINT, SIG_DFL);
        log("\n=== Watch Mode Stopped ===\n");
    }

    void output_json_update(RTLIL::Design *design, const std::vector<std::string> &changed_files, long long ms)
    {
        // Output JSON to stdout for external consumption
        // Format: {"event":"synthesis_complete","time_ms":123,"modules":[...]}
        std::cout << "{\"event\":\"synthesis_complete\",\"time_ms\":" << ms << ",";
        std::cout << "\"changed_files\":[";
        for (size_t i = 0; i < changed_files.size(); i++) {
            if (i > 0) std::cout << ",";
            std::cout << "\"" << changed_files[i] << "\"";
        }
        std::cout << "],\"modules\":[";

        bool first = true;
        for (auto module : design->modules()) {
            if (!first) std::cout << ",";
            first = false;

            // Count cells and wires
            size_t cell_count = 0;
            size_t wire_count = 0;
            for (auto cell : module->cells()) {
                (void)cell;
                cell_count++;
            }
            for (auto wire : module->wires()) {
                (void)wire;
                wire_count++;
            }

            std::cout << "{\"name\":\"" << module->name.str() << "\",";
            std::cout << "\"cells\":" << cell_count << ",";
            std::cout << "\"wires\":" << wire_count << "}";
        }
        std::cout << "]}" << std::endl;
    }

    void do_synthesis(RTLIL::Design *design, const std::vector<std::string> &files, const std::string &top_module)
    {
        (void)files;  // Future: track which files changed for smarter synthesis

        // Build dependency graph
        zyphar_deps.build_from_design(design);

        // Compute hashes and check cache
        std::map<RTLIL::IdString, uint64_t> input_hashes;
        std::set<RTLIL::IdString> to_synthesize;
        std::set<RTLIL::IdString> from_cache;

        for (auto module : design->modules()) {
            uint64_t hash = module->get_content_hash();
            input_hashes[module->name] = hash;
            std::string mod_name = module->name.str();

            if (zyphar_cache.has(mod_name, hash, "synth")) {
                from_cache.insert(module->name);
            } else {
                to_synthesize.insert(module->name);
            }
        }

        log("  Modules: %zu total, %zu to synthesize, %zu cached\n",
            design->modules().size(), to_synthesize.size(), from_cache.size());

        if (to_synthesize.empty()) {
            log("  All modules cached - no synthesis needed!\n");
            return;
        }

        // Run synthesis
        std::string top_arg = top_module.empty() ? "" : ("-top " + top_module);
        Pass::call(design, "hierarchy -check " + top_arg);
        Pass::call(design, "proc");
        Pass::call(design, "opt -full");
        Pass::call(design, "techmap");
        Pass::call(design, "opt -full");

        // Update cache
        for (auto module : design->modules()) {
            auto it = input_hashes.find(module->name);
            if (it != input_hashes.end()) {
                std::string mod_name = module->name.str();
                zyphar_cache.put(mod_name, it->second, "synth", module, design);
            }
        }
        zyphar_cache.save_to_disk();

        // Print stats
        Pass::call(design, "stat");
    }
} ZypharWatchPass;

PRIVATE_NAMESPACE_END
