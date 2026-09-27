/**
 * @file src/platform/linux/pyrowave_linux.h
 * @brief Declarations for PyroWave encoding of DMA-BUF captures.
 */
#pragma once

// standard includes
#include <memory>

// local includes
#include "src/platform/common.h"

namespace pyrowave_linux {

  /**
   * @brief Create a PyroWave encode device for captures that produce egl::img_descriptor_t DMA-BUFs.
   *
   * @param width Capture width in pixels.
   * @param height Capture height in pixels.
   * @param offset_x Horizontal offset of the captured display within the DMA-BUF.
   * @param offset_y Vertical offset of the captured display within the DMA-BUF.
   * @param render_fd DRM file descriptor of the capture GPU, or -1 to use the default GPU.
   * @return Encode device, or nullptr on failure.
   */
  std::unique_ptr<platf::pyrowave_encode_device_t> make_encode_device(int width, int height, int offset_x, int offset_y, int render_fd);

}  // namespace pyrowave_linux
