# Chronos-ITCH: Zero-Copy NASDAQ TotalView-ITCH 5.0 Feed Handler

A high-performance, low-latency market data feed handler for parsing NASDAQ TotalView-ITCH 5.0 protocol messages. Designed for High-Frequency Trading (HFT) environments where every nanosecond counts.

## 🚀 Highlights

| Component | Performance |
|-----------|-------------|
| **ITCH Parser** | 0.60ns latency, 1.66B msg/sec |
| **Matching Engine** | 44.9ns avg order latency |
| **End-to-End Replay** | 290K orders/sec @ 452 MB/sec |
| **Memory Pool** | 10M orders, zero allocation on hot path |

## Features

- **Zero-Copy Parsing**: Direct `reinterpret_cast` from buffers to structs, no `memcpy` on hot path.
- **Lock-Free Memory Pool**: Pre-allocated 10M order slots with O(1) alloc/dealloc.
- **Matching Engine**: Price-time priority order book with intrusive data structures.
- **Micro-Optimized**: Uses C++20 `[[likely]]`/`[[unlikely]]` branch prediction hints.
- **Big Endian Handling**: Compile-time optimized byte swapping using `__builtin_bswap` intrinsics.
- **Python Bindings**: Exposes raw PCAP data to NumPy/Pandas via `pybind11` for quantitative research.
- **Stress Tested**: Validated with 500MB synthetic market data (320K+ orders).

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        CHRONOS Market Replay Engine                      │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   ┌──────────────┐    ┌──────────────┐    ┌──────────────────────────┐  │
│   │  PCAP Reader │───▶│ ITCH Parser  │───▶│      Order Book          │  │
│   │  (mmap I/O)  │    │ (Zero-Copy)  │    │  (Price-Time Priority)   │  │
│   └──────────────┘    └──────────────┘    └──────────────────────────┘  │
│          │                   │                        │                  │
│          ▼                   ▼                        ▼                  │
│   ┌──────────────┐    ┌──────────────┐    ┌──────────────────────────┐  │
│   │ Zero-Copy    │    │  Visitor     │    │     Memory Pool          │  │
│   │ Buffer View  │    │  Pattern     │    │  (10M Pre-allocated)     │  │
│   └──────────────┘    └──────────────┘    └──────────────────────────┘  │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

## Project Structure

```text
├── include/
│   ├── itch/          # Header-only ITCH parser library
│   │   ├── parser.hpp       # Zero-copy message dispatcher
│   │   ├── messages.hpp     # Packed ITCH message structs
│   │   └── pcap_reader.hpp  # Memory-mapped PCAP file reader
│   └── book/          # Order book & matching engine
│       ├── order_book.hpp   # Price-time priority matching
│       ├── memory_pool.hpp  # Lock-free object pool
│       └── intrusive_list.hpp
├── src/
│   ├── main.cpp             # ITCH parser CLI driver
│   ├── replay_driver.cpp    # Chronos market replay engine
│   └── python_bindings.cpp  # pybind11 NumPy integration
├── scripts/
│   └── generate_stress.py   # 500MB stress test generator
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
./build/itch_benchmark 
```

## Chronos Market Replay Engine

The `chronos_replay` binary integrates the ITCH parser with a full order book matching engine, simulating a complete HFT data pipeline.

### Running the Replay

```bash
# Basic usage with sample data
./build/chronos_replay data/Multiple.Packets.pcap

# With a custom PCAP file
./build/chronos_replay /path/to/your/data.pcap
```

### Sample Output

```
╔══════════════════════════════════════════════════════════════╗
║           CHRONOS - Market Replay Engine                     ║
║   Zero-Copy ITCH Parser + High-Frequency Matching Engine     ║
╚══════════════════════════════════════════════════════════════╝

Initializing Memory Pool (Capacity: 10000000 orders)...
  Pool Memory: 352.86 MB
Initializing OrderBook...
Opening PCAP file: data/StressTest.pcap
  File size: 500.00 MB

Starting market replay...
  Match trigger interval: every 100th order

=== Performance ===
Packets processed: 640548
Total time: 1105.813 ms
Throughput: 0.58 million packets/sec
Order Rate: 0.29 million orders/sec
Bandwidth: 452.16 MB/sec

=== Market Replay Metrics ===
Orders Processed:           320274
Orders Added to Book:       320274
Orders Cancelled:                0
Matches Executed:             3202
Avg add_order latency: 44.9 ns

=== Final Book State ===
Orders Resting: 313870
Bid Levels: 1
Ask Levels: 0
Best Bid: 80.5200

Pool Utilization: 3.14% (313870 / 10000000)
```

## Stress Testing

Generate a large synthetic PCAP file to stress test the engine:

```bash
# Generate 500MB stress test file (multiplies template packets)
python3 scripts/generate_stress.py

# Run the stress test
./build/chronos_replay data/StressTest.pcap
```

### Stress Test Results (500MB, 320K Orders)

| Metric | Result |
|--------|--------|
| **File Size** | 500 MB |
| **Packets Processed** | 640,548 |
| **Orders Processed** | 320,274 |
| **Total Time** | 1.1 seconds |
| **Throughput** | 580K packets/sec |
| **Order Rate** | 290K orders/sec |
| **Bandwidth** | 452 MB/sec |
| **Avg Order Latency** | **44.9 nanoseconds** |
| **Matches Executed** | 3,202 |
| **Pool Utilization** | 3.14% (313K / 10M) |

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

### Parser Benchmark Results

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
