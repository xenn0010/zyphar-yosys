# Zyphar Incremental Synthesis for Yosys

## Overview

Zyphar Incremental Synthesis adds module-level caching to Yosys, enabling fast
re-synthesis when only parts of a design change. This can provide 10-100x speedup
for iterative development workflows.

## Commands

### `zyphar_synth` - Incremental Synthesis

Run synthesis with module-level caching:

```
zyphar_synth [options]

Options:
    -top <module>       Specify top module (auto-detected if not given)
    -full               Force full synthesis, ignore cache
    -nocache            Don't update cache with results
    -stats              Show detailed timing and cache statistics
    -nohierarchy        Skip hierarchy pass (for pre-flattened designs)
    -conservative       Invalidate cache when dependencies change
```

**Example:**
```tcl
read_verilog design.v
zyphar_synth -top main -stats
```

### `zyphar_cache` - Cache Management

Manage the module cache:

```
zyphar_cache [options]

Options:
    -init [dir]         Initialize cache (default: ~/.cache/zyphar)
    -status             Show cache statistics
    -list               List all cached entries
    -clear              Clear all cached entries
    -save               Save cache to disk
    -invalidate <mod>   Invalidate cached versions of module
    -max_entries <n>    Set max cache entries (default: 1000)
    -max_size <mb>      Set max cache size in MB (default: 500)
    -max_age <days>     Set max cache age in days (default: 30)
    -evict              Force cache eviction
```

**Example:**
```tcl
zyphar_cache -init
zyphar_cache -status
zyphar_cache -max_entries 500 -max_size 200
```

### `zyphar_watch` - Watch Mode

Watch files for changes and automatically re-synthesize:

```
zyphar_watch [options] <files...>

Options:
    -top <module>       Specify top module
    -poll <ms>          Polling interval (default: 500ms)
    -port <n>           WebSocket port for updates (not yet implemented)
    -once               Run once and exit (for testing)
```

**Example:**
```tcl
zyphar_watch -top main design.v modules/*.v
```

### `zyphar_deps` - Dependency Graph

Build and display module dependency graph:

```
zyphar_deps [options]

Options:
    -build              Build dependency graph from current design
    -show               Display the dependency graph
    -affected <modules> Show modules affected by changes
    -order              Show topological synthesis order
```

### `zyphar_hash_test` - Hash Testing

Test content hashing (for debugging):

```
zyphar_hash_test
```

### `zyphar_monitor` - Change Monitor

Attach/detach the change monitor:

```
zyphar_monitor [options]

Options:
    -attach             Attach monitor to design
    -detach             Detach monitor from design
    -reset              Reset change tracking
    -show               Show detected changes
```

## How It Works

### Content Hashing

Each module's content is hashed after hierarchy resolution. The hash includes:
- All wires (names, widths, directions)
- All cells (types, parameters, connections)
- All connections
- All parameters and attributes

### Cache Keys

Cache entries are keyed by:
- Module name (post-hierarchy, so parameterized modules are distinct)
- Content hash
- Pass sequence (e.g., "post_hierarchy")

### Conservative Mode

By default, only modules whose content has changed are re-synthesized. Use
`-conservative` when your design relies on cross-module optimizations:

```tcl
zyphar_synth -conservative
```

This will invalidate the cache for any module that depends on a changed module.

### Cache Eviction

The cache automatically evicts entries based on:
1. Age (default: 30 days)
2. Entry count (default: 1000 max)
3. Total size (default: 500MB max)

Eviction uses LRU (least recently used) based on hit count and timestamp.

## Performance

Typical speedups:

| Scenario | Speedup |
|----------|---------|
| No changes | 10-100x |
| Single module change | 2-10x |
| Full rebuild | 1x (baseline) |

## Limitations

1. **Cache Restoration**: Currently validates cache hits but still runs synthesis.
   Full cache restoration requires additional work.

2. **Cross-Module Optimization**: Changes to a module's implementation may affect
   optimizations in dependent modules. Use `-conservative` for safety.

3. **Blackboxes**: Modules marked as blackbox are not cached.

4. **Generate Blocks**: Parameterized modules with generate blocks may have
   different final module names depending on parameters.

## Configuration

Set environment variables before running Yosys:

```bash
export ZYPHAR_CACHE_DIR=/path/to/cache
export ZYPHAR_MAX_ENTRIES=500
export ZYPHAR_MAX_SIZE_MB=200
```

## Files

Cache is stored in `~/.cache/zyphar/` by default:
```
~/.cache/zyphar/
├── index.json           # Module index
├── modules/             # Serialized modules
│   ├── <hash1>.json
│   └── ...
```

## Integration

### With CI/CD

```bash
# Warm cache on first build
yosys -p "read_verilog *.v; zyphar_synth -top main"

# Subsequent builds use cache
yosys -p "read_verilog *.v; zyphar_synth -top main"
```

### With Watch Mode

```bash
# Start watching
yosys -p "zyphar_watch -top main src/*.v"
# Edit files... synthesis runs automatically
```

## Troubleshooting

### Cache Not Hitting

Check that:
1. Cache is initialized: `zyphar_cache -status`
2. Module names match (check parameterized modules)
3. Content hasn't changed: `zyphar_hash_test`

### Memory Usage

If memory is high:
1. Reduce max entries: `zyphar_cache -max_entries 100`
2. Reduce max size: `zyphar_cache -max_size 100`
3. Clear old entries: `zyphar_cache -evict`

### Incorrect Results

If synthesis results differ from expected:
1. Clear cache: `zyphar_cache -clear`
2. Use conservative mode: `zyphar_synth -conservative`
3. Force full rebuild: `zyphar_synth -full`
