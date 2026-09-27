/**
 * @file src/clipboard/clipboard.cpp
 * @brief Definitions for clipboard sync with streaming clients.
 */
// standard includes
#include <atomic>
#include <set>

// local includes
#include "clipboard.h"
#include "src/config.h"
#include "src/logging.h"

namespace fs = std::filesystem;

namespace clipboard {

  namespace {
    // Messages are dropped beyond this, which only a misbehaving client can cause, since it pulls
    // contents in small ranges
    constexpr size_t MAX_OUTGOING_BYTES = 8 << 20;

    std::mutex active_dirs_mutex;
    std::set<fs::path> active_dirs;  ///< Directories of running sessions.

    /**
     * @brief Remove the directories of sessions that ended, keeping the files of running sessions.
     *
     * The files of the last session stay around until the next one starts, in case they're still
     * being pasted.
     */
    void remove_stale_dirs(const fs::path &base) {
      try {
        std::error_code ec;
        for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
          auto name = clipboard_sync::path_to_utf8(it->path().filename());
          if (name.rfind("session-", 0) == 0 && !active_dirs.count(it->path())) {
            std::error_code remove_ec;
            fs::remove_all(it->path(), remove_ec);
          }
        }
      } catch (const std::exception &e) {
        BOOST_LOG(warning) << "Clipboard: couldn't remove old files: " << e.what();
      }
    }
  }  // namespace

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
    auto base_dir = platform->cache_dir();
    session->cache_dir = base_dir / ("session-" + std::to_string(++session_counter));
    {
      std::lock_guard lg(active_dirs_mutex);
      active_dirs.insert(session->cache_dir);
      platform->run_as_user([&]() {
        remove_stale_dirs(base_dir);
      });
    }

    clipboard_sync::callbacks_t callbacks;
    callbacks.send = [raw = session.get()](const uint8_t *data, size_t size) {
      std::lock_guard lg(raw->outgoing_mutex);
      if (raw->outgoing_bytes + size > MAX_OUTGOING_BYTES) {
        return false;
      }
      raw->outgoing.emplace_back(data, data + size);
      raw->outgoing_bytes += size;
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

    callbacks.run_as_user = [platform](const std::function<void()> &work) {
      platform->run_as_user(work);
    };

    session->engine = std::make_unique<clipboard_sync::engine_t>(callbacks, session->cache_dir, clipboard_sync::limits_t {}, LI_CLIPBOARD_MESSAGE_MAX);
    session->listener_id = platform->add_listener([engine = session->engine.get()](const clipboard_sync::content_t &content) {
      engine->on_local_changed(content);
    });

    return session;
  }

  session_t::~session_t() {
    // The listener calls into the engine, so it's removed first
    platform->remove_listener(listener_id);
    engine.reset();

    std::lock_guard lg(active_dirs_mutex);
    active_dirs.erase(cache_dir);
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
    outgoing_bytes -= message.size();
    return true;
  }

  size_t session_t::next_outgoing_size() {
    std::lock_guard lg(outgoing_mutex);
    return outgoing.empty() ? 0 : outgoing.front().size();
  }

  bool session_t::busy() {
    std::lock_guard lg(outgoing_mutex);
    return !outgoing.empty() || engine->busy();
  }

}  // namespace clipboard
