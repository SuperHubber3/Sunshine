/**
 * @file src/clipboard/clipboard_sync.h
 * @brief Declarations for the clipboard sync protocol engine.
 *
 * This file is shared verbatim between Sunshine and Moonlight. Both ends run the same engine:
 * each side announces local clipboard changes, and the other side pulls the announced contents in
 * bounded ranges, so large clipboards never delay other control stream traffic. Received contents
 * (including files) are materialized locally before the local clipboard is replaced.
 *
 * Wire format (all integers little endian), every message starts with {u8 type, u8 0, u16 0}:
 * - HELLO    {u32 version, u32 format_mask, u64 max_transfer_bytes}
 * - ANNOUNCE {u32 announce_id, u32 count, count x {u8 format, u8 0, u16 0, u32 item_count, u64 size}}
 * - REQUEST  {u32 request_id, u32 announce_id, u8 format, u8 0, u16 0, u32 index, u64 offset, u64 length}
 * - DATA     {u32 request_id, u8 status, u8 last, u16 0, u64 total_size, u64 offset, payload}
 * - CANCEL   {u32 request_id}
 *
 * A file list is {u32 count, count x {u8 is_directory, u8 0, u16 path_length, u64 size, path}},
 * where paths are UTF-8, relative, and use '/' separators. File contents are requested with the
 * file_data format and the index of the file in the list.
 */
#pragma once

// standard includes
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace clipboard_sync {

  /**
   * @brief Clipboard formats. The values are part of the wire protocol.
   */
  enum class format_e : uint8_t {
    text = 1,  ///< UTF-8 text with LF line endings.
    png = 2,  ///< PNG image.
    file_list = 3,  ///< List of files and directories.
    file_data = 4,  ///< Contents of one file of the file list (requests only).
  };

  /**
   * @brief Contents of a clipboard.
   */
  struct content_t {
    std::optional<std::string> text;  ///< UTF-8 text with LF line endings.
    std::optional<std::vector<uint8_t>> png;  ///< PNG encoded image.
    std::vector<std::filesystem::path> files;  ///< Absolute paths of copied files and directories.

    /**
     * @brief Check whether the clipboard has no supported contents.
     * @return True if empty.
     */
    bool empty() const {
      return !text && !png && files.empty();
    }
  };

  /**
   * @brief Limits for contents received from the remote side.
   */
  struct limits_t {
    uint64_t max_text = 16ull << 20;  ///< Largest text accepted.
    uint64_t max_png = 64ull << 20;  ///< Largest image accepted.
    uint64_t max_files = 2ull << 30;  ///< Largest total size of files accepted.
    uint32_t max_file_count = 10000;  ///< Largest number of files and directories accepted.
  };

  /**
   * @brief Log message severity.
   */
  enum class log_level_e {
    debug,
    info,
    warning,
    error,
  };

  /**
   * @brief Callbacks from the engine to its owner.
   */
  struct callbacks_t {
    /// Send one protocol message to the remote side. Called from the engine thread.
    std::function<bool(const uint8_t *data, size_t size)> send;

    /// Replace the local clipboard with received contents. Called from the engine thread.
    std::function<void(const content_t &content)> set_local;

    /// Log a message. Called from any thread.
    std::function<void(log_level_e level, const std::string &message)> log;

    /// Run engine work, which may access files, with the access rights of the desktop user. Owners that
    /// run with more privileges than the user impersonate them here. Work that isn't run is dropped.
    /// Called from the engine thread. If unset, work runs directly.
    std::function<void(const std::function<void()> &work)> run_as_user;
  };

  /**
   * @brief Clipboard sync protocol engine. All public methods are thread safe.
   */
  class engine_t {
  public:
    /**
     * @brief Create the engine and its worker thread.
     *
     * @param callbacks Callbacks to the owner.
     * @param cache_dir Directory for received files. It's emptied on start.
     * @param limits Limits for received contents.
     * @param max_message_size Largest message the transport can carry.
     */
    engine_t(callbacks_t callbacks, std::filesystem::path cache_dir, limits_t limits, size_t max_message_size);
    ~engine_t();

    engine_t(const engine_t &) = delete;
    engine_t &operator=(const engine_t &) = delete;

    /**
     * @brief Send HELLO. The client does this when the stream starts, the host replies to the client's HELLO.
     */
    void send_hello();

    /**
     * @brief Handle a message from the remote side. The data is copied.
     *
     * @param data Message.
     * @param size Size of the message.
     */
    void on_message(const uint8_t *data, size_t size);

    /**
     * @brief Handle a change of the local clipboard. Changes are announced once HELLOs have been exchanged.
     *
     * @param content New contents of the local clipboard.
     */
    void on_local_changed(content_t content);

    /**
     * @brief Check whether the remote side said HELLO.
     * @return True once clipboard sync is active.
     */
    bool active() const;

    /**
     * @brief Check whether a transfer is in progress, for owners that poll their transport.
     * @return True while contents are being sent or received.
     */
    bool busy() const;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };

  /**
   * @brief Convert a UTF-8 string to a path.
   *
   * @param utf8 UTF-8 string.
   * @return Path.
   */
  std::filesystem::path path_from_utf8(const std::string &utf8);

  /**
   * @brief Convert a path to a UTF-8 string.
   *
   * @param path Path.
   * @return UTF-8 string.
   */
  std::string path_to_utf8(const std::filesystem::path &path);

}  // namespace clipboard_sync
