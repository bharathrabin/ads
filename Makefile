CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -Iinclude
OPTFLAGS = -O3 -march=native -flto
DEBUGFLAGS = -g -O0 -fsanitize=address,undefined

SRC = src/hashmap.c
OBJ = $(SRC:.c=.o)
LIB = build/libads.a

EXAMPLES = examples/hashmap_example.c
EXAMPLE_BINS = $(patsubst examples/%.c,build/%,$(EXAMPLES))

TESTS = tests/hashmap_test.c
TEST_BINS = $(patsubst tests/%.c,build/%,$(TESTS))

BENCH = bench/hashmap_bench.c
BENCH_BINS = $(patsubst bench/%.c,build/%,$(BENCH))

.PHONY: all clean examples test bench debug

all: $(LIB)

# Library build
$(LIB): $(OBJ) | build
	ar rcs $@ $^

src/%.o: src/%.c include/%.h
	$(CC) $(CFLAGS) $(OPTFLAGS) -c $< -o $@

# Examples
examples: $(EXAMPLE_BINS)

build/%: examples/%.c $(LIB) | build
	$(CC) $(CFLAGS) $(OPTFLAGS) $< -L build -lads -o $@

# Tests
test: $(TEST_BINS)
	@for test in $(TEST_BINS); do \
		echo "Running $$test..."; \
		$$test || exit 1; \
	done

build/%: tests/%.c $(LIB) | build
	$(CC) $(CFLAGS) $(DEBUGFLAGS) $< -L build -lads -o $@

# Benchmarks
bench: $(BENCH_BINS)
	@for bench in $(BENCH_BINS); do \
		echo "Running $$bench..."; \
		$$bench; \
	done

build/%: bench/%.c $(LIB) | build
	$(CC) $(CFLAGS) $(OPTFLAGS) $< -L build -lads -o $@

build:
	mkdir -p build

clean:
	rm -rf build src/*.o

debug: OPTFLAGS = $(DEBUGFLAGS)
debug: clean all