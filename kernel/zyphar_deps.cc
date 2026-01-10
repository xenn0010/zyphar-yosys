/*
 * Zyphar Dependency Graph Implementation
 */

#include "kernel/zyphar_deps.h"
#include "kernel/log.h"
#include <algorithm>
#include <queue>
#include <sstream>

YOSYS_NAMESPACE_BEGIN

// Global instance
ZypharDependencyGraph zyphar_deps;

void ZypharDependencyGraph::clear()
{
    dependencies_.clear();
    dependents_.clear();
    all_modules_.clear();
    valid_ = false;
}

void ZypharDependencyGraph::build_from_design(RTLIL::Design *design)
{
    clear();

    // First pass: collect all modules
    for (auto module : design->modules()) {
        all_modules_.insert(module->name);
        dependencies_[module->name] = std::set<RTLIL::IdString>();
        dependents_[module->name] = std::set<RTLIL::IdString>();
    }

    // Second pass: build dependency relationships
    for (auto module : design->modules()) {
        for (auto cell : module->cells()) {
            RTLIL::IdString cell_type = cell->type;

            // Skip primitive cells (those starting with $)
            if (cell_type[0] == '$')
                continue;

            // Check if this cell type is a module in the design
            if (all_modules_.count(cell_type)) {
                // module depends on cell_type (instantiates it)
                dependencies_[module->name].insert(cell_type);
                // cell_type is depended upon by module
                dependents_[cell_type].insert(module->name);
            }
        }
    }

    valid_ = true;
}

std::set<RTLIL::IdString> ZypharDependencyGraph::get_direct_dependents(RTLIL::IdString module_name) const
{
    auto it = dependents_.find(module_name);
    if (it != dependents_.end())
        return it->second;
    return std::set<RTLIL::IdString>();
}

std::set<RTLIL::IdString> ZypharDependencyGraph::get_all_dependents(RTLIL::IdString module_name) const
{
    std::set<RTLIL::IdString> result;
    collect_transitive(dependents_, module_name, result);
    return result;
}

std::set<RTLIL::IdString> ZypharDependencyGraph::get_direct_dependencies(RTLIL::IdString module_name) const
{
    auto it = dependencies_.find(module_name);
    if (it != dependencies_.end())
        return it->second;
    return std::set<RTLIL::IdString>();
}

std::set<RTLIL::IdString> ZypharDependencyGraph::get_all_dependencies(RTLIL::IdString module_name) const
{
    std::set<RTLIL::IdString> result;
    collect_transitive(dependencies_, module_name, result);
    return result;
}

std::set<RTLIL::IdString> ZypharDependencyGraph::get_affected_modules(const std::set<RTLIL::IdString> &changed_modules) const
{
    std::set<RTLIL::IdString> affected;

    // Start with the changed modules themselves
    for (auto &mod : changed_modules) {
        affected.insert(mod);
        // Add all modules that depend on this one
        auto deps = get_all_dependents(mod);
        affected.insert(deps.begin(), deps.end());
    }

    return affected;
}

void ZypharDependencyGraph::collect_transitive(
    const std::map<RTLIL::IdString, std::set<RTLIL::IdString>> &graph,
    RTLIL::IdString start,
    std::set<RTLIL::IdString> &result) const
{
    std::queue<RTLIL::IdString> worklist;

    auto it = graph.find(start);
    if (it == graph.end())
        return;

    for (auto &next : it->second) {
        if (result.insert(next).second) {
            worklist.push(next);
        }
    }

    while (!worklist.empty()) {
        RTLIL::IdString current = worklist.front();
        worklist.pop();

        auto current_it = graph.find(current);
        if (current_it == graph.end())
            continue;

        for (auto &next : current_it->second) {
            if (result.insert(next).second) {
                worklist.push(next);
            }
        }
    }
}

std::vector<RTLIL::IdString> ZypharDependencyGraph::get_topological_order() const
{
    std::vector<RTLIL::IdString> result;
    std::set<RTLIL::IdString> visited;
    std::set<RTLIL::IdString> in_stack;

    std::function<void(RTLIL::IdString)> visit = [&](RTLIL::IdString mod) {
        if (visited.count(mod))
            return;
        if (in_stack.count(mod)) {
            log_warning("Circular dependency detected involving module %s\n", log_id(mod));
            return;
        }

        in_stack.insert(mod);

        auto it = dependencies_.find(mod);
        if (it != dependencies_.end()) {
            for (auto &dep : it->second) {
                visit(dep);
            }
        }

        in_stack.erase(mod);
        visited.insert(mod);
        result.push_back(mod);
    };

    for (auto &mod : all_modules_) {
        visit(mod);
    }

    return result;
}

std::vector<RTLIL::IdString> ZypharDependencyGraph::get_reverse_topological_order() const
{
    auto order = get_topological_order();
    std::reverse(order.begin(), order.end());
    return order;
}

std::map<std::string, std::set<std::string>> ZypharDependencyGraph::get_all_dependents() const
{
    std::map<std::string, std::set<std::string>> result;

    for (auto &pair : dependents_) {
        std::set<std::string> deps_str;
        for (auto &dep : pair.second) {
            deps_str.insert(dep.str());
        }
        result[pair.first.str()] = deps_str;
    }

    return result;
}

std::string ZypharDependencyGraph::to_json() const
{
    std::ostringstream json;
    json << "{\n";
    json << "  \"modules\": [\n";

    bool first_mod = true;
    for (auto &mod : all_modules_) {
        if (!first_mod) json << ",\n";
        first_mod = false;

        json << "    {\n";
        json << "      \"name\": \"" << mod.c_str() << "\",\n";
        json << "      \"dependencies\": [";

        auto it = dependencies_.find(mod);
        if (it != dependencies_.end()) {
            bool first_dep = true;
            for (auto &dep : it->second) {
                if (!first_dep) json << ", ";
                first_dep = false;
                json << "\"" << dep.c_str() << "\"";
            }
        }
        json << "],\n";

        json << "      \"dependents\": [";
        auto dep_it = dependents_.find(mod);
        if (dep_it != dependents_.end()) {
            bool first_dep = true;
            for (auto &dep : dep_it->second) {
                if (!first_dep) json << ", ";
                first_dep = false;
                json << "\"" << dep.c_str() << "\"";
            }
        }
        json << "]\n";
        json << "    }";
    }

    json << "\n  ]\n";
    json << "}\n";
    return json.str();
}

void ZypharDependencyGraph::from_json(const std::string &json)
{
    clear();

    // Simple JSON parsing (not using external library)
    // Format: {"modules": [{"name": "...", "dependencies": [...], "dependents": [...]}]}
    size_t pos = 0;

    auto skip_whitespace = [&]() {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\r' || json[pos] == '\t'))
            pos++;
    };

    auto expect = [&](char c) {
        skip_whitespace();
        if (pos < json.size() && json[pos] == c) {
            pos++;
            return true;
        }
        return false;
    };

    auto parse_string = [&]() -> std::string {
        skip_whitespace();
        if (!expect('"')) return "";
        std::string result;
        while (pos < json.size() && json[pos] != '"') {
            result += json[pos++];
        }
        expect('"');
        return result;
    };

    auto parse_string_array = [&]() -> std::set<RTLIL::IdString> {
        std::set<RTLIL::IdString> result;
        skip_whitespace();
        if (!expect('[')) return result;

        while (true) {
            skip_whitespace();
            if (json[pos] == ']') {
                pos++;
                break;
            }
            std::string s = parse_string();
            if (!s.empty()) {
                result.insert(RTLIL::IdString(s));
            }
            skip_whitespace();
            if (json[pos] == ',') pos++;
        }
        return result;
    };

    // Find "modules" array
    size_t modules_pos = json.find("\"modules\"");
    if (modules_pos == std::string::npos) return;

    pos = modules_pos + 9; // Skip "modules"
    skip_whitespace();
    expect(':');
    skip_whitespace();
    expect('[');

    while (true) {
        skip_whitespace();
        if (json[pos] == ']') break;
        if (!expect('{')) break;

        std::string name;
        std::set<RTLIL::IdString> deps;
        std::set<RTLIL::IdString> dep_of;

        while (true) {
            skip_whitespace();
            if (json[pos] == '}') {
                pos++;
                break;
            }

            std::string key = parse_string();
            skip_whitespace();
            expect(':');

            if (key == "name") {
                name = parse_string();
            } else if (key == "dependencies") {
                deps = parse_string_array();
            } else if (key == "dependents") {
                dep_of = parse_string_array();
            }

            skip_whitespace();
            if (json[pos] == ',') pos++;
        }

        if (!name.empty()) {
            RTLIL::IdString mod_name(name);
            all_modules_.insert(mod_name);
            dependencies_[mod_name] = deps;
            dependents_[mod_name] = dep_of;
        }

        skip_whitespace();
        if (json[pos] == ',') pos++;
    }

    valid_ = !all_modules_.empty();
}

void ZypharDependencyGraph::store_in_scratchpad(RTLIL::Design *design) const
{
    design->scratchpad_set_string("zyphar.deps.json", to_json());
}

bool ZypharDependencyGraph::load_from_scratchpad(RTLIL::Design *design)
{
    std::string json = design->scratchpad_get_string("zyphar.deps.json");
    if (json.empty())
        return false;

    from_json(json);
    return valid_;
}

void ZypharDependencyGraph::log_graph() const
{
    log("\n=== Zyphar Dependency Graph ===\n\n");

    for (auto &mod : all_modules_) {
        log("Module: %s\n", log_id(mod));

        log("  Instantiates:");
        auto deps = dependencies_.find(mod);
        if (deps != dependencies_.end() && !deps->second.empty()) {
            for (auto &d : deps->second)
                log(" %s", log_id(d));
        } else {
            log(" (none)");
        }
        log("\n");

        log("  Instantiated by:");
        auto dep_of = dependents_.find(mod);
        if (dep_of != dependents_.end() && !dep_of->second.empty()) {
            for (auto &d : dep_of->second)
                log(" %s", log_id(d));
        } else {
            log(" (none)");
        }
        log("\n\n");
    }

    log("Topological order: ");
    auto order = get_topological_order();
    for (size_t i = 0; i < order.size(); i++) {
        if (i > 0) log(" -> ");
        log("%s", log_id(order[i]));
    }
    log("\n\n");
}

YOSYS_NAMESPACE_END
