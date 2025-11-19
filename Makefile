CC = gcc
CFLAGS = -Wall -fopenmp

# All test sources under tests/
TEST_SRCS := $(wildcard tests/*.c)
TESTS_V0  := $(patsubst tests/%.c,%_v0,$(TEST_SRCS))
TESTS_V1  := $(patsubst tests/%.c,%_v1,$(TEST_SRCS))

# All benchmark sources under bench/: bench/bench_a.c, bench/bench_b.c, ...
BENCH_SRCS := $(wildcard bench/bench_*.c)
BENCH_LIBC := $(patsubst bench/bench_%.c,bench_%_libc,$(BENCH_SRCS))
BENCH_V0   := $(patsubst bench/bench_%.c,bench_%_v0,$(BENCH_SRCS))
BENCH_V1   := $(patsubst bench/bench_%.c,bench_%_v1,$(BENCH_SRCS))

.PHONY: alloc tests all bench clean default all_default

# Default: just build the allocator objects
alloc: my_alloc_v0.o my_alloc_v1.o

default: alloc
all_default: alloc

# Build all tests (v0 and v1)
tests: $(TESTS_V0) $(TESTS_V1)

# Build everything: alloc + tests + benchmarks
all: alloc tests bench

my_alloc_v0.o: my_alloc_v0.c my_alloc.h
	$(CC) $(CFLAGS) -c $< -o $@

my_alloc_v1.o: my_alloc_v1.c my_alloc.h
	$(CC) $(CFLAGS) -c $< -o $@

%_v0: tests/%.c my_alloc_v0.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%_v1: tests/%.c my_alloc_v1.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ---------- Generic benchmarks (sources in bench/) ----------

# Build all benchmarks (libc + v0 + v1 variants)
bench: $(BENCH_LIBC) $(BENCH_V0) $(BENCH_V1)

# libc version: -DUSE_LIBC, no custom allocator object
bench_%_libc: bench/bench_%.c
	$(CC) $(CFLAGS) -DUSE_LIBC $< -o $@

# v0 version: link against my_alloc_v0.o
bench_%_v0: bench/bench_%.c my_alloc_v0.o
	$(CC) $(CFLAGS) $^ -o $@

# v1 version: link against my_alloc_v1.o
bench_%_v1: bench/bench_%.c my_alloc_v1.o
	$(CC) $(CFLAGS) $^ -o $@

# ----------------------------------------

clean:
	rm -f *.o *_v0 *_v1 bench_*_libc bench_*_v0 bench_*_v1
