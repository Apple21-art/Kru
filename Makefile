CC       ?= gcc
STD      := -std=gnu11
SRC      := $(wildcard src/*.c)
HEADERS  := $(wildcard include/*.h)
BUILD    := build
BIN      := $(BUILD)/kru0

# -O2 is what the PERFORMANCE.md sub-10ms frontend budget was measured
# against; it's the default `make` target. `make debug` swaps in
# ASan/UBSan per the fuzzing/sanitizer gate in CONFORMANCE.md, at the
# cost of the latency budget (sanitizers are not compatible with the
# p95 < 10ms target and are for correctness testing only).
CFLAGS   ?= -O2 -Wall -Wextra
SAN_ASAN_OPTIONS ?= detect_leaks=1:halt_on_error=1
SAN_UBSAN_OPTIONS ?= halt_on_error=1
DEBUG_CFLAGS := -O0 -g -Wall -Wextra -fsanitize=address,undefined

.PHONY: all debug test test-debug stress stress-debug check bench clean

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
	bash tools/test_cli.sh $(BIN)
	bash tools/test_file_errors.sh $(BIN)
	bash tools/test_timing.sh $(BIN)
	bash tools/test_run_native.sh $(BIN)
	bash tools/test_benchmark_timer.sh $(BUILD)/kru-bench-timer
	bash tools/run_tests.sh $(BIN)
	bash tools/test_compile_fail.sh $(BIN)
	bash tools/test_adventure.sh $(BIN)
	bash tools/test_native_backend.sh $(BIN)
	bash tools/test_native_stage5.sh $(BIN)
	bash tools/test_native_collections.sh $(BIN)
	bash tools/test_native_aggregates.sh $(BIN)
	bash tools/test_native_aggregate_rejections.sh $(BIN)
	bash tools/test_native_string_operations.sh $(BIN)
	bash tools/test_native_runtime.sh $(BIN)
	bash tools/test_parser_codex.sh $(BIN)
	bash tools/test_sema_flow.sh $(BIN)
	bash tools/test_shadow_runtime.sh $(BIN)
	bash tools/audit_diagnostics.sh

test-debug: debug
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
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_native_runtime.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_parser_codex.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_sema_flow.sh $(BUILD)/kru0-debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/test_shadow_runtime.sh $(BUILD)/kru0-debug

stress: $(BIN)
	bash tools/stress_tests.sh $(BIN)

stress-debug: debug
	ASAN_OPTIONS=$(SAN_ASAN_OPTIONS) UBSAN_OPTIONS=$(SAN_UBSAN_OPTIONS) bash tools/stress_tests.sh $(BUILD)/kru0-debug

check: test test-debug stress stress-debug

bench: $(BIN) $(BUILD)/kru-bench-timer
	bash tools/benchmark.sh $(BIN) $(BUILD)/kru-bench-timer

clean:
	rm -rf $(BUILD)
