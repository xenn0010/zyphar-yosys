/*
 * Zyphar Module Cache Implementation
 */

#include "kernel/zyphar_cache.h"
#include "kernel/log.h"
#include "kernel/json.h"
#include "backends/rtlil/rtlil_backend.h"
#include "libs/json11/json11.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/stat.h>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

YOSYS_NAMESPACE_BEGIN

// Global instance
ZypharModuleCache zyphar_cache;

ZypharModuleCache::ZypharModuleCache()
{
}

ZypharModuleCache::~ZypharModuleCache()
{
    // Safety: save cache on destruction if needed
    if (initialized_ && dirty_) {
        try {
            save_to_disk();
        } catch (...) {
            // Silently ignore - may be during program shutdown
        }
    }
}

bool ZypharModuleCache::init(const std::string &cache_dir)
{
    if (cache_dir.empty()) {
        // Default to ~/.cache/zyphar
        const char *home = getenv("HOME");
        if (home) {
            cache_dir_ = std::string(home) + "/.cache/zyphar";
        } else {
            cache_dir_ = "/tmp/zyphar_cache";
        }
    } else {
        cache_dir_ = cache_dir;
    }

    // Create directory structure
    struct stat st;
    if (stat(cache_dir_.c_str(), &st) != 0) {
        // Create parent .cache directory first
        std::string parent = cache_dir_.substr(0, cache_dir_.rfind('/'));
        if (stat(parent.c_str(), &st) != 0) {
            mkdir(parent.c_str(), 0755);
        }
        if (mkdir(cache_dir_.c_str(), 0755) != 0) {
            log_warning("Failed to create cache directory: %s\n", cache_dir_.c_str());
            return false;
        }
    }

    // Create modules subdirectory
    std::string modules_dir = cache_dir_ + "/modules";
    if (stat(modules_dir.c_str(), &st) != 0) {
        mkdir(modules_dir.c_str(), 0755);
    }

    // Load existing cache
    load_from_disk();

    initialized_ = true;
    log("Zyphar cache initialized at: %s (%zu entries)\n", cache_dir_.c_str(), entries_.size());
    return true;
}

std::string ZypharModuleCache::make_key(const std::string &module_name, uint64_t hash, const std::string &pass_seq) const
{
    std::ostringstream key;
    key << module_name << "|" << std::hex << hash << "|" << pass_seq;
    return key.str();
}

std::string ZypharModuleCache::get_index_path() const
{
    return cache_dir_ + "/index.json";
}

std::string ZypharModuleCache::get_module_path(const std::string &key) const
{
    // Hash the key to get a safe filename
    uint64_t hash = 0;
    for (char c : key) {
        hash = hash * 31 + c;
    }

    std::ostringstream path;
    path << cache_dir_ << "/modules/" << std::hex << hash << ".json";
    return path.str();
}

bool ZypharModuleCache::has(const std::string &module_name, uint64_t hash, const std::string &pass_seq) const
{
    std::string key = make_key(module_name, hash, pass_seq);
    bool found = entries_.count(key) > 0;

    if (found) {
        total_hits_++;
    } else {
        total_misses_++;
    }

    return found;
}

const ZypharCacheEntry* ZypharModuleCache::get(const std::string &module_name, uint64_t hash, const std::string &pass_seq) const
{
    std::string key = make_key(module_name, hash, pass_seq);
    auto it = entries_.find(key);

    if (it != entries_.end()) {
        total_hits_++;
        const_cast<ZypharCacheEntry*>(&it->second)->hit_count++;
        return &it->second;
    }

    total_misses_++;
    return nullptr;
}

bool ZypharModuleCache::put(const std::string &module_name, uint64_t hash, const std::string &pass_seq,
                            RTLIL::Module *module, RTLIL::Design *design)
{
    if (!module) {
        log_warning("Cannot cache null module: %s\n", module_name.c_str());
        return false;
    }

    std::string key = make_key(module_name, hash, pass_seq);

    ZypharCacheEntry entry;
    entry.module_name = module_name;
    entry.content_hash = hash;
    entry.pass_sequence = pass_seq;
    entry.timestamp = time(nullptr);
    entry.hit_count = 0;

    try {
        entry.json_data = serialize_module(module, design);
    } catch (const std::exception &e) {
        log_warning("Failed to serialize module %s: %s\n", module_name.c_str(), e.what());
        return false;
    } catch (...) {
        log_warning("Failed to serialize module %s: unknown error\n", module_name.c_str());
        return false;
    }

    if (entry.json_data.empty()) {
        log_warning("Serialization produced empty data for module %s\n", module_name.c_str());
        return false;
    }

    entries_[key] = entry;

    // Also cache in memory
    module_json_cache_[key] = entry.json_data;

    dirty_ = true;

    log("Cached module %s (hash: 0x%016llx, pass: %s, size: %zu bytes)\n",
        module_name.c_str(), (unsigned long long)hash, pass_seq.c_str(), entry.json_data.size());

    // Check if we need to evict old entries
    evict_if_needed();

    return true;
}

bool ZypharModuleCache::restore(const std::string &module_name, uint64_t hash, const std::string &pass_seq,
                               RTLIL::Design *design)
{
    std::string key = make_key(module_name, hash, pass_seq);
    auto it = entries_.find(key);

    if (it == entries_.end()) {
        return false;
    }

    // Check memory cache first
    auto mem_it = module_json_cache_.find(key);
    std::string json_data;
    if (mem_it != module_json_cache_.end()) {
        json_data = mem_it->second;
    } else {
        json_data = it->second.json_data;
        // If empty, try loading from disk
        if (json_data.empty()) {
            std::string path = get_module_path(key);
            std::ifstream f(path);
            if (f.good()) {
                std::ostringstream ss;
                ss << f.rdbuf();
                json_data = ss.str();
            }
        }
        module_json_cache_[key] = json_data;
    }

    if (json_data.empty()) {
        log_warning("Cache entry exists but module data is empty: %s\n", key.c_str());
        return false;
    }

    return deserialize_module(json_data, design, module_name);
}

std::string ZypharModuleCache::serialize_module(RTLIL::Module *module, RTLIL::Design *design) const
{
    (void)design;  // Unused but kept for API compatibility

    if (!module) {
        throw std::runtime_error("Cannot serialize null module");
    }

    std::ostringstream out;

    // Create a temporary design with just this module
    RTLIL::Design temp_design;
    RTLIL::Module *temp_mod = nullptr;

    try {
        temp_mod = module->clone();
        if (!temp_mod) {
            throw std::runtime_error("Module clone returned null");
        }
        temp_design.add(temp_mod);

        // Use RTLIL backend to serialize
        RTLIL_BACKEND::dump_module(out, "", temp_mod, &temp_design, false, false);
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("Serialization failed: ") + e.what());
    }

    std::string result = out.str();
    if (result.empty()) {
        throw std::runtime_error("Serialization produced empty output");
    }

    return result;
}

bool ZypharModuleCache::deserialize_module(const std::string &rtlil_data, RTLIL::Design *design, const std::string &module_name)
{
    if (rtlil_data.empty()) {
        log_warning("Cannot deserialize empty RTLIL data for module: %s\n", module_name.c_str());
        return false;
    }

    if (!design) {
        log_warning("Cannot deserialize to null design: %s\n", module_name.c_str());
        return false;
    }

    // Write to temp file and use RTLIL frontend
    std::string temp_file = cache_dir_ + "/temp_restore_" + std::to_string(getpid()) + ".rtlil";

    // Write the RTLIL data to temp file
    {
        std::ofstream f(temp_file);
        if (!f.good()) {
            log_warning("Failed to create temp file for module restore: %s\n", temp_file.c_str());
            return false;
        }
        f << "autoidx 1\n";  // RTLIL header
        f << rtlil_data;
        f.flush();
        if (f.fail()) {
            log_warning("Failed to write temp file for module restore: %s\n", temp_file.c_str());
            unlink(temp_file.c_str());
            return false;
        }
    }

    // Parse the RTLIL
    bool success = false;
    try {
        Frontend::frontend_call(design, nullptr, temp_file, "rtlil");
        success = true;
        log_debug("Successfully restored module %s from cache\n", module_name.c_str());
    } catch (const std::exception &e) {
        log_warning("Failed to restore module %s from cache: %s\n", module_name.c_str(), e.what());
    } catch (...) {
        log_warning("Failed to restore module %s from cache: unknown error\n", module_name.c_str());
    }

    // Always clean up temp file
    unlink(temp_file.c_str());

    return success;
}

void ZypharModuleCache::invalidate(const std::string &module_name)
{
    std::vector<std::string> to_remove;

    for (auto &it : entries_) {
        if (it.second.module_name == module_name) {
            to_remove.push_back(it.first);
        }
    }

    for (auto &key : to_remove) {
        entries_.erase(key);
        module_json_cache_.erase(key);
    }

    if (!to_remove.empty()) {
        dirty_ = true;
        log("Invalidated %zu cache entries for module %s\n", to_remove.size(), module_name.c_str());
    }
}

void ZypharModuleCache::invalidate(const std::string &module_name, uint64_t hash, const std::string &pass_seq)
{
    std::string key = make_key(module_name, hash, pass_seq);
    if (entries_.erase(key) > 0) {
        module_json_cache_.erase(key);
        dirty_ = true;
    }
}

void ZypharModuleCache::invalidate_affected(const std::set<std::string> &changed_modules,
                                           const std::map<std::string, std::set<std::string>> &dependents)
{
    std::set<std::string> to_invalidate = changed_modules;

    // Collect all affected modules (transitive)
    std::vector<std::string> worklist(changed_modules.begin(), changed_modules.end());
    while (!worklist.empty()) {
        std::string mod = worklist.back();
        worklist.pop_back();

        auto it = dependents.find(mod);
        if (it != dependents.end()) {
            for (auto &dep : it->second) {
                if (to_invalidate.insert(dep).second) {
                    worklist.push_back(dep);
                }
            }
        }
    }

    // Invalidate all affected modules
    for (auto &mod : to_invalidate) {
        invalidate(mod);
    }
}

void ZypharModuleCache::clear()
{
    entries_.clear();
    module_json_cache_.clear();
    total_hits_ = 0;
    total_misses_ = 0;
    dirty_ = true;
}

size_t ZypharModuleCache::total_size_bytes() const
{
    size_t total = 0;
    for (const auto &it : entries_) {
        total += it.second.json_data.size();
        total += it.first.size();  // Key size
        total += it.second.module_name.size();
        total += it.second.pass_sequence.size();
    }
    return total;
}

void ZypharModuleCache::evict_if_needed()
{
    // Evict by age first
    evict_by_age();

    // Then evict by count if still over limit
    if (entries_.size() > max_entries_) {
        size_t to_evict = entries_.size() - max_entries_;
        log("Cache over entry limit (%zu > %zu), evicting %zu entries\n",
            entries_.size(), max_entries_, to_evict);
        evict_oldest(to_evict);
    }

    // Then evict by size if still over limit
    size_t current_size = total_size_bytes();
    if (current_size > max_size_bytes_) {
        log("Cache over size limit (%zu > %zu bytes)\n", current_size, max_size_bytes_);
        // Evict entries until under limit, starting with least used
        while (current_size > max_size_bytes_ && !entries_.empty()) {
            evict_oldest(1);
            current_size = total_size_bytes();
        }
    }
}

void ZypharModuleCache::evict_oldest(size_t count)
{
    if (count == 0 || entries_.empty()) return;

    // Build a sorted list by (hit_count, timestamp) - least used/oldest first
    std::vector<std::pair<std::string, std::pair<size_t, time_t>>> sorted_entries;
    for (const auto &it : entries_) {
        sorted_entries.push_back({it.first, {it.second.hit_count, it.second.timestamp}});
    }

    // Sort by hit_count ascending, then timestamp ascending (oldest first)
    std::sort(sorted_entries.begin(), sorted_entries.end(),
        [](const auto &a, const auto &b) {
            if (a.second.first != b.second.first)
                return a.second.first < b.second.first;  // Lower hit count first
            return a.second.second < b.second.second;    // Older timestamp first
        });

    // Evict the first 'count' entries
    size_t evicted = 0;
    for (const auto &entry : sorted_entries) {
        if (evicted >= count) break;

        const std::string &key = entry.first;
        std::string mod_name = entries_[key].module_name;

        // Delete the module file from disk
        std::string mod_path = get_module_path(key);
        unlink(mod_path.c_str());

        // Remove from in-memory caches
        entries_.erase(key);
        module_json_cache_.erase(key);
        evicted++;

        log_debug("Evicted cache entry: %s (hits: %zu)\n", mod_name.c_str(), entry.second.first);
    }

    if (evicted > 0) {
        dirty_ = true;
        log("Evicted %zu cache entries\n", evicted);
    }
}

void ZypharModuleCache::evict_by_age()
{
    if (max_age_seconds_ <= 0) return;

    time_t now = time(nullptr);
    time_t cutoff = now - max_age_seconds_;

    std::vector<std::string> to_evict;
    for (const auto &it : entries_) {
        if (it.second.timestamp < cutoff) {
            to_evict.push_back(it.first);
        }
    }

    for (const std::string &key : to_evict) {
        std::string mod_path = get_module_path(key);
        unlink(mod_path.c_str());

        entries_.erase(key);
        module_json_cache_.erase(key);
    }

    if (!to_evict.empty()) {
        dirty_ = true;
        log("Evicted %zu expired cache entries (older than %ld days)\n",
            to_evict.size(), max_age_seconds_ / 86400);
    }
}

void ZypharModuleCache::save_to_disk()
{
    if (!initialized_) return;

    std::string index_path = get_index_path();

    // Build JSON using json11 for proper escaping
    std::vector<json11::Json> entries_arr;

    for (auto &it : entries_) {
        auto &e = it.second;

        // Save module data to separate file
        std::string mod_path = get_module_path(it.first);
        {
            std::ofstream mf(mod_path);
            if (!mf.good()) {
                log_warning("Failed to write module cache file: %s\n", mod_path.c_str());
                continue;
            }
            mf << e.json_data;
        }

        // Create entry JSON object
        json11::Json entry_obj = json11::Json::object {
            { "key", it.first },
            { "module_name", e.module_name },
            { "hash", static_cast<double>(e.content_hash) },
            { "pass_seq", e.pass_sequence },
            { "timestamp", static_cast<double>(e.timestamp) },
            { "hits", static_cast<int>(e.hit_count) }
        };

        entries_arr.push_back(entry_obj);
    }

    // Create root object
    json11::Json root = json11::Json::object {
        { "version", 1 },
        { "entries", entries_arr }
    };

    // Write to file
    std::ofstream f(index_path);
    if (!f.good()) {
        log_warning("Failed to open cache index for writing: %s\n", index_path.c_str());
        return;
    }

    f << root.dump();

    dirty_ = false;
    log("Saved cache index with %zu entries\n", entries_.size());
}

void ZypharModuleCache::load_from_disk()
{
    std::string index_path = get_index_path();
    std::ifstream f(index_path);
    if (!f.good()) return;

    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    // Parse JSON using json11
    std::string parse_error;
    json11::Json root = json11::Json::parse(content, parse_error);

    if (!parse_error.empty()) {
        log_warning("Failed to parse cache index: %s\n", parse_error.c_str());
        return;
    }

    if (!root.is_object()) {
        log_warning("Cache index is not a valid JSON object\n");
        return;
    }

    // Check version
    int version = root["version"].int_value();
    if (version != 1) {
        log_warning("Cache version mismatch (expected 1, got %d), clearing cache\n", version);
        return;
    }

    entries_.clear();

    // Parse entries array
    const json11::Json &entries_arr = root["entries"];
    if (!entries_arr.is_array()) {
        log_warning("Cache entries is not an array\n");
        return;
    }

    for (const auto &item : entries_arr.array_items()) {
        if (!item.is_object()) continue;

        ZypharCacheEntry entry;

        std::string key = item["key"].string_value();
        if (key.empty()) continue;

        entry.module_name = item["module_name"].string_value();
        entry.content_hash = static_cast<uint64_t>(item["hash"].number_value());
        entry.pass_sequence = item["pass_seq"].string_value();
        entry.timestamp = static_cast<time_t>(item["timestamp"].number_value());
        entry.hit_count = static_cast<size_t>(item["hits"].int_value());

        // Validate required fields
        if (entry.module_name.empty()) {
            log_debug("Skipping cache entry with empty module name\n");
            continue;
        }

        // Load module data from file
        std::string mod_path = get_module_path(key);
        std::ifstream mf(mod_path);
        if (mf.good()) {
            std::ostringstream ss;
            ss << mf.rdbuf();
            entry.json_data = ss.str();
        }

        entries_[key] = entry;
    }

    log("Loaded %zu cache entries from disk\n", entries_.size());
}

double ZypharModuleCache::hit_rate() const
{
    size_t total = total_hits_ + total_misses_;
    if (total == 0) return 0.0;
    return (double)total_hits_ / total * 100.0;
}

void ZypharModuleCache::log_stats() const
{
    log("\n=== Zyphar Cache Statistics ===\n");
    log("  Cache directory: %s\n", cache_dir_.c_str());
    log("  Total entries: %zu\n", entries_.size());
    log("  Cache hits: %zu\n", total_hits_);
    log("  Cache misses: %zu\n", total_misses_);
    log("  Hit rate: %.1f%%\n", hit_rate());
    log("\n");
}

void ZypharModuleCache::log_entries() const
{
    log("\n=== Zyphar Cache Entries ===\n\n");

    for (auto &it : entries_) {
        auto &e = it.second;
        log("  %s\n", e.module_name.c_str());
        log("    Hash: 0x%016llx\n", (unsigned long long)e.content_hash);
        log("    Pass: %s\n", e.pass_sequence.c_str());
        log("    Hits: %zu\n", e.hit_count);
        log("    Size: %zu bytes\n", e.json_data.size());
        log("\n");
    }
}

YOSYS_NAMESPACE_END
