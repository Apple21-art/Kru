CC       ?= gcc
STD      := -std=gnu11
SRC      := $(wildcard src/*.c)
HEADERS  := $(wildcard include/*.h)
BUILD    := build
BIN      := $(BUILD)/kru0
TEST_BIN ?= $(BIN)
AUDIT_BIN := $(BUILD)/kru0-allocation-audit

# -O2 is what the PERFORMANCE.md sub-10ms frontend budget was measured
# against; it's the default `make` target. `make debug` swaps in
# ASan/UBSan per the fuzzing/sanitizer gate in CONFORMANCE.md, at the
# cost of the latency budget (sanitizers are not compatible with the
# p95 < 10ms target and are for correctness testing only).
CFLAGS   ?= -O2 -Wall -Wextra
SAN_ASAN_OPTIONS ?= detect_leaks=1:halt_on_error=1
SAN_UBSAN_OPTIONS ?= halt_on_error=1
DEBUG_CFLAGS := -O0 -g -Wall -Wextra -fsanitize=address,undefined

.PHONY: all debug test test-debug stress stress-debug check bench bench-native clean

all: $(BIN)

$(BIN): $(SRC) $(HEADERS) | $(BUILD)
	$(CC) $(STD) $(CFLAGS) -o $(BIN) $(SRC)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/kru-bench-timer: tools/bench_timer.c | $(BUILD)
	$(CC) $(STD) $(CFLAGS) -o $@ $<

debug: $(SRC) | $(BUILD)
	$(CC) $(STD) $(DEBUG_CFLAGS) -o $(BUILD)/kru0-debug $(SRC)

test: $(BIN) $(BUILD)/kru-bench-timer
	bash tools/test_types.sh
	bash tools/test_native_generic_structs.sh $(TEST_BIN)
	bash tools/test_native_extended.sh $(TEST_BIN)
	bash tools/test_native_reference_flow.sh $(TEST_BIN)
	bash tools/test_native_move_flow.sh $(TEST_BIN)
	bash tools/test_native_slices.sh $(TEST_BIN)
	bash tools/test_native_primitives.sh $(TEST_BIN)
	bash tools/test_native_casts.sh $(TEST_BIN)
	bash tools/test_native_floats.sh $(TEST_BIN)
	bash tools/test_native_i128.sh $(TEST_BIN)
	bash tools/test_native_wide_casts.sh $(TEST_BIN)
	bash tools/test_native_chars.sh $(TEST_BIN)
	bash tools/test_native_expression_scale.sh $(TEST_BIN)
	bash tools/test_cli.sh $(TEST_BIN)
	bash tools/test_file_errors.sh $(TEST_BIN)
	bash tools/test_timing.sh $(TEST_BIN)
	bash tools/test_run_native.sh $(TEST_BIN)
	bash tools/test_benchmark_timer.sh $(BUILD)/kru-bench-timer
	bash tools/run_tests.sh $(BIN)
	bash tools/test_compile_fail.sh $(TEST_BIN)
	bash tools/test_adventure.sh $(TEST_BIN)
	bash tools/test_native_backend.sh $(TEST_BIN)
	bash tools/test_native_stage5.sh $(TEST_BIN)
	bash tools/test_native_collections.sh $(TEST_BIN)
	bash tools/test_native_aggregates.sh $(TEST_BIN)
	bash tools/test_native_aggregate_rejections.sh $(TEST_BIN)
	bash tools/test_native_string_operations.sh $(TEST_BIN)
	bash tools/test_native_string_slices.sh $(TEST_BIN)
	bash tools/test_native_runtime.sh $(TEST_BIN)
	bash tools/test_native_loads.sh $(TEST_BIN)
	bash tools/test_native_compact.sh $(TEST_BIN)
	bash tools/test_native_stores.sh $(TEST_BIN)
	bash tools/test_native_leaf_registers.sh $(TEST_BIN)
	bash tools/test_native_strength.sh $(TEST_BIN)
	python3 tools/test_native_integer_properties.py $(TEST_BIN)
	bash tools/test_native_heap_audit.sh $(TEST_BIN)
	bash tools/test_native_match_methods.sh $(TEST_BIN)
	bash tools/test_c_tracking_growth.sh $(TEST_BIN)
	bash tools/test_bootstrap_string_cleanup.sh $(TEST_BIN)
	bash tools/test_native_tail_calls.sh $(TEST_BIN)
	bash tools/test_native_diagnostics.sh $(TEST_BIN)
	bash tools/test_parser_codex.sh $(TEST_BIN)
	python3 tools/test_parser_progress.py $(TEST_BIN)
	bash tools/test_sema_flow.sh $(TEST_BIN)
	bash tools/test_shadow_runtime.sh $(TEST_BIN)
	bash tools/audit_diagnostics.sh

test-debug: debug
	KRU_TYPE_TEST_FLAGS="$(DEBUG_CFLAGS)" ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_types.sh
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_generic_structs.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_extended.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_reference_flow.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_move_flow.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_slices.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_primitives.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_casts.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_floats.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_i128.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_wide_casts.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_chars.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_expression_scale.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_cli.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_timing.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_run_native.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/run_tests.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_compile_fail.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_adventure.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_backend.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_stage5.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_collections.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_aggregates.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_aggregate_rejections.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_string_operations.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_string_slices.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_runtime.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_loads.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_compact.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_stores.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_leaf_registers.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_strength.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) python3 tools/test_native_integer_properties.py $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_heap_audit.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_match_methods.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_c_tracking_growth.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_bootstrap_string_cleanup.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_tail_calls.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_diagnostics.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_parser_codex.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) python3 tools/test_parser_progress.py $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_sema_flow.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_shadow_runtime.sh $(BUILD)/kru0-debug

stress: $(BIN)
	bash tools/stress_tests.sh $(TEST_BIN)

stress-debug: debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/stress_tests.sh $(BUILD)/kru0-debug

check: test test-debug stress stress-debug

bench: $(BIN) $(BUILD)/kru-bench-timer
	bash tools/benchmark.sh $(BIN) $(BUILD)/kru-bench-timer

clean:
	rm -rf $(BUILD)

bench-native: $(BIN)
	python3 tools/benchmark_native_compare.py $(BIN)

.PHONY: check-allocations
$(AUDIT_BIN): $(SRC) $(HEADERS) tools/allocation_audit.c | $(BUILD)
	$(CC) $(STD) -O0 -g -Wall -Wextra -fno-pie -no-pie -fsanitize=undefined -fno-sanitize-recover=all -o $@ $(SRC) tools/allocation_audit.c -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=strdup,--wrap=strndup

check-allocations: $(AUDIT_BIN)
	bash tools/test_allocation_audit.sh
	: > $(BUILD)/allocation-audit-findings.log
	KRU_ALLOCATION_AUDIT_LOG=$(abspath $(BUILD)/allocation-audit-findings.log) $(MAKE) test stress TEST_BIN=$(AUDIT_BIN)
	test ! -s $(BUILD)/allocation-audit-findings.log
