/*
 * Zyphar Change Monitor Implementation
 */

#include "kernel/zyphar_monitor.h"
#include "kernel/log.h"

YOSYS_NAMESPACE_BEGIN

// Global instance
ZypharChangeMonitor zyphar_monitor;

ZypharChangeMonitor::ZypharChangeMonitor()
{
}

ZypharChangeMonitor::~ZypharChangeMonitor()
{
    // Safety: only detach if design is still valid
    // The design might already be destroyed during program exit
    if (design_) {
        try {
            // Check if design's monitors set still contains us
            if (design_->monitors.count(this) > 0) {
                design_->monitors.erase(this);
            }
        } catch (...) {
            // Silently ignore - design may be in invalid state during shutdown
        }
        design_ = nullptr;
    }
}

void ZypharChangeMonitor::attach(RTLIL::Design *design)
{
    if (design_) {
        detach();
    }

    design_ = design;
    design->monitors.insert(this);

    // Record initial hashes
    original_hashes_.clear();
    for (auto module : design->modules()) {
        original_hashes_[module->name] = module->get_content_hash();
    }

    reset();

    log("Zyphar change monitor attached to design (%zu modules)\n", original_hashes_.size());
}

void ZypharChangeMonitor::detach()
{
    if (design_) {
        design_->monitors.erase(this);
        design_ = nullptr;
    }
    original_hashes_.clear();
}

void ZypharChangeMonitor::reset()
{
    added_modules_.clear();
    deleted_modules_.clear();
    modified_modules_.clear();

    // Update original hashes to current state
    if (design_) {
        original_hashes_.clear();
        for (auto module : design_->modules()) {
            original_hashes_[module->name] = module->get_content_hash();
        }
    }
}

bool ZypharChangeMonitor::has_changes() const
{
    return !added_modules_.empty() || !deleted_modules_.empty() || !modified_modules_.empty();
}

std::set<RTLIL::IdString> ZypharChangeMonitor::get_dirty_modules() const
{
    std::set<RTLIL::IdString> dirty;

    // Add all changed modules
    dirty.insert(added_modules_.begin(), added_modules_.end());
    dirty.insert(modified_modules_.begin(), modified_modules_.end());

    // Note: deleted modules aren't dirty, they're gone

    return dirty;
}

bool ZypharChangeMonitor::is_dirty(RTLIL::IdString module_name) const
{
    return added_modules_.count(module_name) ||
           modified_modules_.count(module_name);
}

void ZypharChangeMonitor::log_changes() const
{
    log("\n=== Zyphar Change Summary ===\n");

    if (!has_changes()) {
        log("  No changes detected.\n");
        log("\n");
        return;
    }

    if (!added_modules_.empty()) {
        log("  Added modules:\n");
        for (auto &m : added_modules_) {
            log("    + %s\n", log_id(m));
        }
    }

    if (!deleted_modules_.empty()) {
        log("  Deleted modules:\n");
        for (auto &m : deleted_modules_) {
            log("    - %s\n", log_id(m));
        }
    }

    if (!modified_modules_.empty()) {
        log("  Modified modules:\n");
        for (auto &m : modified_modules_) {
            log("    ~ %s\n", log_id(m));
        }
    }

    log("\n");
}

void ZypharChangeMonitor::notify_module_add(RTLIL::Module *module)
{
    // If it was deleted earlier in this session, just mark as modified
    if (deleted_modules_.erase(module->name) > 0) {
        modified_modules_.insert(module->name);
    } else {
        added_modules_.insert(module->name);
    }

    log_debug("Zyphar: module added: %s\n", log_id(module->name));
}

void ZypharChangeMonitor::notify_module_del(RTLIL::Module *module)
{
    // If it was added earlier in this session, just remove from added
    if (added_modules_.erase(module->name) > 0) {
        // Transient module - no change
    } else {
        deleted_modules_.insert(module->name);
        modified_modules_.erase(module->name);
    }

    log_debug("Zyphar: module deleted: %s\n", log_id(module->name));
}

void ZypharChangeMonitor::mark_modified(RTLIL::Module *module)
{
    // Don't mark as modified if it was just added
    if (added_modules_.count(module->name) == 0) {
        modified_modules_.insert(module->name);
    }

    // Invalidate the content hash
    module->invalidate_content_hash();

    log_debug("Zyphar: module modified: %s\n", log_id(module->name));
}

void ZypharChangeMonitor::notify_connect(RTLIL::Cell *cell, RTLIL::IdString port,
                                        const RTLIL::SigSpec &old_sig, const RTLIL::SigSpec &sig)
{
    (void)port;
    (void)old_sig;
    (void)sig;

    if (cell->module) {
        mark_modified(cell->module);
    }
}

void ZypharChangeMonitor::notify_connect(RTLIL::Module *module, const RTLIL::SigSig &sigsig)
{
    (void)sigsig;
    mark_modified(module);
}

void ZypharChangeMonitor::notify_connect(RTLIL::Module *module, const std::vector<RTLIL::SigSig> &sigsig_vec)
{
    (void)sigsig_vec;
    mark_modified(module);
}

void ZypharChangeMonitor::notify_blackout(RTLIL::Module *module)
{
    // Module is being completely rewritten
    mark_modified(module);
}

YOSYS_NAMESPACE_END
