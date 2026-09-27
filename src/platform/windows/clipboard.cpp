/**
 * @file src/platform/windows/clipboard.cpp
 * @brief Definitions for the Windows clipboard used by clipboard sync.
 */
// standard includes
#include <condition_variable>
#include <cstring>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

// platform includes
// clang-format off
#include <windows.h>
#include <objidl.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <WtsApi32.h>
// clang-format on

// local includes
#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"
#include "utf_utils.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace platf {

  namespace {
    constexpr UINT WM_APP_SET_CLIPBOARD = WM_APP + 1;
    constexpr UINT_PTR READ_TIMER_ID = 1;

    // Applications often set several formats in separate steps, so reads wait for a pause
    constexpr UINT READ_DELAY_MS = 50;

    // Largest clipboard image that is synced
    constexpr size_t MAX_IMAGE_SIZE = 64 << 20;

    // The user's token is looked up again after this, in case they logged off
    constexpr auto USER_TOKEN_LIFETIME = 5s;

    /**
     * @brief Check whether a copied path is a file system path, rather than a device.
     */
    bool is_file_system_path(const std::wstring &path) {
      // Win32 namespace paths are only allowed for drive letters and UNC shares
      if (path.rfind(LR"(\\.\)", 0) == 0) {
        return false;
      }
      if (path.rfind(LR"(\\?\)", 0) == 0) {
        auto rest = path.substr(4);
        return rest.rfind(LR"(UNC\)", 0) == 0 || (rest.size() >= 3 && iswalpha(rest[0]) && rest[1] == L':' && rest[2] == L'\\');
      }
      return true;
    }

    /**
     * @brief Minimal owning COM pointer.
     */
    template<class T>
    class com_ptr {
    public:
      com_ptr() = default;
      com_ptr(const com_ptr &) = delete;
      com_ptr &operator=(const com_ptr &) = delete;

      ~com_ptr() {
        if (p) {
          p->Release();
        }
      }

      T *operator->() const {
        return p;
      }

      T *get() const {
        return p;
      }

      T **put() {
        if (p) {
          p->Release();
          p = nullptr;
        }
        return &p;
      }

      explicit operator bool() const {
        return p != nullptr;
      }

    private:
      T *p = nullptr;
    };

    /**
     * @brief Replace LF with CRLF, the Windows text convention.
     */
    std::string to_crlf(const std::string &text) {
      std::string result;
      result.reserve(text.size());
      for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
          result += '\r';
        }
        result += text[i];
      }
      return result;
    }

    /**
     * @brief Replace CRLF with LF, the clipboard sync convention.
     */
    std::string to_lf(const std::string &text) {
      std::string result;
      result.reserve(text.size());
      for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
          continue;
        }
        result += text[i];
      }
      return result;
    }

    /**
     * @brief Copy a clipboard handle's memory.
     */
    std::optional<std::vector<uint8_t>> read_global(HANDLE handle, size_t limit) {
      if (!handle) {
        return std::nullopt;
      }

      auto size = GlobalSize(handle);
      if (size == 0 || size > limit) {
        return std::nullopt;
      }

      auto data = (const uint8_t *) GlobalLock(handle);
      if (!data) {
        return std::nullopt;
      }

      std::vector<uint8_t> result(data, data + size);
      GlobalUnlock(handle);
      return result;
    }

    /**
     * @brief Put a copy of memory on the clipboard. The clipboard must be open.
     */
    bool set_global(UINT format, const void *data, size_t size) {
      auto handle = GlobalAlloc(GMEM_MOVEABLE, size);
      if (!handle) {
        return false;
      }

      auto dest = GlobalLock(handle);
      if (!dest) {
        GlobalFree(handle);
        return false;
      }
      std::memcpy(dest, data, size);
      GlobalUnlock(handle);

      // The clipboard owns the memory once it's set
      if (!SetClipboardData(format, handle)) {
        GlobalFree(handle);
        return false;
      }
      return true;
    }

    /**
     * @brief The Windows clipboard, watched and set from a dedicated window thread.
     */
    class win_clipboard_t: public clipboard_t {
    public:
      win_clipboard_t() {
        png_format = RegisterClipboardFormatW(L"PNG");
        drop_effect_format = RegisterClipboardFormatW(L"Preferred DropEffect");
        running_as_system = is_running_as_system();

        std::promise<bool> started;
        auto started_future = started.get_future();
        thread = std::thread(&win_clipboard_t::run, this, std::move(started));
        if (!started_future.get()) {
          thread.join();
          throw std::runtime_error("couldn't start the clipboard thread");
        }
      }

      ~win_clipboard_t() override {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        thread.join();
      }

      int add_listener(listener_t listener) override {
        std::lock_guard lg(listeners_mutex);
        listeners[next_listener_id] = std::move(listener);
        return next_listener_id++;
      }

      void remove_listener(int id) override {
        std::lock_guard lg(listeners_mutex);
        listeners.erase(id);
      }

      void set(const clipboard_sync::content_t &content) override {
        {
          std::lock_guard lg(pending_mutex);
          pending_set = content;
        }
        PostMessageW(hwnd, WM_APP_SET_CLIPBOARD, 0, 0);
      }

      /**
       * @brief Find the directory for received files of the desktop user.
       *
       * Files are pasted by the user's applications, so they're stored in the user's own temporary
       * directory, which other users can't read.
       */
      fs::path cache_dir() override {
        fs::path path;

        if (auto token = user_token()) {
          PWSTR local_app_data = nullptr;
          if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, token.get(), &local_app_data))) {
            path = fs::path(local_app_data) / L"Temp" / L"Sunshine Clipboard";
          }
          CoTaskMemFree(local_app_data);
        } else if (!running_as_system) {
          std::error_code ec;
          path = fs::temp_directory_path(ec) / L"Sunshine Clipboard";
        }

        // Nobody is logged on. Only the user may write files, so files aren't received then.
        if (path.empty()) {
          path = appdata() / "clipboard";
        }

        BOOST_LOG(debug) << "Clipboard files are stored in "sv << path.string();
        return path;
      }

      /**
       * @brief Impersonate the desktop user while running work, since Sunshine runs as SYSTEM.
       *
       * Without a logged on user, the work runs as the anonymous user, which can't access files.
       */
      void run_as_user(const std::function<void()> &work) override {
        if (!running_as_system) {
          work();
          return;
        }

        auto token = user_token();
        if (!(token ? ImpersonateLoggedOnUser(token.get()) : ImpersonateAnonymousToken(GetCurrentThread()))) {
          BOOST_LOG(error) << "Clipboard: couldn't impersonate the user: "sv << GetLastError();
          return;
        }
        auto revert = util::fail_guard([]() {
          RevertToSelf();
        });

        work();
      }

    private:
      /**
       * @brief Get the token of the user logged on at the console.
       * @return The token, or nullptr if nobody is logged on or Sunshine doesn't run as SYSTEM.
       */
      std::shared_ptr<void> user_token() {
        if (!running_as_system) {
          return nullptr;
        }

        auto session_id = WTSGetActiveConsoleSessionId();
        auto now = std::chrono::steady_clock::now();

        std::lock_guard lg(token_mutex);
        if (session_id != token_session_id || now - token_time > USER_TOKEN_LIFETIME) {
          cached_token.reset();
          token_session_id = session_id;
          token_time = now;

          HANDLE handle;
          if (session_id != 0xFFFFFFFF && WTSQueryUserToken(session_id, &handle)) {
            cached_token = std::shared_ptr<void>(handle, CloseHandle);
          }
        }
        return cached_token;
      }

      void run(std::promise<bool> started) {
        // WIC is only used from this thread
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()));

        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SunshineClipboardSync";
        RegisterClassExW(&wc);

        // A message-only window receives clipboard notifications and owns our clipboard contents
        hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
        if (!hwnd || !AddClipboardFormatListener(hwnd)) {
          BOOST_LOG(error) << "Couldn't watch the clipboard: "sv << GetLastError();
          if (hwnd) {
            DestroyWindow(hwnd);
          }
          started.set_value(false);
          CoUninitialize();
          return;
        }
        started.set_value(true);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
          DispatchMessageW(&msg);
        }

        wic.put();
        CoUninitialize();
      }

      static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_NCCREATE) {
          auto create = (CREATESTRUCTW *) lparam;
          SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) create->lpCreateParams);
        }

        auto self = (win_clipboard_t *) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        switch (msg) {
          case WM_CLIPBOARDUPDATE:
            SetTimer(hwnd, READ_TIMER_ID, READ_DELAY_MS, nullptr);
            return 0;
          case WM_TIMER:
            if (wparam == READ_TIMER_ID) {
              KillTimer(hwnd, READ_TIMER_ID);
              self->on_clipboard_changed();
            }
            return 0;
          case WM_APP_SET_CLIPBOARD:
            self->apply_pending_set();
            return 0;
          case WM_CLOSE:
            RemoveClipboardFormatListener(hwnd);
            DestroyWindow(hwnd);
            return 0;
          case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
          default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
      }

      /**
       * @brief Open the clipboard, retrying while another application holds it.
       */
      bool open_clipboard() {
        for (int i = 0; i < 20; i++) {
          if (OpenClipboard(hwnd)) {
            return true;
          }
          Sleep(10);
        }
        BOOST_LOG(warning) << "Couldn't open the clipboard: "sv << GetLastError();
        return false;
      }

      void on_clipboard_changed() {
        // Ignore our own contents, and don't bother reading if no client is syncing
        if (GetClipboardOwner() == hwnd) {
          return;
        }
        {
          std::lock_guard lg(listeners_mutex);
          if (listeners.empty()) {
            return;
          }
        }

        clipboard_sync::content_t content;
        std::optional<std::vector<uint8_t>> dib;

        if (!open_clipboard()) {
          return;
        }

        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
          if (auto data = read_global(GetClipboardData(CF_UNICODETEXT), 64 << 20)) {
            auto chars = (const wchar_t *) data->data();
            auto length = wcsnlen(chars, data->size() / sizeof(wchar_t));
            if (length > 0) {
              content.text = to_lf(utf_utils::to_utf8(std::wstring(chars, length)));
            }
          }
        }

        // Many applications provide PNG, which keeps transparency. The others provide a DIB.
        if (png_format && IsClipboardFormatAvailable(png_format)) {
          content.png = read_global(GetClipboardData(png_format), MAX_IMAGE_SIZE);
        } else if (IsClipboardFormatAvailable(CF_DIBV5)) {
          dib = read_global(GetClipboardData(CF_DIBV5), MAX_IMAGE_SIZE);
        } else if (IsClipboardFormatAvailable(CF_DIB)) {
          dib = read_global(GetClipboardData(CF_DIB), MAX_IMAGE_SIZE);
        }

        if (IsClipboardFormatAvailable(CF_HDROP)) {
          if (auto drop = (HDROP) GetClipboardData(CF_HDROP)) {
            auto count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; i++) {
              auto length = DragQueryFileW(drop, i, nullptr, 0);
              std::wstring path(length, L'\0');
              DragQueryFileW(drop, i, path.data(), length + 1);
              if (is_file_system_path(path)) {
                content.files.emplace_back(path);
              }
            }
          }
        }

        CloseClipboard();

        // Converting the image can take a while, so it happens after the clipboard is closed
        if (dib) {
          content.png = dib_to_png(*dib);
        }

        if (content.empty()) {
          return;
        }

        std::lock_guard lg(listeners_mutex);
        for (auto &listener : listeners) {
          listener.second(content);
        }
      }

      void apply_pending_set() {
        std::optional<clipboard_sync::content_t> content;
        {
          std::lock_guard lg(pending_mutex);
          content.swap(pending_set);
        }
        if (!content) {
          return;
        }

        // Prepare everything before opening the clipboard to hold it as briefly as possible
        std::optional<std::vector<uint8_t>> dib;
        if (content->png) {
          dib = png_to_dib(*content->png);
        }

        if (!open_clipboard()) {
          return;
        }

        // Emptying the clipboard makes our window its owner
        EmptyClipboard();

        if (content->text) {
          auto text = utf_utils::from_utf8(to_crlf(*content->text));
          set_global(CF_UNICODETEXT, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        }

        if (content->png) {
          set_global(png_format, content->png->data(), content->png->size());
          if (dib) {
            set_global(CF_DIBV5, dib->data(), dib->size());
          }
        }

        if (!content->files.empty()) {
          // DROPFILES is followed by a double NUL terminated list of paths
          std::wstring paths;
          for (auto &file : content->files) {
            paths += file.wstring();
            paths += L'\0';
          }
          paths += L'\0';

          std::vector<uint8_t> drop(sizeof(DROPFILES) + paths.size() * sizeof(wchar_t));
          auto header = (DROPFILES *) drop.data();
          header->pFiles = sizeof(DROPFILES);
          header->fWide = TRUE;
          std::memcpy(drop.data() + sizeof(DROPFILES), paths.data(), paths.size() * sizeof(wchar_t));
          set_global(CF_HDROP, drop.data(), drop.size());

          // Pasting copies the files instead of moving them
          DWORD effect = DROPEFFECT_COPY;
          set_global(drop_effect_format, &effect, sizeof(effect));
        }

        CloseClipboard();
      }

      /**
       * @brief Encode a WIC image as PNG.
       */
      std::optional<std::vector<uint8_t>> encode_png(IWICBitmapSource *source) {
        com_ptr<IWICBitmapSource> converted;
        if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, source, converted.put()))) {
          return std::nullopt;
        }

        com_ptr<IStream> stream;
        com_ptr<IWICBitmapEncoder> encoder;
        com_ptr<IWICBitmapFrameEncode> frame;
        com_ptr<IPropertyBag2> properties;
        UINT width, height;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put())) ||
            FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) ||
            FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(frame.put(), properties.put())) ||
            FAILED(frame->Initialize(properties.get())) ||
            FAILED(converted->GetSize(&width, &height)) ||
            FAILED(frame->SetSize(width, height)) ||
            FAILED(frame->SetPixelFormat(&format)) ||
            FAILED(frame->WriteSource(converted.get(), nullptr)) ||
            FAILED(frame->Commit()) ||
            FAILED(encoder->Commit())) {
          return std::nullopt;
        }

        STATSTG stat;
        HGLOBAL memory;
        if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || FAILED(GetHGlobalFromStream(stream.get(), &memory))) {
          return std::nullopt;
        }

        auto data = (const uint8_t *) GlobalLock(memory);
        if (!data) {
          return std::nullopt;
        }
        std::vector<uint8_t> png(data, data + stat.cbSize.QuadPart);
        GlobalUnlock(memory);
        return png;
      }

      /**
       * @brief Convert a clipboard DIB to PNG.
       */
      std::optional<std::vector<uint8_t>> dib_to_png(const std::vector<uint8_t> &dib) {
        if (!wic || dib.size() < sizeof(BITMAPINFOHEADER)) {
          return std::nullopt;
        }

        // Prepending a file header lets WIC's BMP decoder handle every DIB variant
        auto info = (const BITMAPINFOHEADER *) dib.data();
        DWORD masks_size = 0;
        if (info->biSize == sizeof(BITMAPINFOHEADER)) {
          if (info->biCompression == BI_BITFIELDS) {
            masks_size = 3 * sizeof(DWORD);
          } else if (info->biCompression == 6 /* BI_ALPHABITFIELDS */) {
            masks_size = 4 * sizeof(DWORD);
          }
        }
        DWORD colors = info->biClrUsed ? info->biClrUsed : (info->biBitCount <= 8 ? 1u << info->biBitCount : 0);

        BITMAPFILEHEADER file {};
        file.bfType = 0x4D42;  // BM
        file.bfSize = (DWORD) (sizeof(file) + dib.size());
        file.bfOffBits = (DWORD) (sizeof(file) + info->biSize + masks_size + colors * sizeof(RGBQUAD));

        std::vector<uint8_t> bmp(sizeof(file) + dib.size());
        std::memcpy(bmp.data(), &file, sizeof(file));
        std::memcpy(bmp.data() + sizeof(file), dib.data(), dib.size());

        com_ptr<IWICStream> stream;
        com_ptr<IWICBitmapDecoder> decoder;
        com_ptr<IWICBitmapFrameDecode> frame;
        if (FAILED(wic->CreateStream(stream.put())) ||
            FAILED(stream->InitializeFromMemory(bmp.data(), (DWORD) bmp.size())) ||
            FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
            FAILED(decoder->GetFrame(0, frame.put()))) {
          BOOST_LOG(warning) << "Couldn't decode the clipboard image"sv;
          return std::nullopt;
        }

        return encode_png(frame.get());
      }

      /**
       * @brief Convert PNG to a 32-bit DIB with alpha for applications that don't read PNG.
       */
      std::optional<std::vector<uint8_t>> png_to_dib(const std::vector<uint8_t> &png) {
        if (!wic) {
          return std::nullopt;
        }

        com_ptr<IWICStream> stream;
        com_ptr<IWICBitmapDecoder> decoder;
        com_ptr<IWICBitmapFrameDecode> frame;
        com_ptr<IWICBitmapSource> converted;
        UINT width, height;
        if (FAILED(wic->CreateStream(stream.put())) ||
            FAILED(stream->InitializeFromMemory((BYTE *) png.data(), (DWORD) png.size())) ||
            FAILED(wic->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
            FAILED(decoder->GetFrame(0, frame.put())) ||
            FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.get(), converted.put())) ||
            FAILED(converted->GetSize(&width, &height)) ||
            (uint64_t) width * height * 4 > MAX_IMAGE_SIZE) {
          BOOST_LOG(warning) << "Couldn't decode the received clipboard image"sv;
          return std::nullopt;
        }

        UINT stride = width * 4;
        std::vector<uint8_t> pixels((size_t) stride * height);
        if (FAILED(converted->CopyPixels(nullptr, stride, (UINT) pixels.size(), pixels.data()))) {
          return std::nullopt;
        }

        BITMAPV5HEADER header {};
        header.bV5Size = sizeof(header);
        header.bV5Width = (LONG) width;
        header.bV5Height = (LONG) height;  // Bottom-up
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5SizeImage = (DWORD) pixels.size();
        header.bV5RedMask = 0x00FF0000;
        header.bV5GreenMask = 0x0000FF00;
        header.bV5BlueMask = 0x000000FF;
        header.bV5AlphaMask = 0xFF000000;
        header.bV5CSType = LCS_sRGB;
        header.bV5Intent = LCS_GM_IMAGES;

        std::vector<uint8_t> dib(sizeof(header) + pixels.size());
        std::memcpy(dib.data(), &header, sizeof(header));
        for (UINT y = 0; y < height; y++) {
          std::memcpy(dib.data() + sizeof(header) + (size_t) y * stride, pixels.data() + (size_t) (height - 1 - y) * stride, stride);
        }
        return dib;
      }

      HWND hwnd = nullptr;
      std::thread thread;
      UINT png_format = 0;
      UINT drop_effect_format = 0;
      bool running_as_system = false;
      com_ptr<IWICImagingFactory> wic;

      std::mutex token_mutex;
      std::shared_ptr<void> cached_token;
      DWORD token_session_id = 0xFFFFFFFF;
      std::chrono::steady_clock::time_point token_time;

      std::mutex listeners_mutex;
      std::map<int, listener_t> listeners;
      int next_listener_id = 0;

      std::mutex pending_mutex;
      std::optional<clipboard_sync::content_t> pending_set;
    };
  }  // namespace

  std::shared_ptr<clipboard_t> clipboard() {
    // The clipboard is watched for the lifetime of the process once a client could use it
    static std::mutex mutex;
    static std::shared_ptr<clipboard_t> instance;
    static bool failed = false;

    std::lock_guard lg(mutex);
    if (!instance && !failed) {
      try {
        instance = std::make_shared<win_clipboard_t>();
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "Clipboard sync is unavailable: "sv << e.what();
        failed = true;
      }
    }
    return instance;
  }

}  // namespace platf
