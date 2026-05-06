# Efficient Servers

C++ TCP/protobuf server for building per-connection walk graphs and answering shortest-path queries.

Input `.pbf` files are expected one directory above the repo, for example:

```text
../walk3000.pbf
../walk10nodes500.pbf
```

## Build

```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Correctness

```bash
./run_all_tests.sh
```

## Benchmarks

Single input stream:

```bash
./bench_walk3000.sh
```

Parallel clients:

```bash
./bench_parallel.sh 100
./bench_parallel.sh 1000
```

`bench_parallel.sh` prints wall time, throughput, avg/p95/max latency, and response size.
