/**
 * @file src/clipboard/clipboard.cpp
 * @brief Definitions for clipboard sync with streaming clients.
 */
// standard includes
#include <atomic>

// local includes
#include "clipboard.h"
#include "src/config.h"
#include "src/logging.h"

namespace clipboard {

  bool supported() {
    return config::input.clipboard && platf::clipboard() != nullptr;
  }

  std::unique_ptr<session_t> session_t::create() {
    if (!config::input.clipboard) {
      return nullptr;
    }

    auto platform = platf::clipboard();
    if (!platform) {
      return nullptr;
    }

    std::unique_ptr<session_t> session {new session_t()};
    session->platform = platform;

    // Each session gets its own directory for received files, since sessions can run concurrently
    static std::atomic<uint32_t> session_counter {0};
    auto cache_dir = platform->cache_dir() / ("session-" + std::to_string(++session_counter));

    clipboard_sync::callbacks_t callbacks;
    callbacks.send = [raw = session.get()](const uint8_t *data, size_t size) {
      std::lock_guard lg(raw->outgoing_mutex);
      raw->outgoing.emplace_back(data, data + size);
      return true;
    };
    callbacks.set_local = [platform](const clipboard_sync::content_t &content) {
      platform->set(content);
    };
    callbacks.log = [](clipboard_sync::log_level_e level, const std::string &message) {
      switch (level) {
        case clipboard_sync::log_level_e::debug:
          BOOST_LOG(debug) << message;
          break;
        case clipboard_sync::log_level_e::info:
          BOOST_LOG(info) << message;
          break;
        case clipboard_sync::log_level_e::warning:
          BOOST_LOG(warning) << message;
          break;
        case clipboard_sync::log_level_e::error:
          BOOST_LOG(error) << message;
          break;
      }
    };

    session->engine = std::make_unique<clipboard_sync::engine_t>(callbacks, cache_dir, clipboard_sync::limits_t {}, LI_CLIPBOARD_MESSAGE_MAX);
    session->listener_id = platform->add_listener([engine = session->engine.get()](const clipboard_sync::content_t &content) {
      engine->on_local_changed(content);
    });

    return session;
  }

  session_t::~session_t() {
    // The listener calls into the engine, so it's removed first
    platform->remove_listener(listener_id);
    engine.reset();
  }

  void session_t::on_message(std::string_view message) {
    engine->on_message((const uint8_t *) message.data(), message.size());
  }

  bool session_t::pop_outgoing(std::vector<uint8_t> &message) {
    std::lock_guard lg(outgoing_mutex);
    if (outgoing.empty()) {
      return false;
    }
    message = std::move(outgoing.front());
    outgoing.pop_front();
    return true;
  }

  bool session_t::busy() {
    std::lock_guard lg(outgoing_mutex);
    return !outgoing.empty() || engine->busy();
  }

}  // namespace clipboard
