#pragma once

/**
 * @file parser.hpp
 * @brief Zero-overhead ITCH 5.0 message dispatcher with visitor pattern.
 *
 * DESIGN PRINCIPLES:
 * 1. No virtual functions - templates enable full inlining.
 * 2. Switch-case dispatch compiles to efficient jump table.
 * 3. Visitor pattern allows caller to handle only messages they care about.
 * 4. All parsing is zero-copy via reinterpret_cast.
 *
 * USAGE:
 *   struct MyHandler {
 *       void on_add_order(const AddOrder& msg) { ... }
 *       void on_order_executed(const OrderExecuted& msg) { ... }
 *       void on_unknown(char msg_type, const char* data, size_t len) { ... }
 *   };
 *
 *   Parser<MyHandler> parser(handler);
 *   parser.parse(buffer, length);
 */

#include "messages.hpp"
#include <cstddef>

namespace itch {

// ============================================================================
// Parse Result
// ============================================================================

/**
 * @brief Result of parsing a single message.
 */
enum class ParseResult : uint8_t {
  Ok,             ///< Message parsed successfully
  BufferTooSmall, ///< Buffer smaller than message header
  UnknownType,    ///< Unknown message type (still dispatched to on_unknown)
  InvalidLength   ///< Message length doesn't match expected size
};

// ============================================================================
// Default Visitor (No-op handlers)
// ============================================================================

/**
 * @brief Base visitor with no-op handlers.
 *
 * Derive from this to only implement handlers you care about.
 * All methods are intentionally empty - compiler will optimize away.
 */
struct DefaultVisitor {
  // System messages
  void on_system_event(const MessageHeader & /*msg*/) {}

  // Order messages
  void on_add_order(const AddOrder & /*msg*/) {}
  void on_order_executed(const OrderExecuted & /*msg*/) {}

  // Called for unhandled message types
  void on_unknown(char /*msg_type*/, const char * /*data*/, size_t /*len*/) {}
};

// ============================================================================
// Message Size Lookup
// ============================================================================

/**
 * @brief Get expected message size for a given type.
 *
 * Returns 0 for unknown types.
 */
[[nodiscard]] constexpr size_t get_message_size(char msg_type) noexcept {
  switch (msg_type) {
  // Order messages
  case msg_type::AddOrder:
    return sizeof(AddOrder);
  case msg_type::OrderExecuted:
    return sizeof(OrderExecuted);

  // System messages (just header for now)
  case msg_type::SystemEvent:
    return sizeof(MessageHeader);

    // TODO: Add more message types as implemented

  default:
    return 0; // Unknown type
  }
}

// ============================================================================
// Parser Class
// ============================================================================

/**
 * @brief High-performance ITCH message parser with visitor dispatch.
 *
 * @tparam Visitor Handler class with on_xxx methods for each message type.
 *                 Use DefaultVisitor as base to only handle specific messages.
 *
 * The visitor pattern with templates allows:
 * 1. Zero virtual function overhead - calls are statically resolved.
 * 2. Full inlining of handler code into the parse loop.
 * 3. Dead code elimination for unused message handlers.
 *
 * @example
 *   struct OrderHandler : itch::DefaultVisitor {
 *       uint64_t order_count = 0;
 *       void on_add_order(const AddOrder& msg) {
 *           ++order_count;
 *       }
 *   };
 *
 *   OrderHandler handler;
 *   itch::Parser parser;
 *   parser.parse(buffer, length, handler);
 */
class Parser {
public:
  /**
   * @brief Parse a single ITCH message and dispatch to visitor.
   *
   * @tparam Visitor Handler type with on_xxx methods.
   * @param buffer Raw message buffer (must remain valid during call).
   * @param length Buffer length in bytes.
   * @param visitor Handler instance to receive parsed message.
   * @return ParseResult indicating success or failure mode.
   *
   * @note This parses ONE message. For a stream, call repeatedly.
   */
  template <typename Visitor>
  [[nodiscard]] ParseResult parse(const char *buffer, size_t length,
                                  Visitor &visitor) const noexcept {
    // Minimum size check (need at least message type byte)
    if (length < sizeof(MessageHeader)) {
      return ParseResult::BufferTooSmall;
    }

    // Read message type (first byte)
    const char msg_type = buffer[0];

    // Dispatch based on message type
    // Using switch-case compiles to efficient jump table
    // Branch hints: AddOrder is most common (~70% of messages), SystemEvent is
    // rare
    switch (msg_type) {
    [[likely]] case msg_type::AddOrder: {
      if (length < sizeof(AddOrder)) [[unlikely]] {
        return ParseResult::BufferTooSmall;
      }
      const auto *msg = reinterpret_cast<const AddOrder *>(buffer);
      visitor.on_add_order(*msg);
      return ParseResult::Ok;
    }

    case msg_type::OrderExecuted: {
      if (length < sizeof(OrderExecuted)) [[unlikely]] {
        return ParseResult::BufferTooSmall;
      }
      const auto *msg = reinterpret_cast<const OrderExecuted *>(buffer);
      visitor.on_order_executed(*msg);
      return ParseResult::Ok;
    }

    [[unlikely]] case msg_type::SystemEvent: {
      if (length < sizeof(MessageHeader)) [[unlikely]] {
        return ParseResult::BufferTooSmall;
      }
      const auto *msg = reinterpret_cast<const MessageHeader *>(buffer);
      visitor.on_system_event(*msg);
      return ParseResult::Ok;
    }

    default:
      // Unknown message type - still dispatch to on_unknown
      visitor.on_unknown(msg_type, buffer, length);
      return ParseResult::UnknownType;
    }
  }

  /**
   * @brief Parse multiple ITCH messages from a buffer.
   *
   * Parses all complete messages in the buffer, stopping when:
   * - Buffer is exhausted
   * - An error occurs
   * - Unknown message type encountered (can't determine size)
   *
   * @tparam Visitor Handler type with on_xxx methods.
   * @param buffer Raw message buffer.
   * @param length Total buffer length.
   * @param visitor Handler to receive parsed messages.
   * @return Number of bytes successfully consumed.
   */
  template <typename Visitor>
  [[nodiscard]] size_t parse_buffer(const char *buffer, size_t length,
                                    Visitor &visitor) const noexcept {
    size_t consumed = 0;

    while (consumed < length) {
      const char *current = buffer + consumed;
      const size_t remaining = length - consumed;

      // Need at least 1 byte for message type
      if (remaining < 1) {
        break;
      }

      // Get message size
      const char msg_type = current[0];
      const size_t msg_size = get_message_size(msg_type);

      // Unknown type - can't continue (don't know size)
      if (msg_size == 0) {
        visitor.on_unknown(msg_type, current, remaining);
        break;
      }

      // Incomplete message - stop here
      if (remaining < msg_size) {
        break;
      }

      // Parse this message
      ParseResult result = parse(current, msg_size, visitor);
      if (result != ParseResult::Ok && result != ParseResult::UnknownType) {
        break;
      }

      consumed += msg_size;
    }

    return consumed;
  }
};

// ============================================================================
// Convenience Function
// ============================================================================

/**
 * @brief Parse a single message with a visitor (free function).
 *
 * @tparam Visitor Handler type.
 * @param buffer Message buffer.
 * @param length Buffer length.
 * @param visitor Handler instance.
 * @return ParseResult.
 */
template <typename Visitor>
[[nodiscard]] inline ParseResult
parse_message(const char *buffer, size_t length, Visitor &visitor) noexcept {
  Parser parser;
  return parser.parse(buffer, length, visitor);
}

} // namespace itch
