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

  size_t packet_count =
      reader.for_each_packet([&](const char *data, size_t len) {
        // Note: PCAP packet may contain Ethernet/IP/UDP headers
        // For raw ITCH data, we'd need to skip those headers
        // For now, we try to parse the payload directly
        (void)parser.parse_buffer(data, len, stats);
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
