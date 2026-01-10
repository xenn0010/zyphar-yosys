/*
 * Zyphar Change Monitor
 * Tracks design changes for incremental synthesis
 */

#ifndef ZYPHAR_MONITOR_H
#define ZYPHAR_MONITOR_H

#include "kernel/yosys.h"
#include "kernel/rtlil.h"
#include <set>
#include <map>

YOSYS_NAMESPACE_BEGIN

class ZypharChangeMonitor : public RTLIL::Monitor {
public:
    ZypharChangeMonitor();
    ~ZypharChangeMonitor();

    // Install monitor on a design
    void attach(RTLIL::Design *design);

    // Remove monitor from design
    void detach();

    // Reset change tracking (call after processing changes)
    void reset();

    // Query changes
    bool has_changes() const;
    const std::set<RTLIL::IdString>& get_added_modules() const { return added_modules_; }
    const std::set<RTLIL::IdString>& get_deleted_modules() const { return deleted_modules_; }
    const std::set<RTLIL::IdString>& get_modified_modules() const { return modified_modules_; }

    // Get all modules that need re-synthesis
    std::set<RTLIL::IdString> get_dirty_modules() const;

    // Check if a specific module changed
    bool is_dirty(RTLIL::IdString module_name) const;

    // Get change summary for logging
    void log_changes() const;

    // Monitor callbacks
    void notify_module_add(RTLIL::Module *module) override;
    void notify_module_del(RTLIL::Module *module) override;

    void notify_connect(RTLIL::Cell *cell, RTLIL::IdString port,
                       const RTLIL::SigSpec &old_sig, const RTLIL::SigSpec &sig) override;
    void notify_connect(RTLIL::Module *module, const RTLIL::SigSig &sigsig) override;
    void notify_connect(RTLIL::Module *module, const std::vector<RTLIL::SigSig> &sigsig_vec) override;

    void notify_blackout(RTLIL::Module *module) override;

    // Is the monitor currently attached?
    bool is_attached() const { return design_ != nullptr; }

    // Get attached design
    RTLIL::Design* get_design() const { return design_; }

private:
    RTLIL::Design *design_ = nullptr;

    // Track changes
    std::set<RTLIL::IdString> added_modules_;
    std::set<RTLIL::IdString> deleted_modules_;
    std::set<RTLIL::IdString> modified_modules_;

    // Track original hashes for comparison
    std::map<RTLIL::IdString, uint64_t> original_hashes_;

    // Mark a module as modified
    void mark_modified(RTLIL::Module *module);
};

// Global instance
extern ZypharChangeMonitor zyphar_monitor;

YOSYS_NAMESPACE_END

#endif // ZYPHAR_MONITOR_H
