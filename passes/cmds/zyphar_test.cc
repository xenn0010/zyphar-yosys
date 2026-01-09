/*
 *  Zyphar Test Pass - Demonstrates unified IR extensions
 *
 *  This pass shows how to use the new Zyphar extensions:
 *  - ZypharPhysicalHint: ML-predicted timing/area/power
 *  - ZypharIntent: Designer intent preservation
 *  - ZypharGradient: Differentiable optimization
 */

#include "kernel/yosys.h"
#include "kernel/rtlil.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct ZypharTestPass : public Pass {
    ZypharTestPass() : Pass("zyphar_test", "test Zyphar unified IR extensions") { }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing ZYPHAR_TEST pass.\n");

        // Iterate through all modules
        for (auto module : design->selected_modules())
        {
            log("Processing module: %s\n", log_id(module));

            // Demo: Set module-level Zyphar metrics
            module->zyphar_metrics.total_area_um2 = 0.0f;
            module->zyphar_metrics.critical_path_ps = 0.0f;

            int cell_count = 0;

            // Iterate through cells and set Zyphar extensions
            for (auto cell : module->selected_cells())
            {
                cell_count++;

                // Simulate ML prediction for this cell
                // In real implementation, this would call an ML model
                float predicted_delay = 50.0f + (cell_count * 10.0f);  // Fake prediction
                float predicted_area = 1.0f + (cell_count * 0.5f);

                // Set physical hints
                cell->zyphar_physical.estimated_delay_ps = predicted_delay;
                cell->zyphar_physical.estimated_area_um2 = predicted_area;
                cell->zyphar_physical.estimated_power_uw = predicted_delay * 0.01f;
                cell->zyphar_physical.confidence = 0.95f;

                // Accumulate module metrics
                module->zyphar_metrics.total_area_um2 += predicted_area;
                if (predicted_delay > module->zyphar_metrics.critical_path_ps)
                    module->zyphar_metrics.critical_path_ps = predicted_delay;

                // Check if cell has timing-critical intent
                if (cell->zyphar_intent.is_timing_critical) {
                    log("  Cell %s marked as TIMING CRITICAL (priority=%d)\n",
                        log_id(cell), cell->zyphar_intent.priority);
                }

                // Demo: Set gradient (as if from optimization)
                cell->zyphar_grad.d_cost_d_size = 0.1f;
                cell->zyphar_grad.needs_update = true;

                log("  Cell %s: delay=%.1fps, area=%.2fum², confidence=%.0f%%\n",
                    log_id(cell),
                    cell->zyphar_physical.estimated_delay_ps,
                    cell->zyphar_physical.estimated_area_um2,
                    cell->zyphar_physical.confidence * 100);
            }

            // Report module-level predictions
            module->zyphar_metrics.confidence = 0.90f;

            log("\n=== ZYPHAR MODULE SUMMARY ===\n");
            log("  Total cells: %d\n", cell_count);
            log("  Predicted area: %.2f um²\n", module->zyphar_metrics.total_area_um2);
            log("  Predicted critical path: %.1f ps\n", module->zyphar_metrics.critical_path_ps);
            log("  Prediction confidence: %.0f%%\n", module->zyphar_metrics.confidence * 100);
            log("=============================\n\n");
        }
    }
} ZypharTestPass;

// Command to mark cells as critical
struct ZypharMarkCriticalPass : public Pass {
    ZypharMarkCriticalPass() : Pass("zyphar_mark_critical", "mark cells as timing-critical") { }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "Executing ZYPHAR_MARK_CRITICAL pass.\n");

        int priority = 10;  // Default high priority

        // Parse arguments
        for (size_t i = 1; i < args.size(); i++) {
            if (args[i] == "-priority" && i + 1 < args.size()) {
                priority = atoi(args[++i].c_str());
            }
        }

        for (auto module : design->selected_modules())
        {
            for (auto cell : module->selected_cells())
            {
                cell->zyphar_intent.is_timing_critical = true;
                cell->zyphar_intent.priority = priority;
                log("Marked cell %s as timing-critical (priority=%d)\n",
                    log_id(cell), priority);
            }
        }
    }
} ZypharMarkCriticalPass;

// Command to show Zyphar predictions
struct ZypharShowPass : public Pass {
    ZypharShowPass() : Pass("zyphar_show", "show Zyphar predictions for design") { }

    void execute(std::vector<std::string> args, RTLIL::Design *design) override
    {
        log_header(design, "ZYPHAR Design Predictions\n");

        for (auto module : design->selected_modules())
        {
            log("\nModule: %s\n", log_id(module));
            log("├─ Predicted Area: %.2f um²\n", module->zyphar_metrics.total_area_um2);
            log("├─ Predicted Critical Path: %.1f ps\n", module->zyphar_metrics.critical_path_ps);
            log("├─ Predicted Power: %.2f mW\n", module->zyphar_metrics.total_power_mw);
            log("├─ Routability Score: %.2f\n", module->zyphar_metrics.routability_score);
            log("└─ Confidence: %.0f%%\n", module->zyphar_metrics.confidence * 100);

            // Show critical paths if any
            if (!module->zyphar_critical_paths.empty()) {
                log("\nCritical Paths:\n");
                for (const auto& path : module->zyphar_critical_paths) {
                    log("  %s -> %s: %.1f ps (slack: %.1f ps)\n",
                        log_id(path.startpoint), log_id(path.endpoint),
                        path.path_delay_ps, path.slack_ps);
                }
            }
        }
    }
} ZypharShowPass;

PRIVATE_NAMESPACE_END
