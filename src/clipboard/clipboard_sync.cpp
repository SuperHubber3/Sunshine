/**
 * @file src/clipboard/clipboard_sync.cpp
 * @brief Definitions for the clipboard sync protocol engine.
 *
 * This file is shared verbatim between Sunshine and Moonlight, so it only depends on the C++17
 * standard library.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string_view>
#include <thread>
#include <variant>

// local includes
#include "clipboard_sync.h"

namespace fs = std::filesystem;

namespace clipboard_sync {

  namespace {
    constexpr uint32_t PROTOCOL_VERSION = 1;

    // Contents are pulled in ranges of this size, with a bounded number of requests in flight.
    // This keeps the amount of queued clipboard data small, so it can't delay input.
    constexpr uint64_t REQUEST_SIZE = 256 * 1024;
    constexpr size_t MAX_OUTSTANDING_REQUESTS = 2;

    enum class msg_e : uint8_t {
      hello = 1,
      announce = 2,
      request = 3,
      data = 4,
      cancel = 5,
    };

    enum class status_e : uint8_t {
      ok = 0,
      stale = 1,  ///< The announcement was replaced by a newer one.
      unavailable = 2,  ///< The contents couldn't be read.
    };

    constexpr size_t HEADER_SIZE = 4;
    constexpr size_t DATA_HEADER_SIZE = HEADER_SIZE + 4 + 4 + 8 + 8;

    uint32_t format_bit(format_e format) {
      return 1u << (uint32_t) format;
    }

    /**
     * @brief Serializes a protocol message.
     */
    class writer_t {
    public:
      explicit writer_t(msg_e type) {
        u8((uint8_t) type);
        u8(0);
        u16(0);
      }

      void u8(uint8_t value) {
        buf.push_back(value);
      }

      void u16(uint16_t value) {
        number(value, 2);
      }

      void u32(uint32_t value) {
        number(value, 4);
      }

      void u64(uint64_t value) {
        number(value, 8);
      }

      void bytes(const void *data, size_t size) {
        auto p = (const uint8_t *) data;
        buf.insert(buf.end(), p, p + size);
      }

      std::vector<uint8_t> buf;

    private:
      void number(uint64_t value, int size) {
        for (int i = 0; i < size; i++) {
          buf.push_back((uint8_t) (value >> (8 * i)));
        }
      }
    };

    /**
     * @brief Parses a protocol message. Reads past the end set ok to false and return zeroes.
     */
    class reader_t {
    public:
      reader_t(const uint8_t *data, size_t size):
          p(data),
          left(size) {
      }

      uint8_t u8() {
        return (uint8_t) number(1);
      }

      uint16_t u16() {
        return (uint16_t) number(2);
      }

      uint32_t u32() {
        return (uint32_t) number(4);
      }

      uint64_t u64() {
        return number(8);
      }

      const uint8_t *bytes(size_t size) {
        if (left < size) {
          ok = false;
          left = 0;
          return nullptr;
        }
        auto result = p;
        p += size;
        left -= size;
        return result;
      }

      size_t remaining() const {
        return left;
      }

      bool ok = true;

    private:
      uint64_t number(int size) {
        if (left < (size_t) size) {
          ok = false;
          left = 0;
          return 0;
        }
        uint64_t value = 0;
        for (int i = 0; i < size; i++) {
          value |= (uint64_t) p[i] << (8 * i);
        }
        p += size;
        left -= size;
        return value;
      }

      const uint8_t *p;
      size_t left;
    };

    /**
     * @brief One entry of a file list.
     */
    struct file_entry_t {
      std::string path;  ///< Relative UTF-8 path with '/' separators.
      bool is_directory = false;
      uint64_t size = 0;
      fs::path source;  ///< Local path, only set on the sending side.
    };

    std::vector<uint8_t> encode_file_list(const std::vector<file_entry_t> &entries) {
      std::vector<uint8_t> out;
      auto number = [&](uint64_t value, int size) {
        for (int i = 0; i < size; i++) {
          out.push_back((uint8_t) (value >> (8 * i)));
        }
      };

      number(entries.size(), 4);
      for (auto &entry : entries) {
        number(entry.is_directory ? 1 : 0, 1);
        number(0, 1);
        number(entry.path.size(), 2);
        number(entry.size, 8);
        out.insert(out.end(), entry.path.begin(), entry.path.end());
      }
      return out;
    }

    /**
     * @brief Check that a received path can't escape the directory it's extracted to.
     */
    bool is_safe_relative_path(const std::string &path) {
      if (path.empty() || path.size() > 4096 || path.front() == '/' || path.back() == '/') {
        return false;
      }

      size_t start = 0;
      while (start <= path.size()) {
        auto end = path.find('/', start);
        if (end == std::string::npos) {
          end = path.size();
        }

        std::string_view component(path.data() + start, end - start);
        if (component.empty() || component == "." || component == ".." || component.size() > 255) {
          return false;
        }

        for (unsigned char c : component) {
          // Control characters, and characters that are special on Windows (drive letters, streams, separators)
          if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            return false;
          }
        }

        // Windows ignores trailing dots and spaces, which could alias other names
        if (component.back() == '.' || component.back() == ' ') {
          return false;
        }

        start = end + 1;
      }

      return true;
    }

    bool decode_file_list(const std::vector<uint8_t> &data, uint32_t max_count, std::vector<file_entry_t> &entries) {
      reader_t r(data.data(), data.size());
      auto count = r.u32();
      if (!r.ok || count > max_count) {
        return false;
      }

      entries.clear();
      entries.reserve(count);
      for (uint32_t i = 0; i < count; i++) {
        file_entry_t entry;
        entry.is_directory = r.u8() != 0;
        r.u8();
        auto length = r.u16();
        entry.size = r.u64();
        auto path = r.bytes(length);
        if (!r.ok) {
          return false;
        }
        entry.path.assign((const char *) path, length);
        if (!is_safe_relative_path(entry.path) || (entry.is_directory && entry.size != 0)) {
          return false;
        }
        entries.push_back(std::move(entry));
      }

      return r.remaining() == 0;
    }

    /**
     * @brief Identifies clipboard contents, to recognize our own changes coming back.
     */
    struct fingerprint_t {
      std::optional<std::string> text;
      std::optional<std::pair<size_t, size_t>> png;  ///< Size and hash.
      std::vector<fs::path> files;

      bool operator==(const fingerprint_t &other) const {
        return text == other.text && png == other.png && files == other.files;
      }
    };

    fingerprint_t fingerprint(const content_t &content) {
      fingerprint_t result;
      result.text = content.text;
      if (content.png) {
        std::string_view bytes((const char *) content.png->data(), content.png->size());
        result.png = std::make_pair(content.png->size(), std::hash<std::string_view> {}(bytes));
      }
      result.files = content.files;
      return result;
    }
  }  // namespace

  fs::path path_from_utf8(const std::string &utf8) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return fs::u8path(utf8);
#endif
  }

  std::string path_to_utf8(const fs::path &path) {
#if defined(__cpp_char8_t)
    auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#else
    return path.u8string();
#endif
  }

  struct engine_t::impl_t {
    impl_t(callbacks_t callbacks, fs::path cache_dir, limits_t limits, size_t max_message_size):
        callbacks(std::move(callbacks)),
        cache_dir(std::move(cache_dir)),
        limits(limits),
        max_message_size(max_message_size) {
      thread = std::thread(&impl_t::run, this);
    }

    ~impl_t() {
      {
        std::lock_guard lg(mutex);
        stopping = true;
      }
      cv.notify_one();
      thread.join();
    }

    struct hello_event_t {};

    struct message_event_t {
      std::vector<uint8_t> data;
    };

    struct local_event_t {
      content_t content;
    };

    using event_t = std::variant<hello_event_t, message_event_t, local_event_t>;

    void queue(event_t event) {
      {
        std::lock_guard lg(mutex);
        // Only the latest local change matters, and a burst of them shouldn't pile up
        if (std::holds_alternative<local_event_t>(event)) {
          events.erase(std::remove_if(events.begin(), events.end(), [](const event_t &e) {
                         return std::holds_alternative<local_event_t>(e);
                       }),
                       events.end());
        }
        events.push_back(std::move(event));
      }
      cv.notify_one();
    }

    void log(log_level_e level, const std::string &message) {
      if (callbacks.log) {
        callbacks.log(level, "Clipboard: " + message);
      }
    }

    void send(const writer_t &w) {
      if (!callbacks.send(w.buf.data(), w.buf.size())) {
        log(log_level_e::warning, "failed to send message");
      }
    }

    void run() {
      // Remove files received by earlier sessions
      std::error_code ec;
      if (fs::is_directory(cache_dir, ec)) {
        for (auto &entry : fs::directory_iterator(cache_dir, ec)) {
          if (path_to_utf8(entry.path().filename()).rfind("clip-", 0) == 0) {
            fs::remove_all(entry.path(), ec);
          }
        }
      }

      while (true) {
        event_t event;
        {
          std::unique_lock lk(mutex);
          cv.wait(lk, [this]() {
            return stopping || !events.empty();
          });
          if (stopping) {
            break;
          }
          event = std::move(events.front());
          events.pop_front();
        }

        if (std::holds_alternative<hello_event_t>(event)) {
          send_hello();
        } else if (auto message = std::get_if<message_event_t>(&event)) {
          handle_message(message->data);
        } else if (auto local = std::get_if<local_event_t>(&event)) {
          handle_local_change(std::move(local->content));
        }

        busy_flag = fetch.has_value();
      }

      cancel_fetch();
    }

    void send_hello() {
      writer_t w(msg_e::hello);
      w.u32(PROTOCOL_VERSION);
      uint32_t formats = 0;
      if (limits.max_text) {
        formats |= format_bit(format_e::text);
      }
      if (limits.max_png) {
        formats |= format_bit(format_e::png);
      }
      if (limits.max_files) {
        formats |= format_bit(format_e::file_list);
      }
      w.u32(formats);
      w.u64(std::max({limits.max_text, limits.max_png, limits.max_files}));
      send(w);
      hello_sent = true;
    }

    void handle_message(const std::vector<uint8_t> &data) {
      reader_t r(data.data(), data.size());
      auto type = (msg_e) r.u8();
      r.u8();
      r.u16();
      if (!r.ok) {
        return;
      }

      switch (type) {
        case msg_e::hello:
          handle_hello(r);
          break;
        case msg_e::announce:
          handle_announce(r);
          break;
        case msg_e::request:
          handle_request(r);
          break;
        case msg_e::data:
          handle_data(r);
          break;
        case msg_e::cancel:
          // Requests are served as soon as they arrive, so there's nothing left to cancel
          break;
        default:
          log(log_level_e::debug, "ignoring unknown message type " + std::to_string((int) type));
          break;
      }
    }

    void handle_hello(reader_t &r) {
      auto version = r.u32();
      remote_formats = r.u32();
      remote_max_transfer = r.u64();
      if (!r.ok || version < 1) {
        log(log_level_e::warning, "malformed HELLO");
        return;
      }

      if (!hello_sent) {
        send_hello();
      }

      if (!active_flag) {
        active_flag = true;
        log(log_level_e::info, "sync is active");
      }

      if (pending_local) {
        auto content = std::move(*pending_local);
        pending_local.reset();
        announce(std::move(content));
      }
    }

    void handle_local_change(content_t content) {
      if (content.empty()) {
        return;
      }

      // Contents we set from the remote side come back as a local change
      if (last_remote && fingerprint(content) == *last_remote) {
        return;
      }
      last_remote.reset();

      if (!active_flag) {
        pending_local = std::move(content);
        return;
      }

      announce(std::move(content));
    }

    /**
     * @brief Add a file or directory tree to a file list.
     */
    bool add_files(const fs::path &source, uint64_t &total_size, std::vector<file_entry_t> &files) {
      std::error_code ec;
      auto name = path_to_utf8(source.filename());
      if (name.empty()) {
        return false;
      }

      auto status = fs::symlink_status(source, ec);
      if (ec) {
        return false;
      }

      if (fs::is_directory(status)) {
        files.push_back({name, true, 0, source});

        // Symlinked directories aren't followed, which also rules out loops
        fs::recursive_directory_iterator it(source, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
          if (files.size() > limits.max_file_count) {
            return false;
          }

          auto relative = path_to_utf8(it->path().lexically_relative(source));
#ifdef _WIN32
          std::replace(relative.begin(), relative.end(), '\\', '/');
#endif
          auto path = name + "/" + relative;

          auto entry_status = it->symlink_status(ec);
          if (ec) {
            return false;
          }
          if (fs::is_directory(entry_status)) {
            files.push_back({path, true, 0, it->path()});
          } else if (fs::is_regular_file(entry_status)) {
            auto size = it->file_size(ec);
            if (ec) {
              return false;
            }
            files.push_back({path, false, size, it->path()});
            total_size += size;
          }
          // Other files (symlinks, sockets, devices) are skipped
        }
        return !ec;
      }

      if (fs::is_regular_file(status)) {
        auto size = fs::file_size(source, ec);
        if (ec) {
          return false;
        }
        files.push_back({name, false, size, source});
        total_size += size;
        return true;
      }

      return false;
    }

    void announce(content_t content) {
      local_t snapshot;
      snapshot.id = next_announce_id++;

      writer_t w(msg_e::announce);
      w.u32(snapshot.id);

      struct entry_t {
        format_e format;
        uint32_t count;
        uint64_t size;
      };
      std::vector<entry_t> entries;

      if (content.text && (remote_formats & format_bit(format_e::text)) && content.text->size() <= remote_max_transfer) {
        entries.push_back({format_e::text, 0, content.text->size()});
      }
      if (content.png && (remote_formats & format_bit(format_e::png)) && content.png->size() <= remote_max_transfer) {
        entries.push_back({format_e::png, 0, content.png->size()});
      }
      if (!content.files.empty() && (remote_formats & format_bit(format_e::file_list))) {
        uint64_t total_size = 0;
        bool ok = true;
        for (auto &file : content.files) {
          ok = ok && add_files(file, total_size, snapshot.files) && snapshot.files.size() <= limits.max_file_count;
        }

        if (ok && total_size <= remote_max_transfer) {
          snapshot.file_list = encode_file_list(snapshot.files);
          entries.push_back({format_e::file_list, (uint32_t) snapshot.files.size(), total_size});
        } else {
          log(log_level_e::info, "not syncing copied files, there are too many or they're too large");
          snapshot.files.clear();
        }
      }

      if (entries.empty()) {
        return;
      }

      w.u32((uint32_t) entries.size());
      for (auto &entry : entries) {
        w.u8((uint8_t) entry.format);
        w.u8(0);
        w.u16(0);
        w.u32(entry.count);
        w.u64(entry.size);
      }

      snapshot.content = std::move(content);
      local = std::move(snapshot);
      send(w);

      log(log_level_e::debug, "announced local clipboard " + std::to_string(local->id));
    }

    void send_data_header(writer_t &w, uint32_t request_id, status_e status, bool last, uint64_t total_size, uint64_t offset) {
      w.u32(request_id);
      w.u8((uint8_t) status);
      w.u8(last ? 1 : 0);
      w.u16(0);
      w.u64(total_size);
      w.u64(offset);
    }

    void send_status(uint32_t request_id, status_e status) {
      writer_t w(msg_e::data);
      send_data_header(w, request_id, status, true, 0, 0);
      send(w);
    }

    void handle_request(reader_t &r) {
      auto request_id = r.u32();
      auto announce_id = r.u32();
      auto format = (format_e) r.u8();
      r.u8();
      r.u16();
      auto index = r.u32();
      auto offset = r.u64();
      auto length = r.u64();
      if (!r.ok) {
        return;
      }

      if (!local || local->id != announce_id) {
        send_status(request_id, status_e::stale);
        return;
      }

      const uint8_t *memory = nullptr;
      uint64_t total_size = 0;
      std::ifstream file;

      switch (format) {
        case format_e::text:
          if (local->content.text) {
            memory = (const uint8_t *) local->content.text->data();
            total_size = local->content.text->size();
          }
          break;
        case format_e::png:
          if (local->content.png) {
            memory = local->content.png->data();
            total_size = local->content.png->size();
          }
          break;
        case format_e::file_list:
          memory = local->file_list.data();
          total_size = local->file_list.size();
          break;
        case format_e::file_data:
          if (index < local->files.size() && !local->files[index].is_directory) {
            file.open(local->files[index].source, std::ios::binary);
            total_size = local->files[index].size;
          }
          break;
      }

      if ((!memory && !file.is_open()) || offset > total_size) {
        send_status(request_id, status_e::unavailable);
        return;
      }

      length = std::min(length, total_size - offset);
      if (file.is_open()) {
        file.seekg((std::streamoff) offset);
      }

      auto chunk_size = max_message_size - DATA_HEADER_SIZE;
      std::vector<uint8_t> chunk;
      uint64_t sent = 0;
      do {
        auto n = (size_t) std::min<uint64_t>(chunk_size, length - sent);
        const uint8_t *payload = nullptr;
        if (memory) {
          payload = memory + offset + sent;
        } else {
          chunk.resize(n);
          file.read((char *) chunk.data(), (std::streamsize) n);
          if ((size_t) file.gcount() != n) {
            // The file shrank or can't be read
            send_status(request_id, status_e::unavailable);
            return;
          }
          payload = chunk.data();
        }

        writer_t w(msg_e::data);
        send_data_header(w, request_id, status_e::ok, sent + n == length, total_size, offset + sent);
        w.bytes(payload, n);
        send(w);
        sent += n;
      } while (sent < length);
    }

    void handle_announce(reader_t &r) {
      auto announce_id = r.u32();
      auto count = r.u32();
      if (!r.ok || count > 16) {
        return;
      }

      // The remote clipboard changed, so anything we were fetching is obsolete
      cancel_fetch();

      fetch_t f;
      f.announce_id = announce_id;
      f.dir = cache_dir / ("clip-" + std::to_string(++fetch_generation));

      for (uint32_t i = 0; i < count; i++) {
        auto format = (format_e) r.u8();
        r.u8();
        r.u16();
        auto item_count = r.u32();
        auto size = r.u64();
        if (!r.ok) {
          return;
        }

        switch (format) {
          case format_e::text:
            if (size > 0 && size <= limits.max_text) {
              f.items.push_back(std::make_unique<item_t>(format, 0, size));
            }
            break;
          case format_e::png:
            if (size > 0 && size <= limits.max_png) {
              f.items.push_back(std::make_unique<item_t>(format, 0, size));
            }
            break;
          case format_e::file_list:
            if (limits.max_files && size <= limits.max_files && item_count <= limits.max_file_count) {
              // The size of the list itself is only known once its first range arrives
              f.items.push_back(std::make_unique<item_t>(format, 0, UINT64_MAX));
            } else {
              log(log_level_e::info, "not receiving remote files, there are too many or they're too large");
            }
            break;
          default:
            break;
        }
      }

      if (f.items.empty()) {
        return;
      }

      log(log_level_e::debug, "fetching remote clipboard " + std::to_string(announce_id));
      fetch = std::move(f);
      pump();
    }

    struct item_t {
      item_t(format_e format, uint32_t index, uint64_t size):
          format(format),
          index(index),
          size(size) {
      }

      format_e format;
      uint32_t index;
      uint64_t size;  ///< UINT64_MAX while unknown.
      uint64_t requested = 0;
      uint64_t received = 0;
      std::vector<uint8_t> memory;
      std::ofstream file;
      fs::path path;
    };

    struct request_t {
      size_t item;
      uint64_t offset;
      uint64_t length;
      uint64_t received = 0;
    };

    struct fetch_t {
      uint32_t announce_id = 0;
      fs::path dir;
      std::vector<std::unique_ptr<item_t>> items;
      std::map<uint32_t, request_t> requests;
      std::vector<file_entry_t> files;
      uint64_t file_bytes = 0;
    };

    void pump() {
      auto &f = *fetch;
      while (f.requests.size() < MAX_OUTSTANDING_REQUESTS) {
        auto it = std::find_if(f.items.begin(), f.items.end(), [](const std::unique_ptr<item_t> &item) {
          // Items of unknown size get one request until the size is known
          return item->size == UINT64_MAX ? item->requested == 0 : item->requested < item->size;
        });
        if (it == f.items.end()) {
          break;
        }

        auto &item = **it;
        auto length = item.size == UINT64_MAX ? REQUEST_SIZE : std::min(REQUEST_SIZE, item.size - item.requested);
        auto request_id = next_request_id++;

        writer_t w(msg_e::request);
        w.u32(request_id);
        w.u32(f.announce_id);
        w.u8((uint8_t) item.format);
        w.u8(0);
        w.u16(0);
        w.u32(item.index);
        w.u64(item.requested);
        w.u64(length);
        send(w);

        f.requests[request_id] = {(size_t) (it - f.items.begin()), item.requested, length};
        item.requested += length;
      }

      bool done = f.requests.empty() && std::all_of(f.items.begin(), f.items.end(), [](const std::unique_ptr<item_t> &item) {
                    return item->received == item->size;
                  });
      if (done) {
        finish_fetch();
      }
    }

    void cancel_fetch() {
      if (!fetch) {
        return;
      }

      for (auto &request : fetch->requests) {
        writer_t w(msg_e::cancel);
        w.u32(request.first);
        send(w);
      }

      auto dir = fetch->dir;
      fetch.reset();

      std::error_code ec;
      fs::remove_all(dir, ec);
    }

    void abort_fetch(const std::string &reason) {
      log(log_level_e::warning, "couldn't receive remote clipboard: " + reason);
      cancel_fetch();
    }

    void handle_data(reader_t &r) {
      auto request_id = r.u32();
      auto status = (status_e) r.u8();
      auto last = r.u8() != 0;
      r.u16();
      auto total_size = r.u64();
      auto offset = r.u64();
      if (!r.ok || !fetch) {
        return;
      }

      auto &f = *fetch;
      auto request_it = f.requests.find(request_id);
      if (request_it == f.requests.end()) {
        return;
      }
      auto &request = request_it->second;
      auto &item = *f.items[request.item];

      if (status != status_e::ok) {
        // A stale response means a newer announcement is on its way
        if (status != status_e::stale) {
          abort_fetch("the remote contents are unavailable");
        } else {
          cancel_fetch();
        }
        return;
      }

      // Only file lists have an unknown size until their first range arrives
      if (item.size == UINT64_MAX) {
        if (total_size > (64ull << 20)) {
          abort_fetch("the file list is too large");
          return;
        }
        item.size = total_size;
      }

      auto payload_size = r.remaining();
      auto payload = r.bytes(payload_size);
      if (total_size != item.size || offset != request.offset + request.received || request.received + payload_size > request.length ||
          offset + payload_size > item.size) {
        abort_fetch("the remote contents changed during the transfer");
        return;
      }

      if (!write_item(item, payload, payload_size)) {
        return;
      }
      request.received += payload_size;
      item.received += payload_size;

      bool request_done = last || request.received == request.length || item.received == item.size;
      if (request_done) {
        f.requests.erase(request_it);
        if (item.requested > item.size) {
          // The first request of an item of unknown size asked for more than it has
          item.requested = item.size;
        }
      }

      if (item.received == item.size && !complete_item(item)) {
        return;
      }

      if (request_done) {
        pump();
      }
    }

    bool write_item(item_t &item, const uint8_t *payload, size_t size) {
      if (item.format != format_e::file_data) {
        item.memory.insert(item.memory.end(), payload, payload + size);
        return true;
      }

      if (!item.file.is_open()) {
        item.file.open(item.path, std::ios::binary | std::ios::trunc);
        if (!item.file.is_open()) {
          abort_fetch("can't create " + path_to_utf8(item.path));
          return false;
        }
      }

      item.file.write((const char *) payload, (std::streamsize) size);
      if (!item.file) {
        abort_fetch("can't write " + path_to_utf8(item.path));
        return false;
      }
      return true;
    }

    bool complete_item(item_t &item) {
      auto &f = *fetch;

      if (item.format == format_e::file_data) {
        item.file.close();
        return true;
      }

      if (item.format != format_e::file_list) {
        return true;
      }

      if (!decode_file_list(item.memory, limits.max_file_count, f.files)) {
        abort_fetch("the file list is invalid");
        return false;
      }

      std::error_code ec;
      fs::create_directories(f.dir, ec);
      if (ec) {
        abort_fetch("can't create " + path_to_utf8(f.dir));
        return false;
      }

      for (uint32_t i = 0; i < f.files.size(); i++) {
        auto &entry = f.files[i];
        auto path = f.dir / path_from_utf8(entry.path);

        if (entry.is_directory) {
          fs::create_directories(path, ec);
        } else {
          fs::create_directories(path.parent_path(), ec);
          f.file_bytes += entry.size;
          if (f.file_bytes > limits.max_files) {
            abort_fetch("the files are too large");
            return false;
          }

          if (entry.size == 0) {
            std::ofstream empty(path, std::ios::binary | std::ios::trunc);
          } else {
            auto data = std::make_unique<item_t>(format_e::file_data, i, entry.size);
            data->path = path;
            f.items.push_back(std::move(data));
          }
        }

        if (ec) {
          abort_fetch("can't create " + path_to_utf8(path));
          return false;
        }
      }

      return true;
    }

    void finish_fetch() {
      auto &f = *fetch;
      content_t content;

      for (auto &item : f.items) {
        if (item->format == format_e::text) {
          content.text = std::string(item->memory.begin(), item->memory.end());
        } else if (item->format == format_e::png) {
          content.png = std::move(item->memory);
        }
      }

      for (auto &entry : f.files) {
        // Only top level entries go on the clipboard, like they were copied
        if (entry.path.find('/') == std::string::npos) {
          content.files.push_back(f.dir / path_from_utf8(entry.path));
        }
      }

      // Keep the previous set of received files around in case it's still being pasted
      if (!f.files.empty()) {
        received_dirs.push_back(f.dir);
        while (received_dirs.size() > 2) {
          std::error_code ec;
          fs::remove_all(received_dirs.front(), ec);
          received_dirs.pop_front();
        }
      }

      log(log_level_e::debug, "received remote clipboard " + std::to_string(f.announce_id));
      fetch.reset();

      last_remote = fingerprint(content);
      callbacks.set_local(content);
    }

    struct local_t {
      uint32_t id = 0;
      content_t content;
      std::vector<file_entry_t> files;
      std::vector<uint8_t> file_list;
    };

    callbacks_t callbacks;
    fs::path cache_dir;
    limits_t limits;
    size_t max_message_size;

    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
    std::deque<event_t> events;
    std::atomic<bool> active_flag {false};
    std::atomic<bool> busy_flag {false};

    // Everything below is only used by the engine thread
    bool hello_sent = false;
    uint32_t remote_formats = 0;
    uint64_t remote_max_transfer = 0;
    std::optional<content_t> pending_local;
    std::optional<local_t> local;
    uint32_t next_announce_id = 1;
    std::optional<fetch_t> fetch;
    uint32_t next_request_id = 1;
    uint32_t fetch_generation = 0;
    std::deque<fs::path> received_dirs;
    std::optional<fingerprint_t> last_remote;
  };

  engine_t::engine_t(callbacks_t callbacks, fs::path cache_dir, limits_t limits, size_t max_message_size):
      impl(std::make_unique<impl_t>(std::move(callbacks), std::move(cache_dir), limits, max_message_size)) {
  }

  engine_t::~engine_t() = default;

  void engine_t::send_hello() {
    impl->queue(impl_t::hello_event_t {});
  }

  void engine_t::on_message(const uint8_t *data, size_t size) {
    impl->queue(impl_t::message_event_t {std::vector<uint8_t>(data, data + size)});
  }

  void engine_t::on_local_changed(content_t content) {
    impl->queue(impl_t::local_event_t {std::move(content)});
  }

  bool engine_t::active() const {
    return impl->active_flag;
  }

  bool engine_t::busy() const {
    return impl->busy_flag;
  }

}  // namespace clipboard_sync
