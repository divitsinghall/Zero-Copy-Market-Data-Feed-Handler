/**
 * @file main.cpp
 * @brief PCAP-based ITCH 5.0 feed handler driver.
 *
 * Usage: ./itch_driver <pcap_file>
 *
 * This program demonstrates zero-copy ITCH message parsing from a PCAP file:
 * 1. mmap's the PCAP file into memory
 * 2. Iterates over packets, passing pointers directly to parser
 * 3. Collects statistics via visitor pattern
 */

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <itch/parser.hpp>
#include <itch/pcap_reader.hpp>

namespace {

// ============================================================================
// Statistics Visitor
// ============================================================================

/**
 * @brief Visitor that collects message statistics.
 */
struct StatsVisitor : itch::DefaultVisitor {
  uint64_t add_order_count = 0;
  uint64_t order_executed_count = 0;
  uint64_t system_event_count = 0;
  uint64_t unknown_count = 0;
  uint64_t total_shares = 0;
  uint64_t total_executions = 0;

  void on_add_order(const itch::AddOrder &msg) {
    ++add_order_count;
    total_shares += static_cast<uint32_t>(msg.shares);
  }

  void on_order_executed(const itch::OrderExecuted &msg) {
    ++order_executed_count;
    total_executions += static_cast<uint32_t>(msg.executed_shares);
  }

  void on_system_event(const itch::MessageHeader & /*msg*/) {
    ++system_event_count;
  }

  void on_unknown(char /*msg_type*/, const char * /*data*/, size_t /*len*/) {
    ++unknown_count;
  }

  [[nodiscard]] uint64_t total_messages() const {
    return add_order_count + order_executed_count + system_event_count +
           unknown_count;
  }

  void print_stats() const {
    std::printf("\n=== ITCH Message Statistics ===\n");
    std::printf("Add Orders:       %12" PRIu64 "\n", add_order_count);
    std::printf("Order Executed:   %12" PRIu64 "\n", order_executed_count);
    std::printf("System Events:    %12" PRIu64 "\n", system_event_count);
    std::printf("Unknown:          %12" PRIu64 "\n", unknown_count);
    std::printf("--------------------------------\n");
    std::printf("Total Messages:   %12" PRIu64 "\n", total_messages());
    std::printf("Total Shares:     %12" PRIu64 "\n", total_shares);
    std::printf("Total Executions: %12" PRIu64 "\n", total_executions);
  }
};

// ============================================================================
// Print Usage
// ============================================================================

void print_usage(const char *program) {
  std::fprintf(stderr, "Usage: %s <pcap_file>\n", program);
  std::fprintf(stderr, "\nZero-copy ITCH 5.0 feed handler.\n");
  std::fprintf(stderr, "Parses NASDAQ ITCH messages from a PCAP file.\n");
}

} // anonymous namespace

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
  // Parse arguments
  if (argc != 2) {
    print_usage(argv[0]);
    return 1;
  }

  const char *pcap_file = argv[1];

  // Open PCAP file
  std::printf("Opening PCAP file: %s\n", pcap_file);
  itch::PcapReader reader(pcap_file);

  if (!reader.is_open()) {
    std::fprintf(stderr, "Error: Failed to open PCAP file: %s\n", pcap_file);
    return 1;
  }

  std::printf("File size: %.2f MB\n", reader.file_size() / (1024.0 * 1024.0));

  // Prepare parser and visitor
  itch::Parser parser;
  StatsVisitor stats;

  // Process packets
  std::printf("Processing packets...\n");

  auto start_time = std::chrono::high_resolution_clock::now();

  // ============================================================================
  // Network Header Offset Heuristics
  // ============================================================================
  //
  // Standard network headers before ITCH payload:
  //   - Ethernet: 14 bytes
  //   - IP:       20 bytes
  //   - UDP:      8 bytes
  //   - Total:    42 bytes
  //
  // Some captures may have VLAN tags (+4 bytes) or other variations.
  // We use a heuristic: search for valid ITCH message type in first 64 bytes.
  // ============================================================================

  // Valid ITCH message types
  auto is_valid_itch_type = [](char c) {
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
  };

  // Find ITCH payload offset within a packet
  auto find_itch_offset = [&is_valid_itch_type](const char *data,
                                                size_t len) -> size_t {
    // Common header configurations:
    // 1. Standard: Ethernet(14) + IP(20) + UDP(8) = 42 bytes
    // 2. With VLAN: Ethernet(14) + VLAN(4) + IP(20) + UDP(8) = 46 bytes
    // 3. MoldUDP64: ... + Session(10) + Seq(8) + Count(2) = +20 bytes
    // 4. Message length prefix: +2 bytes before each ITCH message
    //
    // Common offsets to try (in order):
    constexpr size_t OFFSETS[] = {
        42, // Standard UDP
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
          // Even without stock_locate validation, valid message type is good
          return offset;
        }
      }
    }

    // Fallback: Search first 100 bytes for valid ITCH message type
    constexpr size_t SEARCH_LIMIT = 100;
    size_t search_end = (len < SEARCH_LIMIT) ? len : SEARCH_LIMIT;

    for (size_t offset = 0; offset < search_end; ++offset) {
      char msg_type = data[offset];
      if (is_valid_itch_type(msg_type)) {
        // Validate with stock_locate
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

    // Last resort: return 42 and let parser handle it
    return 42;
  };

  size_t packet_count =
      reader.for_each_packet([&](const char *data, size_t len) {
        // Find ITCH payload offset (skip network headers)
        size_t offset = find_itch_offset(data, len);

        if (offset < len) {
          const char *itch_data = data + offset;
          size_t itch_len = len - offset;
          (void)parser.parse_buffer(itch_data, itch_len, stats);
        }
      });

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time);

  // Print results
  std::printf("\n=== Performance ===\n");
  std::printf("Packets processed: %zu\n", packet_count);
  std::printf("Time: %.3f ms\n", duration.count() / 1000.0);

  if (duration.count() > 0) {
    double packets_per_sec = packet_count * 1e6 / duration.count();
    double mb_per_sec =
        reader.file_size() / (1024.0 * 1024.0) * 1e6 / duration.count();
    std::printf("Throughput: %.2f million packets/sec\n",
                packets_per_sec / 1e6);
    std::printf("Bandwidth: %.2f MB/sec\n", mb_per_sec);
  }

  stats.print_stats();

  return 0;
}
