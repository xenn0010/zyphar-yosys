#!/bin/bash
# Zyphar Incremental Synthesis Test Suite
# Run from yosys root directory: ./tests/zyphar/run_tests.sh

set -e

YOSYS="./yosys"
TEST_DIR="tests/zyphar"
CACHE_DIR="/tmp/zyphar_test_cache"
PASS_COUNT=0
FAIL_COUNT=0

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
    PASS_COUNT=$((PASS_COUNT + 1))
}

log_fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    FAIL_COUNT=$((FAIL_COUNT + 1))
}

log_info() {
    echo -e "${YELLOW}[INFO]${NC} $1"
}

cleanup() {
    rm -rf "$CACHE_DIR"
    rm -f /tmp/test_*.v /tmp/test_*.log
}

# Test 1: Hash Stability
test_hash_stability() {
    log_info "Test 1: Hash Stability"

    cat > /tmp/test_hash.v << 'EOF'
module test_mod(input a, b, output y);
    assign y = a & b;
endmodule
EOF

    # Run hash test command
    OUTPUT=$($YOSYS -p "read_verilog /tmp/test_hash.v; zyphar_hash_test" 2>&1)

    if echo "$OUTPUT" | grep -q "PASS (hash stable"; then
        log_pass "Hash stability verified"
    else
        log_fail "Hash stability test failed"
        echo "$OUTPUT" | grep -E "(hash|PASS|FAIL)"
    fi

    if echo "$OUTPUT" | grep -q "PASS (hash changed after add"; then
        log_pass "Hash invalidation works"
    else
        log_fail "Hash invalidation failed"
    fi
}

# Test 2: Cache Hit/Miss
test_cache_hit_miss() {
    log_info "Test 2: Cache Hit/Miss"
    cleanup

    cat > /tmp/test_cache.v << 'EOF'
module test_cache(input [3:0] a, b, output [3:0] y);
    assign y = a + b;
endmodule
EOF

    # First run - should be cache miss
    OUTPUT1=$($YOSYS -p "read_verilog /tmp/test_cache.v; zyphar_cache -init $CACHE_DIR; zyphar_synth -top test_cache" 2>&1)

    if echo "$OUTPUT1" | grep -q "0 hits"; then
        log_pass "First run: cache miss detected"
    else
        log_fail "First run should show cache miss"
    fi

    # Second run - should be cache hit
    OUTPUT2=$($YOSYS -p "read_verilog /tmp/test_cache.v; zyphar_cache -init $CACHE_DIR; zyphar_synth -top test_cache" 2>&1)

    if echo "$OUTPUT2" | grep -q "1 hits"; then
        log_pass "Second run: cache hit detected"
    else
        log_fail "Second run should show cache hit"
    fi
}

# Test 3: Cache Restoration
test_cache_restoration() {
    log_info "Test 3: Cache Restoration"
    cleanup

    cat > /tmp/test_restore.v << 'EOF'
module test_restore(input clk, d, output reg q);
    always @(posedge clk) q <= d;
endmodule
EOF

    # First run
    $YOSYS -p "read_verilog /tmp/test_restore.v; zyphar_cache -init $CACHE_DIR; zyphar_synth -top test_restore" 2>&1 > /tmp/test_restore1.log

    # Second run - should restore from cache
    OUTPUT=$($YOSYS -p "read_verilog /tmp/test_restore.v; zyphar_cache -init $CACHE_DIR; zyphar_synth -top test_restore" 2>&1)

    if echo "$OUTPUT" | grep -q "\[RESTORED\]"; then
        log_pass "Cache restoration working"
    else
        log_fail "Cache restoration not working"
    fi

    if echo "$OUTPUT" | grep -q "no synthesis needed"; then
        log_pass "Synthesis skipped for cached modules"
    else
        log_fail "Synthesis should be skipped"
    fi
}

# Test 4: Speedup
test_speedup() {
    log_info "Test 4: Speedup Measurement"
    cleanup

    cat > /tmp/test_speed.v << 'EOF'
module adder8(input [7:0] a, b, output [7:0] sum);
    assign sum = a + b;
endmodule
module adder16(input [15:0] a, b, output [15:0] sum);
    assign sum = a + b;
endmodule
module adder32(input [31:0] a, b, output [31:0] sum);
    assign sum = a + b;
endmodule
module top(input [31:0] a, b, c, output [31:0] y);
    wire [7:0] s8;
    wire [15:0] s16;
    wire [31:0] s32;
    adder8 u1(.a(a[7:0]), .b(b[7:0]), .sum(s8));
    adder16 u2(.a(a[15:0]), .b(b[15:0]), .sum(s16));
    adder32 u3(.a(a), .b(b), .sum(s32));
    assign y = {s8, s16[7:0], s32[15:0]};
endmodule
EOF

    # First run
    OUTPUT1=$($YOSYS -p "read_verilog /tmp/test_speed.v; zyphar_cache -init $CACHE_DIR; zyphar_synth -top top" 2>&1)
    TIME1=$(echo "$OUTPUT1" | grep "Total time:" | awk '{print $3}')

    # Second run
    OUTPUT2=$($YOSYS -p "read_verilog /tmp/test_speed.v; zyphar_cache -init $CACHE_DIR; zyphar_synth -top top" 2>&1)
    TIME2=$(echo "$OUTPUT2" | grep "Total time:" | awk '{print $3}')

    if [ -n "$TIME1" ] && [ -n "$TIME2" ]; then
        log_pass "Timing: First run ${TIME1}, Second run ${TIME2}"
    else
        log_fail "Could not measure timing"
    fi
}

# Test 5: Cache Eviction
test_cache_eviction() {
    log_info "Test 5: Cache Eviction"
    cleanup

    cat > /tmp/test_evict.v << 'EOF'
module test_evict(input a, output y);
    assign y = a;
endmodule
EOF

    # Initialize with small limit
    $YOSYS -p "read_verilog /tmp/test_evict.v; zyphar_cache -init $CACHE_DIR -max_entries 100; zyphar_synth -top test_evict" 2>&1 > /dev/null

    # Check status
    OUTPUT=$($YOSYS -p "zyphar_cache -init $CACHE_DIR -status" 2>&1)

    if echo "$OUTPUT" | grep -q "Total entries:"; then
        log_pass "Cache status reporting works"
    else
        log_fail "Cache status not working"
    fi
}

# Test 6: Dependency Graph
test_dependency_graph() {
    log_info "Test 6: Dependency Graph"

    cat > /tmp/test_deps.v << 'EOF'
module leaf(input a, output y);
    assign y = ~a;
endmodule
module mid(input a, output y);
    leaf l(.a(a), .y(y));
endmodule
module top_dep(input a, output y);
    mid m(.a(a), .y(y));
endmodule
EOF

    OUTPUT=$($YOSYS -p "read_verilog /tmp/test_deps.v; zyphar_deps -build -show" 2>&1)

    if echo "$OUTPUT" | grep -q "top_dep"; then
        log_pass "Dependency graph built"
    else
        log_fail "Dependency graph not built"
    fi
}

# Test 7: Multi-module Design
test_multi_module() {
    log_info "Test 7: Multi-module Design"
    cleanup

    # Create a larger design
    cat > /tmp/test_multi.v << 'EOF'
module m1(input a, output y); assign y = a; endmodule
module m2(input a, output y); assign y = ~a; endmodule
module m3(input a, output y); m1 u1(.a(a), .y(y)); endmodule
module m4(input a, output y); m2 u2(.a(a), .y(y)); endmodule
module m5(input a, output y);
    wire w1, w2;
    m3 u3(.a(a), .y(w1));
    m4 u4(.a(a), .y(w2));
    assign y = w1 ^ w2;
endmodule
EOF

    # First run
    OUTPUT1=$($YOSYS -p "read_verilog /tmp/test_multi.v; zyphar_cache -init $CACHE_DIR; zyphar_synth -top m5" 2>&1)

    if echo "$OUTPUT1" | grep -q "5 misses"; then
        log_pass "All 5 modules synthesized on first run"
    else
        log_fail "Expected 5 module synthesis"
    fi

    # Second run
    OUTPUT2=$($YOSYS -p "read_verilog /tmp/test_multi.v; zyphar_cache -init $CACHE_DIR; zyphar_synth -top m5" 2>&1)

    if echo "$OUTPUT2" | grep -q "5 hits"; then
        log_pass "All 5 modules restored from cache"
    else
        log_fail "Expected 5 cache hits"
    fi
}

# Test 8: Watch Mode (quick test)
test_watch_mode() {
    log_info "Test 8: Watch Mode (one-shot)"

    cat > /tmp/test_watch.v << 'EOF'
module test_watch(input a, output y);
    assign y = a;
endmodule
EOF

    OUTPUT=$($YOSYS -p "zyphar_cache -init $CACHE_DIR; zyphar_watch -once -top test_watch /tmp/test_watch.v" 2>&1)

    if echo "$OUTPUT" | grep -q "Watch Mode"; then
        log_pass "Watch mode starts"
    else
        log_fail "Watch mode failed to start"
    fi
}

# Run all tests
main() {
    echo "========================================"
    echo "Zyphar Incremental Synthesis Test Suite"
    echo "========================================"
    echo ""

    cleanup

    test_hash_stability
    test_cache_hit_miss
    test_cache_restoration
    test_speedup
    test_cache_eviction
    test_dependency_graph
    test_multi_module
    test_watch_mode

    cleanup

    echo ""
    echo "========================================"
    echo "Results: ${PASS_COUNT} passed, ${FAIL_COUNT} failed"
    echo "========================================"

    if [ "$FAIL_COUNT" -gt 0 ]; then
        exit 1
    fi
}

main "$@"
