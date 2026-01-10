/*
 * Zyphar Module Cache
 * Disk-based cache for synthesized modules to enable incremental synthesis
 */

#ifndef ZYPHAR_CACHE_H
#define ZYPHAR_CACHE_H

#include "kernel/yosys.h"
#include "kernel/rtlil.h"
#include <map>
#include <string>
#include <ctime>

YOSYS_NAMESPACE_BEGIN

struct ZypharCacheEntry {
    std::string module_name;
    uint64_t content_hash;
    std::string pass_sequence;  // e.g., "synth;opt;techmap"
    std::string json_data;      // Serialized module
    time_t timestamp;
    size_t hit_count;
};

class ZypharModuleCache {
public:
    ZypharModuleCache();
    ~ZypharModuleCache();

    // Initialize cache directory
    bool init(const std::string &cache_dir = "");

    // Configuration
    void set_max_entries(size_t max_entries) { max_entries_ = max_entries; }
    void set_max_size_bytes(size_t max_bytes) { max_size_bytes_ = max_bytes; }
    void set_max_age_days(int days) { max_age_seconds_ = days * 86400; }
    size_t get_max_entries() const { return max_entries_; }
    size_t get_max_size_bytes() const { return max_size_bytes_; }

    // Eviction
    void evict_if_needed();
    void evict_oldest(size_t count);
    void evict_by_age();
    size_t total_size_bytes() const;

    // Check if a module with given hash exists in cache
    bool has(const std::string &module_name, uint64_t hash, const std::string &pass_seq = "") const;

    // Get cached module data (returns nullptr if not found)
    const ZypharCacheEntry* get(const std::string &module_name, uint64_t hash, const std::string &pass_seq = "") const;

    // Store a synthesized module in cache (returns false on failure)
    bool put(const std::string &module_name, uint64_t hash, const std::string &pass_seq,
             RTLIL::Module *module, RTLIL::Design *design);

    // Restore a cached module into a design
    bool restore(const std::string &module_name, uint64_t hash, const std::string &pass_seq,
                RTLIL::Design *design);

    // Invalidate all cached versions of a module
    void invalidate(const std::string &module_name);

    // Invalidate a specific cached entry
    void invalidate(const std::string &module_name, uint64_t hash, const std::string &pass_seq);

    // Invalidate all modules affected by changes (using dependency graph)
    void invalidate_affected(const std::set<std::string> &changed_modules,
                            const std::map<std::string, std::set<std::string>> &dependents);

    // Clear all cache
    void clear();

    // Persistence
    void save_to_disk();
    void load_from_disk();

    // Statistics
    size_t entry_count() const { return entries_.size(); }
    size_t hit_count() const { return total_hits_; }
    size_t miss_count() const { return total_misses_; }
    double hit_rate() const;

    // Cache directory
    std::string get_cache_dir() const { return cache_dir_; }

    // Debug
    void log_stats() const;
    void log_entries() const;

    // Check if initialized
    bool is_initialized() const { return initialized_; }

private:
    // Cache storage: key = "module_name|hash|pass_seq"
    std::map<std::string, ZypharCacheEntry> entries_;

    // In-memory module storage for fast restore
    std::map<std::string, std::string> module_json_cache_;

    // Cache directory
    std::string cache_dir_;

    // State
    bool initialized_ = false;
    bool dirty_ = false;

    // Cache limits (defaults: 1000 entries, 500MB, 30 days)
    size_t max_entries_ = 1000;
    size_t max_size_bytes_ = 500 * 1024 * 1024;  // 500MB
    time_t max_age_seconds_ = 30 * 86400;        // 30 days

    // Statistics
    mutable size_t total_hits_ = 0;
    mutable size_t total_misses_ = 0;

    // Helper functions
    std::string make_key(const std::string &module_name, uint64_t hash, const std::string &pass_seq) const;
    std::string get_index_path() const;
    std::string get_module_path(const std::string &key) const;

    // Serialize module to JSON
    std::string serialize_module(RTLIL::Module *module, RTLIL::Design *design) const;

    // Deserialize module from JSON
    bool deserialize_module(const std::string &json, RTLIL::Design *design, const std::string &module_name);
};

// Global instance
extern ZypharModuleCache zyphar_cache;

YOSYS_NAMESPACE_END

#endif // ZYPHAR_CACHE_H
