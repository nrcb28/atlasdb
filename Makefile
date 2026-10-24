CC ?= cc
AR ?= ar
BUILD ?= build
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
LDLIBS += -pthread -lm

LIB_SRC = src/util.c src/parser.c src/index.c src/model.c src/engine.c src/storage.c src/database.c
LIB_OBJ = $(LIB_SRC:src/%.c=$(BUILD)/%.o)
APP_OBJ = $(BUILD)/main.o $(BUILD)/server.o

.PHONY: all clean test test-unit test-integration test-oom test-faults differential sanitize check examples benchmark package
all: $(BUILD)/atlasdb $(BUILD)/libatlasdb.a

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.c src/internal.h include/atlasdb.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/libatlasdb.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/atlasdb: $(APP_OBJ) $(BUILD)/libatlasdb.a
	$(CC) $(LDFLAGS) $(CFLAGS) $(APP_OBJ) $(BUILD)/libatlasdb.a $(LDLIBS) -o $@

$(BUILD)/test_atlas: tests/test_atlas.c $(BUILD)/libatlasdb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(BUILD)/libatlasdb.a $(LDLIBS) -o $@

$(BUILD)/test_%: tests/test_%.c $(BUILD)/libatlasdb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(BUILD)/libatlasdb.a $(LDLIBS) -o $@

$(BUILD)/test_parser_alloc: tests/test_parser_alloc.c $(BUILD)/libatlasdb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(BUILD)/libatlasdb.a $(LDLIBS) -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free -o $@

$(BUILD)/test_runtime_alloc: tests/test_runtime_alloc.c $(BUILD)/libatlasdb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(BUILD)/libatlasdb.a $(LDLIBS) -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free -o $@

$(BUILD)/test_storage_faults: tests/test_storage_faults.c $(BUILD)/libatlasdb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(BUILD)/libatlasdb.a $(LDLIBS) -Wl,--wrap=write,--wrap=fsync,--wrap=ftruncate,--wrap=rename -o $@

$(BUILD)/embed: examples/embed.c $(BUILD)/libatlasdb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(BUILD)/libatlasdb.a $(LDLIBS) -o $@

$(BUILD)/benchmark: tools/benchmark.c $(BUILD)/libatlasdb.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(BUILD)/libatlasdb.a $(LDLIBS) -o $@

test: test-unit test-integration

test-unit: $(BUILD)/test_atlas $(BUILD)/test_parser $(BUILD)/test_index $(BUILD)/test_storage $(BUILD)/test_engine_edges $(BUILD)/test_util
	$(BUILD)/test_atlas
	$(BUILD)/test_parser
	$(BUILD)/test_index
	$(BUILD)/test_storage
	$(BUILD)/test_engine_edges
	$(BUILD)/test_util

test-integration: all
	python3 tests/integration.py $(BUILD)/atlasdb
	python3 tests/frontend.py $(BUILD)/atlasdb

test-oom: $(BUILD)/test_parser_alloc $(BUILD)/test_runtime_alloc
	$(BUILD)/test_parser_alloc
	$(BUILD)/test_runtime_alloc

test-faults: $(BUILD)/test_storage_faults
	$(BUILD)/test_storage_faults

differential:
	CC='$(CC)' python3 tests/differential.py

check: test test-oom test-faults differential

sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-O1 -g -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' LDFLAGS='-fsanitize=address,undefined -no-pie' test

examples: all
	$(BUILD)/atlasdb --file examples/demo.sql

benchmark: $(BUILD)/benchmark
	$(BUILD)/benchmark

package:
	python3 tools/package.py

clean:
	rm -rf build

-include $(LIB_OBJ:.o=.d) $(APP_OBJ:.o=.d)
