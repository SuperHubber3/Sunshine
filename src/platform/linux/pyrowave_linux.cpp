/**
 * @file src/platform/linux/pyrowave_linux.cpp
 * @brief Definitions for PyroWave encoding of DMA-BUF captures.
 */
// standard includes
#include <array>
#include <vector>

// platform includes
#include <dlfcn.h>
#include <drm_fourcc.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef SUNSHINE_BUILD_DRM
  #include <xf86drm.h>
#endif

// local includes
#include "graphics.h"
#include "pyrowave_linux.h"
#include "src/logging.h"
#include "src/pyrowave/pyrowave_encoder.h"
#include "src/video.h"

using namespace std::literals;

namespace pyrowave_linux {

  /**
   * @brief Map a DRM fourcc to the Vulkan format PyroWave samples it as.
   *
   * @param fourcc DRM fourcc of the DMA-BUF.
   * @return Vulkan format, or VK_FORMAT_UNDEFINED if unsupported.
   */
  static VkFormat vk_format_from_fourcc(uint32_t fourcc) {
    switch (fourcc) {
      case DRM_FORMAT_XRGB8888:
      case DRM_FORMAT_ARGB8888:
        return VK_FORMAT_B8G8R8A8_UNORM;
      case DRM_FORMAT_XBGR8888:
      case DRM_FORMAT_ABGR8888:
        return VK_FORMAT_R8G8B8A8_UNORM;
      case DRM_FORMAT_XRGB2101010:
      case DRM_FORMAT_ARGB2101010:
        return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
      case DRM_FORMAT_XBGR2101010:
      case DRM_FORMAT_ABGR2101010:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      case DRM_FORMAT_NV12:
        return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
      default:
        return VK_FORMAT_UNDEFINED;
    }
  }

  /**
   * @brief Return the Vulkan loader's vkGetInstanceProcAddr, loading it on first use.
   *
   * @return Function pointer, or nullptr if the loader is unavailable.
   */
  static PFN_vkGetInstanceProcAddr get_instance_proc_addr() {
    static PFN_vkGetInstanceProcAddr fn = [] {
      void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
      return lib ? (PFN_vkGetInstanceProcAddr) dlsym(lib, "vkGetInstanceProcAddr") : nullptr;
    }();
    return fn;
  }

  /**
   * @brief PyroWave encode device for DMA-BUF captures.
   *
   * Captures rotate through a few buffers, so every buffer is imported once and kept for the
   * lifetime of the encoder. That also keeps PyroWave's cached views of them valid.
   * The cursor isn't composited on this path.
   */
  class dmabuf_encode_device_t: public platf::pyrowave_encode_device_t {
  public:
    /**
     * @brief Create the PyroWave device on the capture GPU.
     *
     * @param width Capture width in pixels.
     * @param height Capture height in pixels.
     * @param offset_x Horizontal offset of the display within the DMA-BUF.
     * @param offset_y Vertical offset of the display within the DMA-BUF.
     * @param render_fd DRM file descriptor of the capture GPU, or -1.
     * @return True on success.
     */
    bool init_device(int width, int height, int offset_x, int offset_y, int render_fd) {
      capture_rect = {{offset_x, offset_y}, {(uint32_t) width, (uint32_t) height}};

      // Match the capture GPU by PCI ID, otherwise PyroWave picks the default GPU
      pyrowave::device_id_t id;
#ifdef SUNSHINE_BUILD_DRM
      drmDevicePtr drm_device = nullptr;
      if (render_fd >= 0 && drmGetDevice2(render_fd, 0, &drm_device) == 0) {
        if (drm_device->bustype == DRM_BUS_PCI) {
          id.vendor_id = drm_device->deviceinfo.pci->vendor_id;
          id.device_id = drm_device->deviceinfo.pci->device_id;
        }
        drmFreeDevice(&drm_device);
      }
#endif

      return encoder.create_device(id);
    }

    /**
     * @brief Create the PyroWave encoder for the client stream configuration.
     *
     * @param client_config Client stream configuration negotiated for this session.
     * @param colorspace Colorimetry information used for conversion or encoding.
     * @return True on success.
     */
    bool init_encoder(const video::config_t &client_config, const video::sunshine_colorspace_t &colorspace) override {
      width = client_config.width;
      height = client_config.height;
      yuv444 = client_config.chromaSamplingType == 1;

      // Captures are SDR, but the client decodes what it negotiated, so SDR is placed in HDR10 then
      hdr = client_config.dynamicRange > 0;

      max_frame_bytes = (size_t) client_config.bitrate * 1000 / 8 / std::max(client_config.framerate, 1);
      if (client_config.maxFrameSize) {
        max_frame_bytes = std::min(max_frame_bytes, client_config.maxFrameSize);
      }

      BOOST_LOG(info) << "PyroWave frame budget: "sv << max_frame_bytes / 1024 << " KiB"sv;

      return encoder.create_encoder(width, height, yuv444, hdr);
    }

    /**
     * @brief Select the DMA-BUF of a captured frame, importing it the first time it's seen.
     *
     * @param img Captured egl::img_descriptor_t.
     * @return Conversion status.
     */
    int convert(platf::img_t &img) override {
      auto &descriptor = (egl::img_descriptor_t &) img;

      // Dummy frames have no buffer, the previous (or black) frame is repeated
      if (descriptor.sequence == 0) {
        return 0;
      }
      if (descriptor.sd.fds[0] < 0) {
        BOOST_LOG(error) << "PyroWave needs DMA-BUF captures, but the capture delivered a frame in memory"sv;
        return -1;
      }

      struct stat st;
      if (fstat(descriptor.sd.fds[0], &st) < 0) {
        return -1;
      }

      for (auto &cached : images) {
        if (cached.inode == st.st_ino && cached.fourcc == descriptor.sd.fourcc && cached.modifier == descriptor.sd.modifier &&
            cached.width == descriptor.sd.width && cached.height == descriptor.sd.height) {
          current = (int) (&cached - images.data());
          return 0;
        }
      }

      // A capture that keeps allocating new buffers would grow the cache without bound
      if (images.size() >= 8) {
        BOOST_LOG(info) << "PyroWave DMA-BUF cache is full, starting over"sv;
        current = -1;
        images.clear();
        encoder.destroy_encoder();
        if (!encoder.create_encoder(width, height, yuv444, hdr)) {
          return -1;
        }
      }

      auto index = import_dmabuf(descriptor.sd);
      if (index < 0) {
        return -1;
      }

      images.push_back({st.st_ino, descriptor.sd.fourcc, descriptor.sd.modifier, descriptor.sd.width, descriptor.sd.height, index, vk_format_from_fourcc(descriptor.sd.fourcc)});
      current = (int) images.size() - 1;
      return 0;
    }

    /**
     * @brief Encode the most recently converted image.
     *
     * @param bitstream Receives the encoded frame.
     * @return True on success.
     */
    bool encode_frame(std::vector<uint8_t> &bitstream) override {
      // Nothing has been captured yet
      if (current < 0) {
        return encoder.encode_black(max_frame_bytes, bitstream);
      }

      auto &image = images[current];
      pyrowave::encode_input_t input {image.index, image.format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, capture_rect, 0};
      return encoder.encode(input, max_frame_bytes, bitstream);
    }

  private:
    /**
     * @brief Import a DMA-BUF into PyroWave.
     *
     * @param sd DMA-BUF description.
     * @return Image index, or -1 on failure.
     */
    int import_dmabuf(const egl::surface_descriptor_t &sd) {
      auto format = vk_format_from_fourcc(sd.fourcc);
      if (format == VK_FORMAT_UNDEFINED) {
        BOOST_LOG(error) << "PyroWave can't encode DRM format 0x"sv << util::hex(sd.fourcc).to_string_view();
        return -1;
      }

      int planes = 0;
      while (planes < 4 && sd.fds[planes] >= 0) {
        planes++;
      }

      // Buffers can come with metadata planes (like AMD DCC) that the driver may not expect
      auto expected = modifier_plane_count(format, sd.modifier);
      if (expected > 0 && expected < planes) {
        planes = expected;
      }

      std::array<VkSubresourceLayout, 4> layouts {};
      for (int i = 0; i < planes; i++) {
        layouts[i].offset = sd.offsets[i];
        layouts[i].rowPitch = sd.pitches[i];
      }

      VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
      modifier_info.drmFormatModifier = sd.modifier == DRM_FORMAT_MOD_INVALID ? DRM_FORMAT_MOD_LINEAR : sd.modifier;
      modifier_info.drmFormatModifierPlaneCount = planes;
      modifier_info.pPlaneLayouts = layouts.data();

      VkImageCreateInfo info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.pNext = &modifier_info;
      info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      info.imageType = VK_IMAGE_TYPE_2D;
      info.format = format;
      info.extent = {(uint32_t) sd.width, (uint32_t) sd.height, 1};
      info.mipLevels = 1;
      info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
      info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

      int fd = dup(sd.fds[0]);
      struct stat buffer_st;
      if (fd < 0 || fstat(fd, &buffer_st) < 0) {
        if (fd >= 0) {
          close(fd);
        }
        return -1;
      }

      auto index = encoder.import_image((pyrowave_os_handle) fd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, info, VK_QUEUE_FAMILY_FOREIGN_EXT);
      if (index < 0) {
        // Vulkan owns the fd once the memory import succeeded, even if a later step failed. The fd is
        // only still ours if it refers to the buffer, since its number could have been reused.
        struct stat fd_st;
        if (fstat(fd, &fd_st) == 0 && fd_st.st_dev == buffer_st.st_dev && fd_st.st_ino == buffer_st.st_ino) {
          close(fd);
        }
        BOOST_LOG(error) << "PyroWave DMA-BUF import failed (format 0x"sv << util::hex(sd.fourcc).to_string_view()
                         << ", modifier 0x"sv << util::hex(sd.modifier).to_string_view() << ')';
      }
      return index;
    }

    /**
     * @brief Ask the driver how many memory planes a format modifier has.
     *
     * @param format Vulkan format.
     * @param modifier DRM format modifier.
     * @return Plane count, or 0 if unknown.
     */
    int modifier_plane_count(VkFormat format, uint64_t modifier) {
      auto gipa = get_instance_proc_addr();
      if (!gipa) {
        return 0;
      }

      VkInstance instance;
      VkPhysicalDevice physical_device;
      encoder.get_vk_handles(&instance, &physical_device);
      auto get_format_properties2 = (PFN_vkGetPhysicalDeviceFormatProperties2) gipa(instance, "vkGetPhysicalDeviceFormatProperties2");
      if (!get_format_properties2) {
        return 0;
      }

      std::array<VkDrmFormatModifierPropertiesEXT, 32> modifiers {};
      VkDrmFormatModifierPropertiesListEXT list {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
      list.drmFormatModifierCount = modifiers.size();
      list.pDrmFormatModifierProperties = modifiers.data();
      VkFormatProperties2 properties {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
      properties.pNext = &list;
      get_format_properties2(physical_device, format, &properties);

      for (uint32_t i = 0; i < list.drmFormatModifierCount; i++) {
        if (modifiers[i].drmFormatModifier == modifier) {
          return (int) modifiers[i].drmFormatModifierPlaneCount;
        }
      }
      return 0;
    }

    struct cached_image_t {
      ino_t inode;
      uint32_t fourcc;
      uint64_t modifier;
      int width;
      int height;
      int index;
      VkFormat format;
    };

    pyrowave::encoder encoder;
    std::vector<cached_image_t> images;
    int current = -1;  ///< Index into images of the most recently captured buffer.
    VkRect2D capture_rect {};
    int width = 0;
    int height = 0;
    bool yuv444 = false;
    bool hdr = false;
    size_t max_frame_bytes = 0;
  };

  std::unique_ptr<platf::pyrowave_encode_device_t> make_encode_device(int width, int height, int offset_x, int offset_y, int render_fd) {
    auto device = std::make_unique<dmabuf_encode_device_t>();
    if (!device->init_device(width, height, offset_x, offset_y, render_fd)) {
      return nullptr;
    }
    return device;
  }

}  // namespace pyrowave_linux
