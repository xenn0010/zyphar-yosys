/*
 *  Zyphar Passes - Intent Preservation for EDA
 *
 *  Commands:
 *  - zyphar_mark_critical: Mark cells as timing-critical
 *  - zyphar_show_intent: Show which cells have intent markers
 */

#include "kernel/yosys.h"
#include "kernel/rtlil.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

// Command to mark cells as critical
struct ZypharMarkCriticalPass : public Pass {
    ZypharMarkCriticalPass() : Pass("zyphar_mark_critical", "mark cells as timing-critical") { }

    void help() override
    {
        log("\n");
        log("    zyphar_mark_critical [options]\n");
        log("\n");
        log("Mark selected cells as timing-critical. This intent survives\n");
        log("through synthesis transforms and can be read by physical tools.\n");
        log("\n");
        log("    -priority <n>\n");
        log("        Set priority level (default: 10, higher = more important)\n");
        log("\n");
        log("    -preserve\n");
        log("        Also mark cells to preserve structure (don't optimize away)\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing ZYPHAR_MARK_CRITICAL pass.\n");

        int priority = 10;
        bool preserve = false;

        for (size_t i = 1; i < args.size(); i++) {
            if (args[i] == "-priority" && i + 1 < args.size()) {
                priority = atoi(args[++i].c_str());
            }
            else if (args[i] == "-preserve") {
                preserve = true;
            }
        }

        int count = 0;
        for (auto module : design->selected_modules())
        {
            for (auto cell : module->selected_cells())
            {
                cell->zyphar_intent.is_timing_critical = true;
                cell->zyphar_intent.priority = priority;
                if (preserve)
                    cell->zyphar_intent.preserve_structure = true;
                count++;
            }
        }
        log("Marked %d cells as timing-critical (priority=%d%s)\n",
            count, priority, preserve ? ", preserved" : "");
    }
} ZypharMarkCriticalPass;

// Command to show intent markers
struct ZypharShowIntentPass : public Pass {
    ZypharShowIntentPass() : Pass("zyphar_show_intent", "show Zyphar intent markers") { }

    void help() override
    {
        log("\n");
        log("    zyphar_show_intent\n");
        log("\n");
        log("Display all cells that have Zyphar intent markers set.\n");
        log("\n");
    }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "ZYPHAR Intent Report\n");

        for (auto module : design->selected_modules())
        {
            log("\nModule: %s\n", log_id(module));

            int critical_count = 0;
            int preserved_count = 0;
            int total_cells = 0;

            for (auto cell : module->cells())
            {
                total_cells++;
                bool has_intent = false;

                if (cell->zyphar_intent.is_timing_critical) {
                    log("  [CRITICAL p=%d] %s (%s)\n",
                        cell->zyphar_intent.priority,
                        log_id(cell),
                        log_id(cell->type));
                    critical_count++;
                    has_intent = true;
                }

                if (cell->zyphar_intent.preserve_structure) {
                    if (!has_intent)
                        log("  [PRESERVE] %s (%s)\n", log_id(cell), log_id(cell->type));
                    preserved_count++;
                }
            }

            log("\nSummary: %d cells total, %d critical, %d preserved\n",
                total_cells, critical_count, preserved_count);
        }
    }
} ZypharShowIntentPass;

// Command to clear intent markers
struct ZypharClearIntentPass : public Pass {
    ZypharClearIntentPass() : Pass("zyphar_clear_intent", "clear Zyphar intent markers") { }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Clearing ZYPHAR intent markers.\n");

        int count = 0;
        for (auto module : design->selected_modules())
        {
            for (auto cell : module->selected_cells())
            {
                if (cell->zyphar_intent.is_timing_critical ||
                    cell->zyphar_intent.preserve_structure ||
                    cell->zyphar_intent.priority > 0) {
                    cell->zyphar_intent.is_timing_critical = false;
                    cell->zyphar_intent.preserve_structure = false;
                    cell->zyphar_intent.priority = 0;
                    count++;
                }
            }
        }
        log("Cleared intent from %d cells\n", count);
    }
} ZypharClearIntentPass;

PRIVATE_NAMESPACE_END
