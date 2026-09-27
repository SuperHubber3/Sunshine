/**
 * @file src/pyrowave/pyrowave_encoder.cpp
 * @brief Definitions for the PyroWave encoder wrapper.
 */
// standard includes
#include <algorithm>
#include <cstdio>

// local includes
#include "pyrowave_encoder.h"
#include "src/logging.h"

namespace pyrowave {

  encoder::~encoder() {
    destroy_encoder();
    if (timeline_) {
      pyrowave_sync_object_destroy(timeline_);
      timeline_ = nullptr;
    }
    if (device_) {
      pyrowave_device_destroy(device_);
      device_ = nullptr;
    }
  }

  bool encoder::create_device(const device_id_t &id) {
    uint32_t major, minor, patch;
    pyrowave_get_api_version(&major, &minor, &patch);
    if (major != PYROWAVE_API_VERSION_MAJOR || minor != PYROWAVE_API_VERSION_MINOR) {
      BOOST_LOG(error) << "PyroWave library version " << major << '.' << minor << '.' << patch
                       << " doesn't match the version Sunshine was built against ("
                       << PYROWAVE_API_VERSION_MAJOR << '.' << PYROWAVE_API_VERSION_MINOR << ')';
      return false;
    }

    // A realtime compute queue keeps encode latency low while a game saturates the GPU.
    // The driver silently downgrades the priority if the process isn't allowed to use it.
    auto result = pyrowave_create_device_by_compat2(
      id.vendor_id,
      id.device_id,
      id.device_uuid ? &*id.device_uuid : nullptr,
      nullptr,
      id.luid ? &*id.luid : nullptr,
      VK_QUEUE_GLOBAL_PRIORITY_REALTIME,
      &device_
    );
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "Failed to create PyroWave device: " << result;
      device_ = nullptr;
      return false;
    }

    auto priority = pyrowave_device_get_global_priority(device_);
    BOOST_LOG(info) << "PyroWave compute queue priority: "
                    << (priority >= VK_QUEUE_GLOBAL_PRIORITY_REALTIME ? "realtime" :
                        priority >= VK_QUEUE_GLOBAL_PRIORITY_HIGH     ? "high" :
                                                                        "normal");
    return true;
  }

  bool encoder::create_encoder(int width, int height, bool yuv444, bool hdr) {
    pyrowave_encoder_create_info info {};
    info.device = device_;
    info.width = width;
    info.height = height;
    info.chroma = yuv444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;

    auto result = pyrowave_encoder_create(&info, &encoder_);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "Failed to create PyroWave encoder (" << width << 'x' << height << "): " << result;
      encoder_ = nullptr;
      return false;
    }

    width_ = width;
    height_ = height;
    yuv444_ = yuv444;
    hdr_ = hdr;
    return true;
  }

  int encoder::import_image(pyrowave_os_handle handle, VkExternalMemoryHandleTypeFlagBits handle_type, const VkImageCreateInfo &create_info, uint32_t queue_family) {
    pyrowave_image_create_info info {};
    info.device = device_;
    info.external_handle = handle;
    info.handle_type = handle_type;
    info.image_create_info = &create_info;

    pyrowave_image image;
    auto result = pyrowave_image_create(&info, &image);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "Failed to import image into PyroWave: " << result;
      return -1;
    }

    images_.push_back({image, create_info.format, create_info.extent.width, create_info.extent.height, queue_family});
    return (int) images_.size() - 1;
  }

  void encoder::destroy_encoder() {
    // Destroying the encoder waits for the GPU to go idle, so the images can be released after it
    if (encoder_) {
      pyrowave_encoder_destroy(encoder_);
      encoder_ = nullptr;
    }
    for (auto &image : images_) {
      pyrowave_image_destroy(image.image);
    }
    images_.clear();
  }

  bool encoder::import_timeline(pyrowave_os_handle handle, VkExternalSemaphoreHandleTypeFlagBits handle_type) {
    pyrowave_sync_object_create_info info {};
    info.device = device_;
    info.external_handle = handle;
    info.handle_type = handle_type;
    info.semaphore_type = VK_SEMAPHORE_TYPE_TIMELINE;

    auto result = pyrowave_sync_object_create(&info, &timeline_);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "Failed to import timeline semaphore into PyroWave: " << result;
      timeline_ = nullptr;
      return false;
    }

    return true;
  }

  bool encoder::encode(const encode_input_t &input, size_t max_frame_bytes, std::vector<uint8_t> &bitstream) {
    if (input.image_index < 0 || input.image_index >= (int) images_.size()) {
      return false;
    }
    auto &image = images_[input.image_index];

    pyrowave_scaled_encode_info info {};
    info.view.image = pyrowave_image_get_handle(image.image);
    info.view.width = image.width;
    info.view.height = image.height;
    info.view.image_format = image.format;
    info.view.view_format = input.view_format;
    info.view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    info.view.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
    info.view.layout = VK_IMAGE_LAYOUT_GENERAL;
    info.input_color_space = input.color_space;
    info.output_color_space = hdr_ ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    // 16-bit intermediates are required to preserve HDR precision. 8-bit SDR intermediates are dithered.
    info.intermediate_plane_format = hdr_ ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    // Clients decode SDR into 8-bit planes, where neutral chroma is 128/255, and HDR into 16-bit planes
    info.ycbcr_chroma_midpoint = hdr_ ? 0.5f : 128.0f / 255.0f;
    info.crop_rect = input.crop ? &*input.crop : nullptr;

    // The exporter owns the image between frames, so ownership moves to us and back every frame.
    pyrowave_gpu_external_reference ref {image.image, image.queue_family};

    pyrowave_gpu_sync_operation acquire {};
    acquire.images = &ref;
    acquire.num_images = 1;
    if (timeline_ && input.acquire_value) {
      acquire.sync.semaphore = pyrowave_sync_object_get_semaphore(timeline_);
      acquire.sync.value = input.acquire_value;
    }

    pyrowave_gpu_sync_operation release {};
    release.images = &ref;
    release.num_images = 1;

    pyrowave_rate_control rate_control {};
    rate_control.maximum_bitstream_size = max_frame_bytes;

    auto result = pyrowave_encoder_encode_gpu_scaled_synchronous(encoder_, &acquire, &release, &info, &rate_control);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave encode failed: " << result;
      return false;
    }

    return read_bitstream(bitstream);
  }

  bool encoder::encode_black(size_t max_frame_bytes, std::vector<uint8_t> &bitstream) {
    auto chroma_width = yuv444_ ? width_ : width_ / 2;
    auto chroma_height = yuv444_ ? height_ : height_ / 2;
    size_t luma_size = (size_t) width_ * height_;
    size_t chroma_size = (size_t) chroma_width * chroma_height;

    // Full range black: Y = 0, neutral chroma
    if (black_planes_.empty()) {
      black_planes_.resize(luma_size + 2 * chroma_size, 0);
      std::fill(black_planes_.begin() + luma_size, black_planes_.end(), 128);
    }

    pyrowave_cpu_buffer buffer {};
    buffer.data[0] = black_planes_.data();
    buffer.data[1] = black_planes_.data() + luma_size;
    buffer.data[2] = black_planes_.data() + luma_size + chroma_size;
    buffer.row_stride_in_bytes[0] = width_;
    buffer.row_stride_in_bytes[1] = buffer.row_stride_in_bytes[2] = chroma_width;
    buffer.plane_size_in_bytes[0] = luma_size;
    buffer.plane_size_in_bytes[1] = buffer.plane_size_in_bytes[2] = chroma_size;
    buffer.width = width_;
    buffer.height = height_;
    buffer.format = yuv444_ ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;

    pyrowave_rate_control rate_control {};
    rate_control.maximum_bitstream_size = max_frame_bytes;

    auto result = pyrowave_encoder_encode_cpu_synchronous(encoder_, &buffer, &rate_control);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave black frame encode failed: " << result;
      return false;
    }

    return read_bitstream(bitstream);
  }

  bool encoder::read_bitstream(std::vector<uint8_t> &bitstream) {
    // Packetizing blocks until the GPU finishes. A boundary of SIZE_MAX yields the whole
    // frame as one contiguous run, starting with the start-of-frame header. The transport
    // layer splits it into network packets itself.
    size_t num_packets = 0;
    auto result = pyrowave_encoder_compute_num_packets(encoder_, SIZE_MAX, &num_packets);
    if (result != PYROWAVE_SUCCESS || num_packets == 0) {
      BOOST_LOG(error) << "PyroWave packet count failed: " << result;
      return false;
    }

    std::vector<pyrowave_packet> packets(num_packets);

    // Packetizing doesn't bounds check its output in release builds. Rate control keeps the frame
    // within max_frame_bytes, but the encoder's own bitstream buffer is a hard upper bound.
    const void *mapped_bitstream, *mapped_meta;
    size_t mapped_bitstream_size, mapped_meta_size;
    result = pyrowave_encoder_get_mapped_raw_bitstream(encoder_, &mapped_bitstream, &mapped_bitstream_size, &mapped_meta, &mapped_meta_size);
    if (result != PYROWAVE_SUCCESS) {
      BOOST_LOG(error) << "PyroWave bitstream readback failed: " << result;
      return false;
    }

    bitstream.resize(mapped_bitstream_size + sizeof(uint64_t));
    result = pyrowave_encoder_packetize(encoder_, packets.data(), SIZE_MAX, &num_packets, bitstream.data(), bitstream.size());
    if (result != PYROWAVE_SUCCESS || num_packets == 0) {
      BOOST_LOG(error) << "PyroWave packetization failed: " << result;
      return false;
    }

    auto &last = packets[num_packets - 1];
    bitstream.resize(last.offset + last.size);
    return true;
  }

  const char *library_version() {
    static char version[32];
    uint32_t major, minor, patch;
    pyrowave_get_api_version(&major, &minor, &patch);
    std::snprintf(version, sizeof(version), "%u.%u.%u", major, minor, patch);
    return version;
  }

}  // namespace pyrowave
