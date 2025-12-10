/**
 * @file itch_bench.cpp
 * @brief Performance benchmarks for ITCH 5.0 parser.
 *
 * Compares zero-copy parsing (reinterpret_cast with lazy byte-swap)
 * against naive parsing (memcpy + manual byte conversion).
 *
 * METHODOLOGY:
 * 1. Pre-load PCAP data into memory to avoid measuring disk I/O.
 * 2. Use synthetic ITCH messages for consistent benchmarking.
 * 3. Report both latency (ns/message) and throughput (messages/sec).
 */

#include <benchmark/benchmark.h>
#include <cstring>
#include <vector>

#include <itch/compat.hpp>
#include <itch/messages.hpp>
#include <itch/parser.hpp>

namespace {

// ============================================================================
// Synthetic ITCH Message Data
// ============================================================================

/**
 * @brief Create a valid AddOrder message in network byte order.
 *
 * This is the "ground truth" message we'll parse in benchmarks.
 * Using synthetic data ensures consistent results across runs.
 */
std::vector<char> create_add_order_message() {
  std::vector<char> buffer(36);

  // Message type 'A' at offset 0
  buffer[0] = 'A';

  // Stock locate = 1234 (big-endian) at offset 1
  buffer[1] = 0x04;
  buffer[2] = 0xD2;

  // Tracking number = 5678 (big-endian) at offset 3
  buffer[3] = 0x16;
  buffer[4] = 0x2E;

  // Timestamp (6 bytes) at offset 5 - 12:34:56.789012345 ns
  uint64_t ts = 45296789012345ULL; // nanoseconds since midnight
  buffer[5] = (ts >> 40) & 0xFF;
  buffer[6] = (ts >> 32) & 0xFF;
  buffer[7] = (ts >> 24) & 0xFF;
  buffer[8] = (ts >> 16) & 0xFF;
  buffer[9] = (ts >> 8) & 0xFF;
  buffer[10] = ts & 0xFF;

  // Order reference number (8 bytes, big-endian) at offset 11
  uint64_t order_ref = 0x123456789ABCDEF0ULL;
  for (int i = 0; i < 8; ++i) {
    buffer[11 + i] = (order_ref >> (56 - i * 8)) & 0xFF;
  }

  // Side = 'B' (Buy) at offset 19
  buffer[19] = 'B';

  // Shares = 1000 (big-endian) at offset 20
  uint32_t shares = 1000;
  buffer[20] = (shares >> 24) & 0xFF;
  buffer[21] = (shares >> 16) & 0xFF;
  buffer[22] = (shares >> 8) & 0xFF;
  buffer[23] = shares & 0xFF;

  // Stock symbol "AAPL    " at offset 24
  std::memcpy(&buffer[24], "AAPL    ", 8);

  // Price = 150.25 * 10000 = 1502500 (big-endian) at offset 32
  uint32_t price = 1502500;
  buffer[32] = (price >> 24) & 0xFF;
  buffer[33] = (price >> 16) & 0xFF;
  buffer[34] = (price >> 8) & 0xFF;
  buffer[35] = price & 0xFF;

  return buffer;
}

// Pre-created message buffer (global for benchmark access)
const std::vector<char> g_add_order_msg = create_add_order_message();

// ============================================================================
// Naive Implementation (Baseline for Comparison)
// ============================================================================

/**
 * @brief Standard C++ struct with natural alignment (NOT packed).
 *
 * This represents what a "typical" developer might create,
 * requiring memcpy to populate from network data.
 */
struct NaiveAddOrder {
  char msg_type;
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint64_t order_ref;
  char side;
  uint32_t shares;
  char stock[8];
  uint32_t price;
};

/**
 * @brief Parse AddOrder message the "naive" way.
 *
 * Uses memcpy + ntohl/ntohll to convert from network format.
 * This simulates what a developer would do without zero-copy design.
 */
inline void parse_naive(const char *buffer, NaiveAddOrder &out) {
  out.msg_type = buffer[0];

  // Stock locate (2 bytes, big-endian)
  uint16_t tmp16;
  std::memcpy(&tmp16, buffer + 1, 2);
  out.stock_locate = itch::ntoh(tmp16);

  // Tracking number (2 bytes, big-endian)
  std::memcpy(&tmp16, buffer + 3, 2);
  out.tracking_number = itch::ntoh(tmp16);

  // Timestamp (6 bytes, big-endian) - manual reconstruction
  out.timestamp =
      (static_cast<uint64_t>(static_cast<uint8_t>(buffer[5])) << 40) |
      (static_cast<uint64_t>(static_cast<uint8_t>(buffer[6])) << 32) |
      (static_cast<uint64_t>(static_cast<uint8_t>(buffer[7])) << 24) |
      (static_cast<uint64_t>(static_cast<uint8_t>(buffer[8])) << 16) |
      (static_cast<uint64_t>(static_cast<uint8_t>(buffer[9])) << 8) |
      static_cast<uint64_t>(static_cast<uint8_t>(buffer[10]));

  // Order reference (8 bytes, big-endian)
  uint64_t tmp64;
  std::memcpy(&tmp64, buffer + 11, 8);
  out.order_ref = itch::ntoh(tmp64);

  // Side (1 byte)
  out.side = buffer[19];

  // Shares (4 bytes, big-endian)
  uint32_t tmp32;
  std::memcpy(&tmp32, buffer + 20, 4);
  out.shares = itch::ntoh(tmp32);

  // Stock symbol (8 bytes, ASCII)
  std::memcpy(out.stock, buffer + 24, 8);

  // Price (4 bytes, big-endian)
  std::memcpy(&tmp32, buffer + 32, 4);
  out.price = itch::ntoh(tmp32);
}

// ============================================================================
// Benchmark Fixture
// ============================================================================

/**
 * @brief Fixture that pre-loads message data into memory.
 *
 * This ensures we're measuring CPU parsing speed, not disk I/O.
 */
class ITCHParseFixture : public benchmark::Fixture {
public:
  void SetUp(const benchmark::State & /*state*/) override {
    // Create a buffer with multiple messages for realistic iteration
    constexpr size_t NUM_MESSAGES = 10000;
    buffer_.reserve(NUM_MESSAGES * 36);

    for (size_t i = 0; i < NUM_MESSAGES; ++i) {
      buffer_.insert(buffer_.end(), g_add_order_msg.begin(),
                     g_add_order_msg.end());
    }

    num_messages_ = NUM_MESSAGES;
  }

  void TearDown(const benchmark::State & /*state*/) override {
    buffer_.clear();
  }

protected:
  std::vector<char> buffer_;
  size_t num_messages_ = 0;
};

// ============================================================================
// Benchmark 1: Zero-Copy Implementation
// ============================================================================

/**
 * @brief Benchmark the zero-copy parser using reinterpret_cast.
 *
 * This is the optimized implementation that:
 * 1. Does NOT copy any data.
 * 2. Swaps bytes lazily on field access.
 * 3. Uses packed structs matching wire format.
 */
BENCHMARK_DEFINE_F(ITCHParseFixture, ZeroCopyParse)(benchmark::State &state) {
  // Dummy visitor that just counts (prevents optimization)
  struct CountingVisitor : itch::DefaultVisitor {
    uint64_t count = 0;
    uint64_t total_shares = 0;

    void on_add_order(const itch::AddOrder &msg) {
      ++count;
      total_shares += static_cast<uint32_t>(msg.shares);
    }
  };

  itch::Parser parser;

  for (auto _ : state) {
    CountingVisitor visitor;
    const char *ptr = buffer_.data();
    const char *end = ptr + buffer_.size();

    while (ptr < end) {
      auto result = parser.parse(ptr, 36, visitor);
      benchmark::DoNotOptimize(result);
      ptr += 36;
    }

    benchmark::DoNotOptimize(visitor.count);
    benchmark::DoNotOptimize(visitor.total_shares);
  }

  state.SetItemsProcessed(state.iterations() * num_messages_);
  state.SetBytesProcessed(state.iterations() * buffer_.size());
}

BENCHMARK_REGISTER_F(ITCHParseFixture, ZeroCopyParse)
    ->Unit(benchmark::kNanosecond)
    ->MinTime(1.0);

// ============================================================================
// Benchmark 2: Naive Implementation (Baseline)
// ============================================================================

/**
 * @brief Benchmark the naive parser using memcpy + ntohl.
 *
 * This simulates traditional parsing:
 * 1. memcpy bytes into local struct.
 * 2. Manually byte-swap each field.
 * 3. Struct has natural alignment (may have padding).
 */
BENCHMARK_DEFINE_F(ITCHParseFixture, NaiveParse)(benchmark::State &state) {
  uint64_t count = 0;
  uint64_t total_shares = 0;

  for (auto _ : state) {
    count = 0;
    total_shares = 0;
    const char *ptr = buffer_.data();
    const char *end = ptr + buffer_.size();

    NaiveAddOrder order;
    while (ptr < end) {
      parse_naive(ptr, order);
      benchmark::DoNotOptimize(order);
      ++count;
      total_shares += order.shares;
      ptr += 36;
    }

    benchmark::DoNotOptimize(count);
    benchmark::DoNotOptimize(total_shares);
  }

  state.SetItemsProcessed(state.iterations() * num_messages_);
  state.SetBytesProcessed(state.iterations() * buffer_.size());
}

BENCHMARK_REGISTER_F(ITCHParseFixture, NaiveParse)
    ->Unit(benchmark::kNanosecond)
    ->MinTime(1.0);

// ============================================================================
// Benchmark 3: Single Message Parse (Latency Focus)
// ============================================================================

/**
 * @brief Measure latency for parsing a single message.
 *
 * This isolates the per-message overhead without loop overhead.
 */
static void BM_SingleMessageZeroCopy(benchmark::State &state) {
  const char *msg_data = g_add_order_msg.data();
  const size_t msg_len = g_add_order_msg.size();

  struct MinimalVisitor : itch::DefaultVisitor {
    uint32_t shares = 0;
    void on_add_order(const itch::AddOrder &msg) {
      shares = static_cast<uint32_t>(msg.shares);
    }
  };

  itch::Parser parser;

  for (auto _ : state) {
    MinimalVisitor visitor;
    auto result = parser.parse(msg_data, msg_len, visitor);
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(visitor.shares);
  }

  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SingleMessageZeroCopy)->Unit(benchmark::kNanosecond);

static void BM_SingleMessageNaive(benchmark::State &state) {
  const char *msg_data = g_add_order_msg.data();
  NaiveAddOrder order;

  for (auto _ : state) {
    parse_naive(msg_data, order);
    benchmark::DoNotOptimize(order);
  }

  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SingleMessageNaive)->Unit(benchmark::kNanosecond);

// ============================================================================
// Benchmark 4: Raw Pointer Access (Best Case Baseline)
// ============================================================================

/**
 * @brief Measure raw reinterpret_cast overhead (no parsing logic).
 *
 * This shows the theoretical minimum latency for zero-copy access.
 */
static void BM_RawPointerAccess(benchmark::State &state) {
  const char *msg_data = g_add_order_msg.data();

  for (auto _ : state) {
    const auto *msg = reinterpret_cast<const itch::AddOrder *>(msg_data);
    uint32_t shares = msg->shares;
    uint32_t price = msg->price;
    benchmark::DoNotOptimize(shares);
    benchmark::DoNotOptimize(price);
  }

  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RawPointerAccess)->Unit(benchmark::kNanosecond);

} // anonymous namespace
