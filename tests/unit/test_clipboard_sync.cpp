/**
 * @file tests/unit/test_clipboard_sync.cpp
 * @brief Test src/clipboard/clipboard_sync.*.
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <random>

// local includes
#include <src/clipboard/clipboard_sync.h>

using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

  /**
   * @brief One side of a clipboard sync connection, wired to its peer in memory.
   */
  struct side_t {
    std::unique_ptr<clipboard_sync::engine_t> engine;
    side_t *peer = nullptr;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<clipboard_sync::content_t> set_calls;
    std::vector<std::vector<uint8_t>> sent;

    void create(const fs::path &cache, clipboard_sync::limits_t limits = {}) {
      clipboard_sync::callbacks_t callbacks;
      callbacks.send = [this](const uint8_t *data, size_t size) {
        EXPECT_LE(size, 32768u);
        {
          std::lock_guard lg(mutex);
          sent.emplace_back(data, data + size);
        }
        if (peer) {
          peer->engine->on_message(data, size);
        }
        return true;
      };
      callbacks.set_local = [this](const clipboard_sync::content_t &content) {
        std::lock_guard lg(mutex);
        set_calls.push_back(content);
        cv.notify_all();
      };
      engine = std::make_unique<clipboard_sync::engine_t>(callbacks, cache, limits, 32768);
    }

    bool wait_for_sets(size_t count) {
      std::unique_lock lk(mutex);
      return cv.wait_for(lk, 10s, [&] {
        return set_calls.size() >= count;
      });
    }
  };

  void write_random_file(const fs::path &path, size_t size, uint32_t seed) {
    fs::create_directories(path.parent_path());
    std::mt19937 rng(seed);
    std::string data(size, '\0');
    for (auto &c : data) {
      c = (char) rng();
    }
    std::ofstream(path, std::ios::binary) << data;
  }

  std::string read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
  }

}  // namespace

class ClipboardSyncTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    root = fs::temp_directory_path() / "sunshine-clipboard-sync-test";
    fs::remove_all(root);
    fs::create_directories(root / "source");

    client.peer = &host;
    host.peer = &client;
    client.create(root / "client-cache");
    host.create(root / "host-cache");
  }

  void TearDown() override {
    client.engine.reset();
    host.engine.reset();
    fs::remove_all(root);
    BaseTest::TearDown();
  }

  fs::path root;
  side_t client;
  side_t host;
};

TEST_F(ClipboardSyncTest, ChangesBeforeHelloAreSentOnceActive) {
  clipboard_sync::content_t content;
  content.text = "copied before connecting";
  client.engine->on_local_changed(content);
  client.engine->send_hello();

  ASSERT_TRUE(host.wait_for_sets(1));
  EXPECT_EQ(host.set_calls[0].text, content.text);
  EXPECT_TRUE(client.engine->active());
  EXPECT_TRUE(host.engine->active());
}

TEST_F(ClipboardSyncTest, TextIsSyncedAndNotEchoed) {
  client.engine->send_hello();

  clipboard_sync::content_t content;
  content.text = "hello from the host\nline 2 \xc3\xa9";
  host.engine->on_local_changed(content);
  ASSERT_TRUE(client.wait_for_sets(1));
  EXPECT_EQ(client.set_calls[0].text, content.text);

  // Setting the clipboard is reported back as a local change, which must not bounce back
  size_t sent_before;
  {
    std::lock_guard lg(client.mutex);
    sent_before = client.sent.size();
  }
  client.engine->on_local_changed(client.set_calls[0]);
  std::this_thread::sleep_for(200ms);
  std::lock_guard lg(client.mutex);
  EXPECT_EQ(client.sent.size(), sent_before);
}

TEST_F(ClipboardSyncTest, LargeImageIsTransferredInRanges) {
  client.engine->send_hello();

  clipboard_sync::content_t content;
  content.png = std::vector<uint8_t>(3 * 1024 * 1024 + 123);
  for (size_t i = 0; i < content.png->size(); i++) {
    (*content.png)[i] = (uint8_t) (i * 7);
  }
  client.engine->on_local_changed(content);

  ASSERT_TRUE(host.wait_for_sets(1));
  EXPECT_EQ(host.set_calls[0].png, content.png);
}

TEST_F(ClipboardSyncTest, FilesAndDirectoriesAreMaterialized) {
  client.engine->send_hello();

  write_random_file(root / "source/single.bin", 1000000, 1);
  write_random_file(root / "source/dir/a.txt", 10, 2);
  write_random_file(root / "source/dir/sub/b.bin", 700000, 3);
  write_random_file(root / "source/dir/sub/empty", 0, 4);
  fs::create_directories(root / "source/dir/emptydir");

  clipboard_sync::content_t content;
  content.files = {root / "source/single.bin", root / "source/dir"};
  host.engine->on_local_changed(content);

  ASSERT_TRUE(client.wait_for_sets(1));
  auto &files = client.set_calls[0].files;
  ASSERT_EQ(files.size(), 2u);
  EXPECT_EQ(read_file(files[0]), read_file(root / "source/single.bin"));
  EXPECT_EQ(read_file(files[1] / "a.txt"), read_file(root / "source/dir/a.txt"));
  EXPECT_EQ(read_file(files[1] / "sub/b.bin"), read_file(root / "source/dir/sub/b.bin"));
  EXPECT_TRUE(fs::exists(files[1] / "sub/empty"));
  EXPECT_TRUE(fs::is_directory(files[1] / "emptydir"));
}

TEST_F(ClipboardSyncTest, FilesAboveTheReceiverLimitAreSkipped) {
  side_t small_client;
  small_client.peer = &host;
  host.peer = &small_client;
  clipboard_sync::limits_t limits;
  limits.max_files = 1000;
  small_client.create(root / "small-client-cache", limits);
  small_client.engine->send_hello();
  std::this_thread::sleep_for(100ms);

  write_random_file(root / "source/large.bin", 100000, 5);
  clipboard_sync::content_t content;
  content.files = {root / "source/large.bin"};
  content.text = "the text still arrives";
  host.engine->on_local_changed(content);

  ASSERT_TRUE(small_client.wait_for_sets(1));
  EXPECT_TRUE(small_client.set_calls[0].files.empty());
  EXPECT_EQ(small_client.set_calls[0].text, content.text);

  small_client.engine.reset();
  host.peer = &client;
}

/**
 * @brief Build a raw protocol message for hostile peer tests.
 */
struct raw_message_t {
  explicit raw_message_t(uint8_t type) {
    bytes = {type, 0, 0, 0};
  }

  raw_message_t &number(uint64_t value, int size) {
    for (int i = 0; i < size; i++) {
      bytes.push_back((uint8_t) (value >> (8 * i)));
    }
    return *this;
  }

  raw_message_t &text(const std::string &value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
    return *this;
  }

  std::vector<uint8_t> bytes;
};

struct ClipboardSyncHostilePathTest: BaseTest, testing::WithParamInterface<std::string> {};

TEST_P(ClipboardSyncHostilePathTest, IsRejected) {
  auto root = fs::temp_directory_path() / "sunshine-clipboard-sync-hostile";
  fs::remove_all(root);

  side_t victim;
  victim.create(root / "cache");

  // HELLO, then an announcement of a single file whose name is hostile
  auto hello = raw_message_t(1).number(1, 4).number(0xffffffff, 4).number(1ull << 40, 8);
  victim.engine->on_message(hello.bytes.data(), hello.bytes.size());
  auto announce = raw_message_t(2).number(1, 4).number(1, 4).number(3, 1).number(0, 3).number(1, 4).number(4, 8);
  victim.engine->on_message(announce.bytes.data(), announce.bytes.size());

  // Answer the file list request
  std::vector<uint8_t> request;
  for (int i = 0; i < 100 && request.empty(); i++) {
    std::this_thread::sleep_for(10ms);
    std::lock_guard lg(victim.mutex);
    for (auto &message : victim.sent) {
      if (message[0] == 3) {
        request = message;
      }
    }
  }
  ASSERT_FALSE(request.empty());
  uint32_t request_id = request[4] | request[5] << 8 | request[6] << 16 | request[7] << 24;

  auto path = GetParam();
  raw_message_t list(0);
  list.bytes.clear();
  list.number(1, 4).number(0, 2).number(path.size(), 2).number(4, 8).text(path);
  auto data = raw_message_t(4).number(request_id, 4).number(0, 1).number(1, 1).number(0, 2).number(list.bytes.size(), 8).number(0, 8);
  data.bytes.insert(data.bytes.end(), list.bytes.begin(), list.bytes.end());
  victim.engine->on_message(data.bytes.data(), data.bytes.size());

  std::this_thread::sleep_for(200ms);
  std::lock_guard lg(victim.mutex);
  EXPECT_TRUE(victim.set_calls.empty());
  EXPECT_FALSE(fs::exists(root / "evil"));
}

INSTANTIATE_TEST_SUITE_P(
  ClipboardSyncTests,
  ClipboardSyncHostilePathTest,
  testing::Values(
    "../evil",
    "a/../../evil",
    "/abs",
    "C:evil",
    "a\\b",
    "a//b",
    "a/./b",
    "dot.",
    "",
    "a/",
    // Invalid UTF-8: a stray continuation byte, an overlong '/', a surrogate, and a truncated sequence
    "\xff",
    "a\xc0\xafb",
    "\xed\xa0\x80",
    "a\xe2\x82"
#ifdef _WIN32
    ,
    // Device names
    "NUL",
    "dir/com1.txt",
    "Lpt9",
    "CON .txt"
#endif
  )
);

namespace {
  /**
   * @brief Find the messages of a type that one side sent.
   */
  std::vector<std::vector<uint8_t>> sent_messages(side_t &side, uint8_t type) {
    std::lock_guard lg(side.mutex);
    std::vector<std::vector<uint8_t>> result;
    for (auto &message : side.sent) {
      if (message[0] == type) {
        result.push_back(message);
      }
    }
    return result;
  }

  uint64_t read_number(const std::vector<uint8_t> &message, size_t offset, int size) {
    uint64_t value = 0;
    for (int i = 0; i < size; i++) {
      value |= (uint64_t) message[offset + i] << (8 * i);
    }
    return value;
  }
}  // namespace

class ClipboardSyncPeerTest: public BaseTest {
protected:
  void SetUp() override {
    BaseTest::SetUp();
    root = fs::temp_directory_path() / "sunshine-clipboard-sync-peer";
    fs::remove_all(root);
    engine.create(root / "cache");

    auto hello = raw_message_t(1).number(1, 4).number(0xffffffff, 4).number(1ull << 40, 8);
    engine.engine->on_message(hello.bytes.data(), hello.bytes.size());
  }

  void TearDown() override {
    engine.engine.reset();
    fs::remove_all(root);
    BaseTest::TearDown();
  }

  fs::path root;
  side_t engine;
};

TEST_F(ClipboardSyncPeerTest, ServedRangesAreBounded) {
  clipboard_sync::content_t content;
  content.text = std::string(1 << 20, 'x');
  engine.engine->on_local_changed(content);

  // Ask for everything at once
  auto request = raw_message_t(3).number(7, 4).number(1, 4).number(1, 1).number(0, 3).number(0, 4).number(0, 8).number(UINT64_MAX, 8);
  engine.engine->on_message(request.bytes.data(), request.bytes.size());

  uint64_t served = 0;
  bool last = false;
  for (int i = 0; i < 200 && !last; i++) {
    std::this_thread::sleep_for(10ms);
    served = 0;
    for (auto &data : sent_messages(engine, 4)) {
      if (read_number(data, 4, 4) == 7) {
        ASSERT_EQ(data[8], 0) << "the request failed";
        served += data.size() - 28;
        last = last || data[9] != 0;
      }
    }
  }

  EXPECT_TRUE(last);
  EXPECT_EQ(served, 256u * 1024);
}

TEST_F(ClipboardSyncPeerTest, TruncatedRangesAbortTheTransfer) {
  // Announce 1000 bytes of text
  auto announce = raw_message_t(2).number(1, 4).number(1, 4).number(1, 1).number(0, 3).number(0, 4).number(1000, 8);
  engine.engine->on_message(announce.bytes.data(), announce.bytes.size());

  std::vector<std::vector<uint8_t>> requests;
  for (int i = 0; i < 100 && requests.empty(); i++) {
    std::this_thread::sleep_for(10ms);
    requests = sent_messages(engine, 3);
  }
  ASSERT_EQ(requests.size(), 1u);
  auto request_id = read_number(requests[0], 4, 4);

  // Claim the range ends after 10 bytes
  auto data = raw_message_t(4).number(request_id, 4).number(0, 1).number(1, 1).number(0, 2).number(1000, 8).number(0, 8).text(std::string(10, 'y'));
  engine.engine->on_message(data.bytes.data(), data.bytes.size());

  for (int i = 0; i < 100 && engine.engine->busy(); i++) {
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_FALSE(engine.engine->busy());
  std::lock_guard lg(engine.mutex);
  EXPECT_TRUE(engine.set_calls.empty());
}
