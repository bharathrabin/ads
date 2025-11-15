# ADS - Algorithms and Data Structures Library

A high-performance algorithms and data structures library written in C.

## Features

- **Hashmap**: Generic hash table implementation inspired by Go's map, with inline storage and incremental rehashing
- More data structures coming soon...

## Building

```bash
make
```

## Running Examples

```bash
make examples
./build/hashmap_example
```

## Running Tests

```bash
make test
```

## Running Benchmarks

```bash
make bench
```

## Project Structure

```
ads/
├── include/     # Public headers
├── src/         # Implementation files
├── examples/    # Usage examples
├── tests/       # Unit tests
└── bench/       # Performance benchmarks
```