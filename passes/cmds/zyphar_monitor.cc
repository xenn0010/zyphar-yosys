/*
 * Zyphar Monitor Control Pass
 * Manages change tracking for incremental synthesis
 */

#include "kernel/yosys.h"
#include "kernel/zyphar_monitor.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ZypharMonitorPass : public Pass {
    ZypharMonitorPass() : Pass("zyphar_monitor", "manage change tracking for incremental synthesis") { }

    void help() override
    {
        log("\n");
        log("    zyphar_monitor [options]\n");
        log("\n");
        log("Manage the Zyphar change monitor for incremental synthesis.\n");
        log("\n");
        log("    -attach\n");
        log("        Attach monitor to current design\n");
        log("\n");
        log("    -detach\n");
        log("        Detach monitor from design\n");
        log("\n");
        log("    -status\n");
        log("        Show current change status\n");
        log("\n");
        log("    -reset\n");
        log("        Reset change tracking (mark current state as baseline)\n");
        log("\n");
        log("    -dirty\n");
        log("        List all dirty (changed) modules\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing ZYPHAR_MONITOR pass.\n");

        bool do_attach = false;
        bool do_detach = false;
        bool do_status = false;
        bool do_reset = false;
        bool do_dirty = false;

        size_t argidx;
        for (argidx = 1; argidx < args.size(); argidx++) {
            if (args[argidx] == "-attach") {
                do_attach = true;
                continue;
            }
            if (args[argidx] == "-detach") {
                do_detach = true;
                continue;
            }
            if (args[argidx] == "-status") {
                do_status = true;
                continue;
            }
            if (args[argidx] == "-reset") {
                do_reset = true;
                continue;
            }
            if (args[argidx] == "-dirty") {
                do_dirty = true;
                continue;
            }
            break;
        }
        extra_args(args, argidx, design);

        // Default action: attach and status
        if (!do_attach && !do_detach && !do_status && !do_reset && !do_dirty) {
            if (!zyphar_monitor.is_attached()) {
                do_attach = true;
            }
            do_status = true;
        }

        if (do_detach) {
            if (zyphar_monitor.is_attached()) {
                zyphar_monitor.detach();
                log("Monitor detached.\n");
            } else {
                log("Monitor not attached.\n");
            }
        }

        if (do_attach) {
            zyphar_monitor.attach(design);
        }

        if (do_reset) {
            if (zyphar_monitor.is_attached()) {
                zyphar_monitor.reset();
                log("Change tracking reset.\n");
            } else {
                log_warning("Monitor not attached.\n");
            }
        }

        if (do_status) {
            if (zyphar_monitor.is_attached()) {
                log("\nMonitor attached to design.\n");
                zyphar_monitor.log_changes();
            } else {
                log("\nMonitor not attached.\n");
            }
        }

        if (do_dirty) {
            if (zyphar_monitor.is_attached()) {
                auto dirty = zyphar_monitor.get_dirty_modules();
                if (dirty.empty()) {
                    log("No dirty modules.\n");
                } else {
                    log("Dirty modules (%zu):\n", dirty.size());
                    for (auto &m : dirty) {
                        log("  %s\n", log_id(m));
                    }
                }
            } else {
                log_warning("Monitor not attached.\n");
            }
        }
    }
} ZypharMonitorPass;

PRIVATE_NAMESPACE_END
