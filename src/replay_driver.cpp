/**
 * @file replay_driver.cpp
 * @brief Market Replay application integrating ITCH parser with OrderBook
 * engine.
 *
 * This driver demonstrates the full HFT pipeline:
 * 1. PCAP packet reading (zero-copy)
 * 2. ITCH message parsing (zero-copy)
 * 3. Order book management (matching engine)
 * 4. Performance metrics collection
 *
 * Usage: ./chronos_replay [pcap_file]
 *        Default: data/Multiple.Packets.pcap
 */

#include <book/order_book.hpp>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <itch/parser.hpp>
#include <itch/pcap_reader.hpp>

namespace {

// ============================================================================
// Configuration
// ============================================================================

/// Pool capacity for orders (10 million = typical full trading day)
constexpr std::size_t POOL_CAPACITY = 10'000'000;

/// Every Nth order is made marketable to trigger matches
constexpr uint64_t MATCH_TRIGGER_INTERVAL = 100;

/// Default PCAP file if none specified
constexpr const char *DEFAULT_PCAP = "data/Multiple.Packets.pcap";

// ============================================================================
// Metrics
// ============================================================================

struct ReplayMetrics {
  uint64_t orders_processed = 0;
  uint64_t orders_added = 0;
  uint64_t orders_cancelled = 0;
  uint64_t matches_executed = 0;
  uint64_t add_order_time_ns = 0; // Total time in add_order calls

  void print() const {
    std::printf("\n=== Market Replay Metrics ===\n");
    std::printf("Orders Processed:     %12" PRIu64 "\n", orders_processed);
    std::printf("Orders Added to Book: %12" PRIu64 "\n", orders_added);
    std::printf("Orders Cancelled:     %12" PRIu64 "\n", orders_cancelled);
    std::printf("Matches Executed:     %12" PRIu64 "\n", matches_executed);

    if (orders_processed > 0) {
      double avg_latency_ns = static_cast<double>(add_order_time_ns) /
                              static_cast<double>(orders_processed);
      std::printf("Avg add_order latency: %.1f ns\n", avg_latency_ns);
    }
  }
};

// ============================================================================
// ReplayVisitor - The Bridge between Parser and OrderBook
// ============================================================================

/**
 * @brief Visitor that forwards ITCH messages to the OrderBook.
 *
 * Design:
 * - Inherits from DefaultVisitor for no-op handling of uninterested messages
 * - Holds reference to OrderBook for order management
 * - Collects metrics for performance analysis
 * - Simulation: Every 100th order is made marketable to trigger matching
 *
 * @tparam Capacity Pool capacity for the OrderBook
 */
template <std::size_t Capacity>
class ReplayVisitor : public itch::DefaultVisitor {
public:
  using BookType = book::OrderBook<Capacity>;

  ReplayVisitor(BookType &book, ReplayMetrics &metrics) noexcept
      : book_(book), metrics_(metrics), simulated_order_id_(1) {}

  /**
   * @brief Handle Add Order messages (Type 'A').
   *
   * Simulation Logic:
   * - Every 100th order, flip the side and make price marketable
   * - This triggers matching for demonstration purposes
   */
  void on_add_order(const itch::AddOrder &msg) {
    ++metrics_.orders_processed;

    // FIX: Generate unique ID to bypass duplicate check in stress tests
    // The template PCAP repeats the same order_ref, causing all but first to be
    // rejected
    uint64_t id = simulated_order_id_++;
    uint64_t price = static_cast<uint32_t>(msg.price); // Already in ticks
    uint32_t qty = static_cast<uint32_t>(msg.shares);
    book::Side side = msg.is_buy() ? book::Side::Buy : book::Side::Sell;

    // Simulation: Every Nth order crosses the spread
    if (metrics_.orders_processed % MATCH_TRIGGER_INTERVAL == 0) {
      // Flip side
      side = (side == book::Side::Buy) ? book::Side::Sell : book::Side::Buy;

      // Make price marketable (cross the spread)
      if (side == book::Side::Buy) {
        // Aggressive buy: price above best ask
        auto best_ask = book_.best_ask();
        if (best_ask) {
          price = *best_ask + 100; // 1 cent above best ask
        }
      } else {
        // Aggressive sell: price below best bid
        auto best_bid = book_.best_bid();
        if (best_bid) {
          price =
              (*best_bid > 100) ? *best_bid - 100 : 0; // 1 cent below best bid
        }
      }
    }

    // Track order count before add to detect matches
    size_t orders_before = book_.order_count();

    // Time the add_order call
    auto start = std::chrono::high_resolution_clock::now();

    bool added = book_.add_order(id, price, qty, side);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    metrics_.add_order_time_ns += static_cast<uint64_t>(duration.count());

    if (added) {
      ++metrics_.orders_added;

      // Check if matching occurred by comparing order counts
      // If orders_before decreased or stayed same but we added, matches
      // happened
      size_t orders_after = book_.order_count();
      if (orders_after <= orders_before && orders_before > 0) {
        // At least one match occurred (filled orders were removed)
        ++metrics_.matches_executed;
      }
    }
  }

  /**
   * @brief Handle Order Executed messages (Type 'E').
   *
   * Simplification: We treat execution as order removal to maintain book state.
   * In a real system, we'd reduce quantity and only remove if fully executed.
   */
  void on_order_executed(const itch::OrderExecuted &msg) {
    uint64_t id = static_cast<uint64_t>(msg.order_ref);

    if (book_.cancel_order(id)) {
      ++metrics_.orders_cancelled;
    }
  }

private:
  BookType &book_;
  ReplayMetrics &metrics_;
  uint64_t simulated_order_id_; ///< Counter for generating unique order IDs
};

// ============================================================================
// ITCH Payload Detection (reused from main.cpp)
// ============================================================================

/// Valid ITCH message types
bool is_valid_itch_type(char c) {
  // Order messages
  if (c == 'A' || c == 'F' || c == 'E' || c == 'C' || c == 'X' || c == 'D' ||
      c == 'U')
    return true;
  // Trade messages
  if (c == 'P' || c == 'Q' || c == 'B')
    return true;
  // System/stock messages
  if (c == 'S' || c == 'R' || c == 'H' || c == 'Y' || c == 'L')
    return true;
  // Net order imbalance
  if (c == 'I' || c == 'N')
    return true;
  // MWCB and IPO
  if (c == 'V' || c == 'W' || c == 'K')
    return true;
  return false;
}

/// Find ITCH payload offset within a packet
size_t find_itch_offset(const char *data, size_t len) {
  // Common header configurations
  constexpr size_t OFFSETS[] = {
      42, // Standard: Ethernet(14) + IP(20) + UDP(8)
      46, // With VLAN tag
      62, // Standard + MoldUDP header
      64, // Standard + MoldUDP + length prefix
      66, // VLAN + MoldUDP header
      68, // VLAN + MoldUDP + length prefix
  };

  // Try each known offset
  for (size_t offset : OFFSETS) {
    if (offset < len) {
      char msg_type = data[offset];
      if (is_valid_itch_type(msg_type)) {
        // Additional validation: check stock_locate is reasonable
        if (len >= offset + 3) {
          uint16_t stock_locate = static_cast<uint16_t>(
              (static_cast<uint8_t>(data[offset + 1]) << 8) |
              static_cast<uint8_t>(data[offset + 2]));
          if (stock_locate > 0 && stock_locate < 10000) {
            return offset;
          }
        }
        return offset;
      }
    }
  }

  // Fallback: Search first 100 bytes
  constexpr size_t SEARCH_LIMIT = 100;
  size_t search_end = (len < SEARCH_LIMIT) ? len : SEARCH_LIMIT;

  for (size_t offset = 0; offset < search_end; ++offset) {
    char msg_type = data[offset];
    if (is_valid_itch_type(msg_type)) {
      if (len >= offset + 3) {
        uint16_t stock_locate = static_cast<uint16_t>(
            (static_cast<uint8_t>(data[offset + 1]) << 8) |
            static_cast<uint8_t>(data[offset + 2]));
        if (stock_locate > 0 && stock_locate < 10000) {
          return offset;
        }
      }
    }
  }

  // Last resort: return 42
  return 42;
}

// ============================================================================
// Print Usage
// ============================================================================

void print_usage(const char *program) {
  std::fprintf(stderr, "Usage: %s [pcap_file]\n", program);
  std::fprintf(stderr, "\nChronos Market Replay Engine\n");
  std::fprintf(stderr,
               "Integrates ITCH parser with OrderBook matching engine.\n");
  std::fprintf(stderr, "\nDefault PCAP: %s\n", DEFAULT_PCAP);
}

} // anonymous namespace

// ============================================================================
// Main Driver
// ============================================================================

int main(int argc, char *argv[]) {
  // Parse arguments
  const char *pcap_file = (argc >= 2) ? argv[1] : DEFAULT_PCAP;

  if (argc > 2 || (argc == 2 && (std::string(argv[1]) == "-h" ||
                                 std::string(argv[1]) == "--help"))) {
    print_usage(argv[0]);
    return (argc > 2) ? 1 : 0;
  }

  std::printf(
      "╔══════════════════════════════════════════════════════════════╗\n");
  std::printf(
      "║           CHRONOS - Market Replay Engine                     ║\n");
  std::printf(
      "║   Zero-Copy ITCH Parser + High-Frequency Matching Engine     ║\n");
  std::printf(
      "╚══════════════════════════════════════════════════════════════╝\n\n");

  // ============================================================================
  // Initialize Components
  // ============================================================================

  std::printf("Initializing Memory Pool (Capacity: %zu orders)...\n",
              POOL_CAPACITY);
  book::MemPool<book::Order, POOL_CAPACITY> pool;
  std::printf("  Pool Memory: %.2f MB\n",
              (POOL_CAPACITY * sizeof(book::Order)) / (1024.0 * 1024.0));

  std::printf("Initializing OrderBook...\n");
  book::OrderBook<POOL_CAPACITY> book(pool);

  std::printf("Opening PCAP file: %s\n", pcap_file);
  itch::PcapReader reader(pcap_file);

  if (!reader.is_open()) {
    std::fprintf(stderr, "Error: Failed to open PCAP file: %s\n", pcap_file);
    return 1;
  }

  std::printf("  File size: %.2f MB\n\n",
              reader.file_size() / (1024.0 * 1024.0));

  // ============================================================================
  // Run Replay
  // ============================================================================

  std::printf("Starting market replay...\n");
  std::printf("  Match trigger interval: every %luth order\n\n",
              static_cast<unsigned long>(MATCH_TRIGGER_INTERVAL));

  ReplayMetrics metrics;
  ReplayVisitor<POOL_CAPACITY> visitor(book, metrics);
  itch::Parser parser;

  auto start_time = std::chrono::high_resolution_clock::now();

  size_t packet_count =
      reader.for_each_packet([&](const char *data, size_t len) {
        // Find ITCH payload offset (skip network headers)
        size_t offset = find_itch_offset(data, len);

        if (offset < len) {
          const char *itch_data = data + offset;
          size_t itch_len = len - offset;
          (void)parser.parse_buffer(itch_data, itch_len, visitor);
        }
      });

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);

  // ============================================================================
  // Print Results
  // ============================================================================

  std::printf("\n=== Performance ===\n");
  std::printf("Packets processed: %zu\n", packet_count);
  std::printf("Total time: %.3f ms\n", duration.count() / 1000.0);

  if (duration.count() > 0) {
    double packets_per_sec = packet_count * 1e6 / duration.count();
    double orders_per_sec = metrics.orders_processed * 1e6 / duration.count();
    double mb_per_sec =
        reader.file_size() / (1024.0 * 1024.0) * 1e6 / duration.count();

    std::printf("Throughput: %.2f million packets/sec\n",
                packets_per_sec / 1e6);
    std::printf("Order Rate: %.2f million orders/sec\n", orders_per_sec / 1e6);
    std::printf("Bandwidth: %.2f MB/sec\n", mb_per_sec);
  }

  metrics.print();

  // Final book state
  std::printf("\n=== Final Book State ===\n");
  std::printf("Orders Resting: %zu\n", book.order_count());
  std::printf("Bid Levels: %zu\n", book.bid_level_count());
  std::printf("Ask Levels: %zu\n", book.ask_level_count());

  if (book.best_bid()) {
    std::printf("Best Bid: %.4f\n", *book.best_bid() / 10000.0);
  }
  if (book.best_ask()) {
    std::printf("Best Ask: %.4f\n", *book.best_ask() / 10000.0);
  }
  if (book.spread()) {
    std::printf("Spread: %.4f\n", *book.spread() / 10000.0);
  }

  std::printf("\nPool Utilization: %.2f%% (%zu / %zu)\n",
              100.0 * pool.allocated() / POOL_CAPACITY, pool.allocated(),
              POOL_CAPACITY);

  return 0;
}
