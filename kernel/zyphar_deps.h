/*
 * Zyphar Dependency Graph
 * Tracks module instantiation relationships for incremental synthesis
 */

#ifndef ZYPHAR_DEPS_H
#define ZYPHAR_DEPS_H

#include "kernel/yosys.h"
#include "kernel/rtlil.h"
#include <map>
#include <set>
#include <vector>
#include <string>

YOSYS_NAMESPACE_BEGIN

class ZypharDependencyGraph {
public:
    // Clear all dependency information
    void clear();

    // Build dependency graph from design
    void build_from_design(RTLIL::Design *design);

    // Get modules that directly instantiate the given module
    std::set<RTLIL::IdString> get_direct_dependents(RTLIL::IdString module_name) const;

    // Get all modules that depend on the given module (transitive closure)
    std::set<RTLIL::IdString> get_all_dependents(RTLIL::IdString module_name) const;

    // Get modules that are directly instantiated by the given module
    std::set<RTLIL::IdString> get_direct_dependencies(RTLIL::IdString module_name) const;

    // Get all modules that the given module depends on (transitive closure)
    std::set<RTLIL::IdString> get_all_dependencies(RTLIL::IdString module_name) const;

    // Given a set of changed modules, get all modules that need re-synthesis
    std::set<RTLIL::IdString> get_affected_modules(const std::set<RTLIL::IdString> &changed_modules) const;

    // Get topological order (dependencies before dependents)
    std::vector<RTLIL::IdString> get_topological_order() const;

    // Get reverse topological order (dependents before dependencies)
    std::vector<RTLIL::IdString> get_reverse_topological_order() const;

    // Serialize to JSON string (for storage in scratchpad)
    std::string to_json() const;

    // Deserialize from JSON string
    void from_json(const std::string &json);

    // Store in design's scratchpad
    void store_in_scratchpad(RTLIL::Design *design) const;

    // Load from design's scratchpad
    bool load_from_scratchpad(RTLIL::Design *design);

    // Check if graph is valid/built
    bool is_valid() const { return valid_; }

    // Get number of modules
    size_t module_count() const { return dependencies_.size(); }

    // Get all dependents map (for conservative invalidation)
    std::map<std::string, std::set<std::string>> get_all_dependents() const;

    // Debug: print the graph
    void log_graph() const;

private:
    // Module -> modules it instantiates (children)
    std::map<RTLIL::IdString, std::set<RTLIL::IdString>> dependencies_;

    // Module -> modules that instantiate it (parents)
    std::map<RTLIL::IdString, std::set<RTLIL::IdString>> dependents_;

    // All known modules
    std::set<RTLIL::IdString> all_modules_;

    // Whether the graph has been built
    bool valid_ = false;

    // Helper for transitive closure
    void collect_transitive(const std::map<RTLIL::IdString, std::set<RTLIL::IdString>> &graph,
                           RTLIL::IdString start,
                           std::set<RTLIL::IdString> &result) const;
};

// Global instance (can be accessed from passes)
extern ZypharDependencyGraph zyphar_deps;

YOSYS_NAMESPACE_END

#endif // ZYPHAR_DEPS_H
