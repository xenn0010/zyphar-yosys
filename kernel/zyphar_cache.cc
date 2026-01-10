/*
 * Zyphar Module Cache Implementation
 */

#include "kernel/zyphar_cache.h"
#include "kernel/log.h"
#include "backends/rtlil/rtlil_backend.h"
#include <fstream>
#include <sstream>
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
    if (initialized_ && dirty_) {
        save_to_disk();
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

void ZypharModuleCache::put(const std::string &module_name, uint64_t hash, const std::string &pass_seq,
                           RTLIL::Module *module, RTLIL::Design *design)
{
    std::string key = make_key(module_name, hash, pass_seq);

    ZypharCacheEntry entry;
    entry.module_name = module_name;
    entry.content_hash = hash;
    entry.pass_sequence = pass_seq;
    entry.json_data = serialize_module(module, design);
    entry.timestamp = time(nullptr);
    entry.hit_count = 0;

    entries_[key] = entry;

    // Also cache in memory
    module_json_cache_[key] = entry.json_data;

    dirty_ = true;

    log("Cached module %s (hash: 0x%016llx, pass: %s)\n",
        module_name.c_str(), (unsigned long long)hash, pass_seq.c_str());
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
    std::ostringstream out;

    // Create a temporary design with just this module
    RTLIL::Design temp_design;
    RTLIL::Module *temp_mod = module->clone();
    temp_design.add(temp_mod);

    // Use RTLIL backend to serialize
    RTLIL_BACKEND::dump_module(out, "", temp_mod, &temp_design, false, false);

    return out.str();
}

bool ZypharModuleCache::deserialize_module(const std::string &rtlil_data, RTLIL::Design *design, const std::string &module_name)
{
    // Write to temp file and use RTLIL frontend
    std::string temp_file = cache_dir_ + "/temp_restore.rtlil";
    {
        std::ofstream f(temp_file);
        f << "autoidx 1\n";  // RTLIL header
        f << rtlil_data;
    }

    // Parse the RTLIL
    try {
        std::vector<std::string> args;
        args.push_back(temp_file);
        Frontend::frontend_call(design, nullptr, temp_file, "rtlil");
        unlink(temp_file.c_str());
        return true;
    } catch (...) {
        log_warning("Failed to restore module from cache: %s\n", module_name.c_str());
        unlink(temp_file.c_str());
        return false;
    }
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

void ZypharModuleCache::save_to_disk()
{
    if (!initialized_) return;

    std::string index_path = get_index_path();
    std::ofstream f(index_path);

    f << "{\n";
    f << "  \"version\": 1,\n";
    f << "  \"entries\": [\n";

    bool first = true;
    for (auto &it : entries_) {
        if (!first) f << ",\n";
        first = false;

        auto &e = it.second;

        // Save module data to separate file
        std::string mod_path = get_module_path(it.first);
        {
            std::ofstream mf(mod_path);
            mf << e.json_data;
        }

        f << "    {\n";
        f << "      \"key\": \"" << it.first << "\",\n";
        f << "      \"module_name\": \"" << e.module_name << "\",\n";
        f << "      \"hash\": " << e.content_hash << ",\n";
        f << "      \"pass_seq\": \"" << e.pass_sequence << "\",\n";
        f << "      \"timestamp\": " << e.timestamp << ",\n";
        f << "      \"hits\": " << e.hit_count << "\n";
        f << "    }";
    }

    f << "\n  ]\n";
    f << "}\n";

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

    // Simple JSON parsing
    entries_.clear();

    size_t pos = 0;
    while ((pos = content.find("\"key\":", pos)) != std::string::npos) {
        ZypharCacheEntry entry;
        std::string key;

        // Parse key
        size_t start = content.find('"', pos + 6) + 1;
        size_t end = content.find('"', start);
        key = content.substr(start, end - start);
        pos = end;

        // Parse module_name
        pos = content.find("\"module_name\":", pos);
        start = content.find('"', pos + 14) + 1;
        end = content.find('"', start);
        entry.module_name = content.substr(start, end - start);
        pos = end;

        // Parse hash
        pos = content.find("\"hash\":", pos);
        start = pos + 8;
        end = content.find_first_of(",}", start);
        entry.content_hash = std::stoull(content.substr(start, end - start));
        pos = end;

        // Parse pass_seq
        pos = content.find("\"pass_seq\":", pos);
        start = content.find('"', pos + 11) + 1;
        end = content.find('"', start);
        entry.pass_sequence = content.substr(start, end - start);
        pos = end;

        // Parse timestamp
        pos = content.find("\"timestamp\":", pos);
        start = pos + 13;
        end = content.find_first_of(",}", start);
        entry.timestamp = std::stol(content.substr(start, end - start));
        pos = end;

        // Parse hits
        pos = content.find("\"hits\":", pos);
        if (pos != std::string::npos) {
            start = pos + 8;
            end = content.find_first_of(",}\n", start);
            entry.hit_count = std::stoul(content.substr(start, end - start));
            pos = end;
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
