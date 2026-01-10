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

    // Get file modification time, returns 0 if file doesn't exist
    time_t get_mtime(const std::string &path) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            return st.st_mtime;
        }
        return 0;
    }

    // Check if file exists and is readable
    bool file_exists(const std::string &path) {
        struct stat st;
        return stat(path.c_str(), &st) == 0;
    }

    // Safe file read with error handling
    bool safe_read_verilog(RTLIL::Design *design, const std::string &file) {
        if (!file_exists(file)) {
            log_warning("File not found: %s\n", file.c_str());
            return false;
        }

        try {
            Pass::call(design, "read_verilog " + file);
            return true;
        } catch (const std::exception &e) {
            log_warning("Failed to read %s: %s\n", file.c_str(), e.what());
            return false;
        } catch (...) {
            log_warning("Failed to read %s: unknown error\n", file.c_str());
            return false;
        }
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

        // Track file modification times and validate files
        std::map<std::string, time_t> file_mtimes;
        for (auto &file : watch_files) {
            if (!file_exists(file)) {
                log_warning("File not found at start: %s\n", file.c_str());
            }
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

        // Initial file read with error handling
        log("Reading initial design...\n");
        bool initial_read_ok = true;
        for (auto &file : watch_files) {
            if (!safe_read_verilog(design, file)) {
                initial_read_ok = false;
            }
        }

        if (design->modules().size() == 0) {
            log_warning("No modules loaded. Check your Verilog files.\n");
        }

        // Initial synthesis (only if we have modules)
        if (design->modules().size() > 0) {
            log("Running initial synthesis...\n");
            try {
                do_synthesis(design, watch_files, top_module);
            } catch (const std::exception &e) {
                log_warning("Initial synthesis failed: %s\n", e.what());
            } catch (...) {
                log_warning("Initial synthesis failed: unknown error\n");
            }
        } else if (!initial_read_ok) {
            log_warning("Waiting for valid Verilog files...\n");
        }

        if (run_once) {
            log("One-shot mode, exiting.\n");
            return;
        }

        // Watch loop
        int iteration = 0;
        int consecutive_errors = 0;
        const int max_consecutive_errors = 5;
        const int debounce_ms = 100;  // Wait this long after detecting change for stability

        while (watch_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));

            // Check for changes
            bool changes = false;
            std::vector<std::string> changed_files;
            std::vector<std::string> missing_files;

            for (auto &file : watch_files) {
                time_t new_mtime = get_mtime(file);

                // Track missing files
                if (new_mtime == 0 && file_mtimes[file] != 0) {
                    log("[%d] File deleted or inaccessible: %s\n", iteration + 1, file.c_str());
                    missing_files.push_back(file);
                    file_mtimes[file] = 0;
                    changes = true;
                    continue;
                }

                // Track modified files
                if (new_mtime != file_mtimes[file] && new_mtime != 0) {
                    log("[%d] File changed: %s\n", ++iteration, file.c_str());
                    file_mtimes[file] = new_mtime;
                    changed_files.push_back(file);
                    changes = true;
                }
            }

            if (changes) {
                // Debounce: wait a bit for file to stabilize (editors do save-in-place)
                std::this_thread::sleep_for(std::chrono::milliseconds(debounce_ms));

                // Re-check if file changed again during debounce
                bool stable = true;
                for (auto &file : changed_files) {
                    time_t current_mtime = get_mtime(file);
                    if (current_mtime != file_mtimes[file]) {
                        file_mtimes[file] = current_mtime;
                        stable = false;
                    }
                }

                if (!stable) {
                    // File still changing, skip this iteration
                    log("File still changing, waiting...\n");
                    continue;
                }

                auto start = std::chrono::high_resolution_clock::now();

                // Clear design and re-read
                log("Reloading design...\n");
                design->selection_stack.clear();

                // Safely clear modules
                std::vector<RTLIL::IdString> to_remove;
                for (auto mod : design->modules())
                    to_remove.push_back(mod->name);
                for (auto &name : to_remove)
                    design->remove(design->module(name));

                // Re-read all files with error handling
                bool read_ok = true;
                for (auto &file : watch_files) {
                    if (!safe_read_verilog(design, file)) {
                        read_ok = false;
                    }
                }

                if (!read_ok || design->modules().size() == 0) {
                    consecutive_errors++;
                    if (consecutive_errors >= max_consecutive_errors) {
                        log_warning("Too many consecutive errors, consider fixing your files.\n");
                        consecutive_errors = 0;  // Reset to avoid spam
                    }
                    continue;
                }

                consecutive_errors = 0;  // Reset on success

                // Run incremental synthesis with error handling
                try {
                    do_synthesis(design, changed_files, top_module);
                } catch (const std::exception &e) {
                    log_warning("Synthesis failed: %s\n", e.what());
                    continue;
                } catch (...) {
                    log_warning("Synthesis failed: unknown error\n");
                    continue;
                }

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
