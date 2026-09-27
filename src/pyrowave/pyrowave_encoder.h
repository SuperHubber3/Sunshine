/**
 * @file src/pyrowave/pyrowave_encoder.h
 * @brief Declarations for the PyroWave encoder wrapper.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <vector>

// lib includes
#include <vulkan/vulkan_core.h>

#include <pyrowave.h>

namespace pyrowave {

  /**
   * @brief Identifies the Vulkan physical device that must run the encoder.
   *        The capture adapter and the encoder must be the same GPU to share memory.
   */
  struct device_id_t {
    std::optional<pyrowave_luid> luid;  ///< Adapter LUID (Windows).
    std::optional<pyrowave_uuid> device_uuid;  ///< Vulkan device UUID (Linux).
    uint32_t vendor_id = 0;  ///< PCI vendor ID, or 0 to match any vendor.
    uint32_t device_id = 0;  ///< PCI device ID, or 0 to match any device.
  };

  /**
   * @brief Describes how a captured image is interpreted for one encode operation.
   */
  struct encode_input_t {
    int image_index;  ///< Index returned by encoder::import_image().
    VkFormat view_format;  ///< Format used to sample the image.
    VkColorSpaceKHR color_space;  ///< Color space of the captured pixels.
    std::optional<VkRect2D> crop;  ///< Optional source rectangle, the full image is used otherwise.
    uint64_t acquire_value;  ///< Timeline value that signals the image contents are ready, or 0 for none.
  };

  /**
   * @brief Owns a PyroWave device, encoder, and the external resources it samples from.
   *
   * Encoding is synchronous: encode() returns once the complete frame bitstream has been read back,
   * so every imported image is idle again when it returns.
   */
  class encoder {
  public:
    encoder() = default;
    ~encoder();

    encoder(const encoder &) = delete;
    encoder &operator=(const encoder &) = delete;

    /**
     * @brief Create the Vulkan device, preferring a high priority compute queue.
     *
     * @param id Physical device to use.
     * @return True on success.
     */
    bool create_device(const device_id_t &id);

    /**
     * @brief Create the encoder for a fixed output size.
     *
     * @param width Encoded width in pixels.
     * @param height Encoded height in pixels.
     * @param yuv444 Encode 4:4:4 instead of 4:2:0.
     * @param hdr Produce BT.2020 PQ instead of BT.709 SDR.
     * @return True on success.
     */
    bool create_encoder(int width, int height, bool yuv444, bool hdr);

    /**
     * @brief Import an external image. Ownership of the handle passes to PyroWave if the import succeeds,
     *        otherwise it stays with the caller.
     *
     * @param handle OS handle (NT HANDLE or file descriptor).
     * @param handle_type Vulkan handle type of `handle`.
     * @param create_info Image description matching the exporter's image.
     * @param queue_family Queue family that owns the image between frames:
     *                     VK_QUEUE_FAMILY_EXTERNAL for D3D11 textures, VK_QUEUE_FAMILY_FOREIGN_EXT for DMA-BUFs.
     * @return Index to pass in encode_input_t, or -1 on failure.
     */
    int import_image(pyrowave_os_handle handle, VkExternalMemoryHandleTypeFlagBits handle_type, const VkImageCreateInfo &create_info, uint32_t queue_family = VK_QUEUE_FAMILY_EXTERNAL);

    /**
     * @brief Destroy the encoder and all imported images, keeping the device and timeline.
     *        The encoder caches views of imported images, so images are only replaced this way.
     */
    void destroy_encoder();

    /**
     * @brief Import a timeline semaphore used to wait for the exporter's writes. Ownership of the handle
     *        passes to PyroWave on success, so pass a duplicate that isn't used afterwards.
     *
     * @param handle OS handle of the fence or semaphore.
     * @param handle_type Vulkan handle type of `handle`.
     * @return True on success.
     */
    bool import_timeline(pyrowave_os_handle handle, VkExternalSemaphoreHandleTypeFlagBits handle_type);

    /**
     * @brief Encode one frame and return the complete bitstream.
     *
     * @param input Image to encode.
     * @param max_frame_bytes Rate control limit for the frame.
     * @param bitstream Receives the encoded frame.
     * @return True on success.
     */
    bool encode(const encode_input_t &input, size_t max_frame_bytes, std::vector<uint8_t> &bitstream);

    /**
     * @brief Encode a black frame, for use before anything has been captured.
     *
     * @param max_frame_bytes Rate control limit for the frame.
     * @param bitstream Receives the encoded frame.
     * @return True on success.
     */
    bool encode_black(size_t max_frame_bytes, std::vector<uint8_t> &bitstream);

    /**
     * @brief Return the Vulkan handles of the device.
     *
     * @param instance Receives the instance.
     * @param physical_device Receives the physical device.
     */
    void get_vk_handles(VkInstance *instance, VkPhysicalDevice *physical_device) const {
      pyrowave_device_get_vk_device_handles(device_, instance, physical_device, nullptr);
    }

    /**
     * @brief Return the encoded width.
     * @return Encoded width in pixels.
     */
    int width() const {
      return width_;
    }

    /**
     * @brief Return the encoded height.
     * @return Encoded height in pixels.
     */
    int height() const {
      return height_;
    }

  private:
    struct imported_image_t {
      pyrowave_image image = nullptr;
      VkFormat format = VK_FORMAT_UNDEFINED;
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t queue_family = VK_QUEUE_FAMILY_EXTERNAL;
    };

    bool read_bitstream(std::vector<uint8_t> &bitstream);

    pyrowave_device device_ = nullptr;
    pyrowave_encoder encoder_ = nullptr;
    pyrowave_sync_object timeline_ = nullptr;
    std::vector<imported_image_t> images_;
    int width_ = 0;
    int height_ = 0;
    bool yuv444_ = false;
    bool hdr_ = false;
    std::vector<uint8_t> black_planes_;
  };

  /**
   * @brief Return the pyrowave library version as a string for logging.
   * @return Version string.
   */
  const char *library_version();

}  // namespace pyrowave
