#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>

#include "format.hpp"

constexpr uint32_t DEFAULT_FB_ALLOC_HEIGHT = 8192;
constexpr uint32_t DEFAULT_FB_ALLOC_WIDTH = 8192;


struct FrameBuffer {

  FrameBuffer() = delete;

  inline FrameBuffer(
    uint32_t allocation_height,
    uint32_t allocation_width,
    uint32_t init_height,
    uint32_t init_width,
    SpaVideoFormat_e const& format
  ){
    data_size = allocation_height * allocation_width * 4;
    data.reset(new uint8_t[data_size]);
    update_param(init_height, init_width, format);
  }
  
  inline void update_param(
    uint32_t height,
    uint32_t width,
    SpaVideoFormat_e const& format
  ){

    int bytes_per_pixel = spa_videoformat_bytesize(format);
    if (bytes_per_pixel == -1) {
      throw std::runtime_error("Invalid format");
    }

    // always store in (height, width):(stride, 1) layout
    uint32_t needed_stride = (width * bytes_per_pixel + 4 - 1) / 4 * 4;
    this->height = height;
    this->width = width;
    this->row_byte_stride = needed_stride;
    this->format = format;
  }

  std::unique_ptr<uint8_t[]> data{nullptr};
  size_t data_size{0};
  uint32_t height{0};
  uint32_t width{0};
  uint32_t row_byte_stride{0};
  SpaVideoFormat_e format{SpaVideoFormat_e::INVALID};

  int crop_x{0}, crop_y{0}, crop_width{0}, crop_height{0};
  int rotate{0};
  int flip{0};

};
