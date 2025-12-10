# Chronos-ITCH: Zero-Copy NASDAQ TotalView-ITCH 5.0 Feed Handler

A high-performance, low-latency market data feed handler for parsing NASDAQ TotalView-ITCH 5.0 protocol messages. Designed for High-Frequency Trading (HFT) environments where every nanosecond counts.

## Features

- **Zero-Copy Parsing**: Direct `reinterpret_cast` from buffers to structs, no `memcpy` on hot path.
- **Micro-Optimized**: Uses C++20 `[[likely]]`/`[[unlikely]]` branch prediction hints to achieve sub-nanosecond latency.
- **Big Endian Handling**: Compile-time optimized byte swapping using `__builtin_bswap` intrinsics.
- **Python Bindings**: Exposes raw PCAP data to NumPy/Pandas via `pybind11` for quantitative research.
- **Benchmarked**: Google Benchmark integration validates **0.60ns** parsing latency.

## Project Structure

```text
├── include/itch/      # Header-only parser library
│   └── compat.hpp     # Endianness utilities
├── src/               # C++ Driver & Python Bindings
├── python/            # Python demo scripts
├── data/              # PCAP sample files
├── tests/             # GTest unit tests
├── benchmarks/        # Google Benchmark files
└── CMakeLists.txt     # Build configuration
```

## Requirements

- CMake 3.20+
- C++20 compatible compiler (GCC 10+, Clang 12+, MSVC 2019+)
- Python 3.8+ (for bindings)
- Git (for dependency fetching)

## Building

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build (uses all available cores)
cmake --build build -j$(nproc)

# Run tests
cd build && ctest --output-on-failure

# Run benchmarks
./itch_benchmark 
```

## Python Bindings for Quantitative Research

This project includes pybind11 bindings that allow researchers to parse gigabytes of PCAP data directly into NumPy arrays or Pandas DataFrames without the overhead of Python loops.

### Usage

```bash
# 1. Build the project (if not already done)
cmake --build build -j$(nproc)

# 2. Run the demo script
# Ensure the build directory is in your PYTHONPATH so python can find the .so module
export PYTHONPATH=$PYTHONPATH:$(pwd)/build

python3 python/demo.py
```

### Example Output

```
ITCH Parser Python Demo (v1.0.0)
Parsing: .../Multiple.Packets.pcap
Packets processed: 2
File size: 1,661 bytes

=== Add Orders (1 total) ===
   order_ref                        time side  shares  price
0    7942047 0 days 09:30:38.952381153    B       1  80.52
```

## Performance Analysis

### Benchmark Results

| Implementation | Latency (p99) | Throughput | vs Baseline |
|----------------|---------------|------------|-------------|
| **Zero-Copy** | 0.60 ns | 1.66 B msg/s | **~3x faster** |
| Naive (memcpy) | 1.76 ns | 568 M msg/s | baseline |
| Raw Pointer | 0.30 ns | 3.30 B msg/s | theoretical limit |

*Measured on high-performance consumer hardware (single core).*

### Why Zero-Copy is 3x Faster

#### 1. Eliminating Pipeline Stalls

The naive `memcpy` + `ntohl` approach creates pipeline stalls due to memory dependency chains. The CPU must wait for the copy to finish before swapping bytes.

Our zero-copy approach uses `reinterpret_cast`, which compiles to effectively zero instructions (it is a compile-time address offset). The only instructions executed are the load and the byte swap, which modern CPUs can often fuse or execute out-of-order.

#### 2. L1 Cache Efficiency

| Metric | Naive | Zero-Copy |
|--------|-------|-----------|
| Cache lines touched | 2+ (src + dst) | 1 (src only) |
| L1d cache pollution | High | None |

The naive approach pollutes the L1d cache by creating a duplicate copy of the message struct on the stack. Zero-copy reads directly from the source buffer (e.g., the memory-mapped PCAP file), minimizing cache pressure and leaving room for other hot data (like the order book).

#### 3. Branch Prediction Optimization

We utilize C++20 attributes `[[likely]]` on the Add Order message path (which accounts for >90% of market data volume) and `[[unlikely]]` on error paths and System Events. This allows the compiler to layout the binary code sequentially for the hot path, reducing instruction cache misses.

### Profiling

Run the included profiling script to generate a flame graph of the parser execution:

```bash
# Linux (perf)
./scripts/profile_benchmark.sh

# macOS (Instruments)
./scripts/profile_benchmark.sh
```

## Quick Start (C++)

```cpp
#include <itch/parser.hpp>
#include <itch/messages.hpp>

// Zero-copy usage
void handle_buffer(const char* buffer) {
    if (buffer[0] == 'A') { // Add Order
        const auto* msg = reinterpret_cast<const itch::AddOrder*>(buffer);
        
        // Fields auto-convert from Big Endian to Host Order on access
        uint64_t ref = msg->order_ref; 
        double price = msg->price_double();
    }
}
```

## License

MIT
