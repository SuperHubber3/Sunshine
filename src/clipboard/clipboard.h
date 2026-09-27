/**
 * @file src/clipboard/clipboard.h
 * @brief Declarations for clipboard sync with streaming clients.
 */
#pragma once

// standard includes
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

// local includes
#include "clipboard_sync.h"
#include "src/platform/common.h"

namespace clipboard {

  /**
   * @brief Check whether clipboard sync is enabled and supported on this platform.
   * @return True if clients may sync their clipboard.
   */
  bool supported();

  /**
   * @brief Clipboard sync with the client of one streaming session.
   *
   * Messages from the client are handled on the control stream thread, and messages to the client
   * are queued for the control stream thread to send.
   */
  class session_t {
  public:
    /**
     * @brief Create clipboard sync for a session.
     * @return Session state, or nullptr if clipboard sync is unsupported or disabled.
     */
    static std::unique_ptr<session_t> create();

    ~session_t();

    /**
     * @brief Handle a clipboard message from the client.
     *
     * @param message Message payload.
     */
    void on_message(std::string_view message);

    /**
     * @brief Take the next message to send to the client.
     *
     * @param message Receives the message.
     * @return True if a message was taken.
     */
    bool pop_outgoing(std::vector<uint8_t> &message);

    /**
     * @brief Check whether messages are waiting or a transfer is in progress.
     * @return True while the control stream should be serviced frequently.
     */
    bool busy();

  private:
    session_t() = default;

    std::shared_ptr<platf::clipboard_t> platform;
    int listener_id = -1;
    std::unique_ptr<clipboard_sync::engine_t> engine;

    std::mutex outgoing_mutex;
    std::deque<std::vector<uint8_t>> outgoing;
  };

}  // namespace clipboard
