# Multi-threaded Malloc

Memory allocator for multi-threaded programs.

## Set Up

* Compile my allocs
```
make alloc
```

* Compile Test Files
```
make tests
```

This will create executables like `test0_v0`, which means `test_0` compiled with `my_alloc_v0`.

* Compile Benchmark Programs

The benchmarks are what I call Programs A, B, and C in the report.
```
make bench
```

This will create executables like `bench_a_v0`, which means `bench_a` compiled with `my_alloc_v0`.

* Run Benchmarks

We can pass arguments to the benchmarks. 
```
./bench_a_<variant> [num_allocs] [num_iters]
```

```
./bench_b_<variant> [num_threads] [num_allocs] [num_iters]
```

```
./bench_c_<variant> [num_consumers] [num_allocs] [num_iters]
```