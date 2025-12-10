#pragma once

/**
 * @file pcap_reader.hpp
 * @brief Zero-copy PCAP file reader using mmap.
 *
 * DESIGN PRINCIPLES:
 * 1. No libpcap dependency - manual header parsing.
 * 2. mmap entire file for zero-copy access.
 * 3. Direct pointer passing to parser (no memcpy).
 *
 * PCAP File Format:
 *   Global Header: 24 bytes (magic, version, snaplen, etc.)
 *   For each packet:
 *     Packet Header: 16 bytes (ts_sec, ts_usec, incl_len, orig_len)
 *     Packet Data: incl_len bytes
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace itch {

// ============================================================================
// PCAP Header Structures
// ============================================================================

/**
 * @brief PCAP Global Header (24 bytes).
 */
struct __attribute__((packed)) PcapGlobalHeader {
  uint32_t magic_number;  // 0xa1b2c3d4 (native) or 0xd4c3b2a1 (swapped)
  uint16_t version_major; // 2
  uint16_t version_minor; // 4
  int32_t thiszone;       // GMT offset (usually 0)
  uint32_t sigfigs;       // Accuracy of timestamps (usually 0)
  uint32_t snaplen;       // Max length of captured packets
  uint32_t network;       // Data link type
};

static_assert(sizeof(PcapGlobalHeader) == 24,
              "PcapGlobalHeader must be 24 bytes");

/**
 * @brief PCAP Packet Header (16 bytes).
 */
struct __attribute__((packed)) PcapPacketHeader {
  uint32_t ts_sec;   // Timestamp seconds
  uint32_t ts_usec;  // Timestamp microseconds
  uint32_t incl_len; // Number of bytes captured
  uint32_t orig_len; // Original packet length
};

static_assert(sizeof(PcapPacketHeader) == 16,
              "PcapPacketHeader must be 16 bytes");

// ============================================================================
// PCAP Reader Class
// ============================================================================

/**
 * @brief Memory-mapped PCAP file reader.
 *
 * Opens a PCAP file, mmaps it into memory, and provides iteration
 * over packet payloads with zero-copy semantics.
 *
 * @example
 *   PcapReader reader("data.pcap");
 *   if (!reader.is_open()) { error... }
 *
 *   reader.for_each_packet([&](const char* data, size_t len) {
 *       parser.parse(data, len, handler);
 *   });
 */
class PcapReader {
public:
  PcapReader() = default;

  explicit PcapReader(const char *filename) { open(filename); }

  ~PcapReader() { close(); }

  // Non-copyable (owns mmap'd memory)
  PcapReader(const PcapReader &) = delete;
  PcapReader &operator=(const PcapReader &) = delete;

  // Movable
  PcapReader(PcapReader &&other) noexcept
      : data_(other.data_), size_(other.size_), fd_(other.fd_),
        needs_swap_(other.needs_swap_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
  }

  PcapReader &operator=(PcapReader &&other) noexcept {
    if (this != &other) {
      close();
      data_ = other.data_;
      size_ = other.size_;
      fd_ = other.fd_;
      needs_swap_ = other.needs_swap_;
      other.data_ = nullptr;
      other.size_ = 0;
      other.fd_ = -1;
    }
    return *this;
  }

  /**
   * @brief Open and mmap a PCAP file.
   * @param filename Path to PCAP file.
   * @return true if successful.
   */
  bool open(const char *filename) {
    close();

    // Open file
    fd_ = ::open(filename, O_RDONLY);
    if (fd_ < 0) {
      return false;
    }

    // Get file size
    struct stat st;
    if (fstat(fd_, &st) < 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    size_ = static_cast<size_t>(st.st_size);

    // Memory map the file
    data_ = static_cast<const char *>(
        mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
    if (data_ == MAP_FAILED) {
      ::close(fd_);
      fd_ = -1;
      data_ = nullptr;
      size_ = 0;
      return false;
    }

    // Verify and parse global header
    if (size_ < sizeof(PcapGlobalHeader)) {
      close();
      return false;
    }

    const auto *global_header =
        reinterpret_cast<const PcapGlobalHeader *>(data_);

    // Check magic number
    // Standard PCAP (microsecond): 0xa1b2c3d4 (native) or 0xd4c3b2a1 (swapped)
    // Nanosecond PCAP:             0xa1b23c4d (native) or 0x4d3cb2a1 (swapped)
    const uint32_t magic = global_header->magic_number;
    if (magic == 0xa1b2c3d4 || magic == 0xa1b23c4d) {
      needs_swap_ = false; // Native byte order
    } else if (magic == 0xd4c3b2a1 || magic == 0x4d3cb2a1) {
      needs_swap_ = true; // Need to swap bytes
    } else {
      close(); // Invalid PCAP file
      return false;
    }

    return true;
  }

  /**
   * @brief Close the file and unmap memory.
   */
  void close() {
    if (data_ != nullptr) {
      munmap(const_cast<char *>(data_), size_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    size_ = 0;
  }

  /**
   * @brief Check if file is open.
   */
  [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }

  /**
   * @brief Get file size.
   */
  [[nodiscard]] size_t file_size() const noexcept { return size_; }

  /**
   * @brief Iterate over all packet payloads.
   *
   * @tparam Callback Function with signature void(const char* data, size_t len)
   * @param callback Called for each packet's payload.
   * @return Number of packets processed.
   */
  template <typename Callback>
  size_t for_each_packet(Callback &&callback) const {
    if (!is_open()) {
      return 0;
    }

    size_t offset = sizeof(PcapGlobalHeader);
    size_t packet_count = 0;

    while (offset + sizeof(PcapPacketHeader) <= size_) {
      const auto *pkt_header =
          reinterpret_cast<const PcapPacketHeader *>(data_ + offset);

      uint32_t incl_len = pkt_header->incl_len;
      if (needs_swap_) {
        incl_len = __builtin_bswap32(incl_len);
      }

      offset += sizeof(PcapPacketHeader);

      // Check if payload fits in file
      if (offset + incl_len > size_) {
        break; // Truncated packet
      }

      // Pass payload directly to callback (zero-copy!)
      const char *payload = data_ + offset;
      callback(payload, incl_len);

      offset += incl_len;
      ++packet_count;
    }

    return packet_count;
  }

  /**
   * @brief Get raw mmap'd data pointer.
   */
  [[nodiscard]] const char *data() const noexcept { return data_; }

private:
  const char *data_ = nullptr;
  size_t size_ = 0;
  int fd_ = -1;
  bool needs_swap_ = false;
};

} // namespace itch
