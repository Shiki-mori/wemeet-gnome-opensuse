#pragma once

#include <string>
#include <spa/param/video/format-utils.h>
#include <X11/Xlib.h>

enum class SpaVideoFormat_e {
  RGBx = SPA_VIDEO_FORMAT_RGBx,
  BGRx = SPA_VIDEO_FORMAT_BGRx,
  RGBA = SPA_VIDEO_FORMAT_RGBA,
  BGRA = SPA_VIDEO_FORMAT_BGRA,
  RGB = SPA_VIDEO_FORMAT_RGB,
  BGR = SPA_VIDEO_FORMAT_BGR,
  INVALID = -1
};

inline std::string spa_to_string(SpaVideoFormat_e const& format){
  switch (format) {
    case SpaVideoFormat_e::RGBx:
      return "RGBx";
    case SpaVideoFormat_e::BGRx:
      return "BGRx";
    case SpaVideoFormat_e::RGBA:
      return "RGBA";
    case SpaVideoFormat_e::BGRA:
      return "BGRA";
    case SpaVideoFormat_e::RGB:
      return "RGB";
    case SpaVideoFormat_e::BGR:
      return "BGR";
    default:
      return "INVALID";
  }
}

// 0 = no conversion, 1 = swap R and B in 4-byte pixels (RGBA <-> BGRA)
inline int needs_rb_swap(
  SpaVideoFormat_e const& src_format,
  SpaVideoFormat_e const& dst_format
){
  auto is_rgb = [](SpaVideoFormat_e f) {
    return f == SpaVideoFormat_e::RGBA || f == SpaVideoFormat_e::RGBx || f == SpaVideoFormat_e::RGB;
  };
  auto is_bgr = [](SpaVideoFormat_e f) {
    return f == SpaVideoFormat_e::BGRA || f == SpaVideoFormat_e::BGRx || f == SpaVideoFormat_e::BGR;
  };
  if (src_format == dst_format) {
    return 0;
  }
  if ((is_rgb(src_format) && is_rgb(dst_format)) || (is_bgr(src_format) && is_bgr(dst_format))) {
    return 0;
  }
  if ((is_rgb(src_format) && is_bgr(dst_format)) || (is_bgr(src_format) && is_rgb(dst_format))) {
    return 1;
  }
  return 0;
}

// unused leftover from opencv path; kept so older comments still compile if referenced
inline int get_opencv_cAPI_color_convert_code(
  SpaVideoFormat_e const& src_format,
  SpaVideoFormat_e const& dst_format
){
  return needs_rb_swap(src_format, dst_format) ? 1 : -1;
}

#if 0
inline int get_opencv_cAPI_color_convert_code_old(
  SpaVideoFormat_e const& src_format,
  SpaVideoFormat_e const& dst_format
){
  if (src_format == dst_format) {
    return -1;
  }
  if (src_format == SpaVideoFormat_e::RGBA && dst_format == SpaVideoFormat_e::RGBx ||
      src_format == SpaVideoFormat_e::RGBx && dst_format == SpaVideoFormat_e::RGBA
  ) {
    return -1;
  }
  if (src_format == SpaVideoFormat_e::BGRA && dst_format == SpaVideoFormat_e::BGRx ||
      src_format == SpaVideoFormat_e::BGRx && dst_format == SpaVideoFormat_e::BGRA
  ) {
    return -1;
  }
  if (src_format == SpaVideoFormat_e::RGB){
    // RGB -> BGR
    if (dst_format == SpaVideoFormat_e::BGR){
      return CV_RGB2BGR;
    }
    // RGB -> BGRA / BGRx
    if (dst_format == SpaVideoFormat_e::BGRA || dst_format == SpaVideoFormat_e::BGRx){
      return CV_RGB2BGRA;
    }
    // RGB -> RGBA / RGBx
    if (dst_format == SpaVideoFormat_e::RGBA || dst_format == SpaVideoFormat_e::RGBx){
      return CV_RGB2RGBA;
    }
  }

  if (src_format == SpaVideoFormat_e::BGR){
    // BGR -> RGB
    if (dst_format == SpaVideoFormat_e::RGB){
      return CV_BGR2RGB;
    }
    // BGR -> BGRA / BGRx
    if (dst_format == SpaVideoFormat_e::BGRA || dst_format == SpaVideoFormat_e::BGRx){
      return CV_BGR2BGRA;
    }
    // BGR -> RGBA / RGBx
    if (dst_format == SpaVideoFormat_e::RGBA || dst_format == SpaVideoFormat_e::RGBx){
      return CV_BGR2RGBA;
    }
  }

  if (src_format == SpaVideoFormat_e::RGBA || src_format == SpaVideoFormat_e::RGBx){
    // RGBA/RGBx -> RGB
    if (dst_format == SpaVideoFormat_e::RGB){
      return CV_RGBA2RGB;
    }
    // RGBA/RGBx -> BGR
    if (dst_format == SpaVideoFormat_e::BGR){
      return CV_RGBA2BGR;
    }
    // RGBA/RGBx -> BGRA/BGRx
    if (dst_format == SpaVideoFormat_e::BGRA || dst_format == SpaVideoFormat_e::BGRx){
      return CV_RGBA2BGRA;
    }
  }

  if (src_format == SpaVideoFormat_e::BGRA || src_format == SpaVideoFormat_e::BGRx){
    // BGRA/BGRx -> RGB
    if (dst_format == SpaVideoFormat_e::RGB){
      return CV_BGRA2RGB;
    }
    // BGRA/BGRx -> BGR
    if (dst_format == SpaVideoFormat_e::BGR){
      return CV_BGRA2BGR;
    }
    // BGRA/BGRx -> RGBA/RGBx
    if (dst_format == SpaVideoFormat_e::RGBA || dst_format == SpaVideoFormat_e::RGBx){
      return CV_BGRA2RGBA;
    }
  }

  // guard
  return -1;
}
#endif

inline auto spa_videoformat_bytesize(const SpaVideoFormat_e& format) -> int {
  switch (format) {
    case SpaVideoFormat_e::RGBx:
      return 4;
    case SpaVideoFormat_e::BGRx:
      return 4;
    case SpaVideoFormat_e::RGBA:
      return 4;
    case SpaVideoFormat_e::BGRA:
      return 4;
    case SpaVideoFormat_e::RGB:
      return 3;
    case SpaVideoFormat_e::BGR:
      return 3;
    default:
      return -1;
  }
}

inline auto ximage_to_spa(const XImage& ximage) -> SpaVideoFormat_e {
  if (ximage.format != 2){
    // we only support ZPixmap
    return SpaVideoFormat_e::INVALID;
  }
  if (ximage.bits_per_pixel == 32) {
    // possibly RGBA, BGRA, RGBx or BGRx
    // we just combine RGBx and BGRx to RGBA and BGRA, respectively
    if (ximage.red_mask == 0xff0000 && ximage.green_mask == 0xff00 && ximage.blue_mask == 0xff) {
      return SpaVideoFormat_e::BGRA;
    } else if (ximage.red_mask == 0xff && ximage.green_mask == 0xff00 && ximage.blue_mask == 0xff0000) {
      return SpaVideoFormat_e::RGBA;
    } else {
      return SpaVideoFormat_e::INVALID;
    }
  } else {
    // possibly RGB or BGR
    if (ximage.red_mask == 0xff0000 && ximage.green_mask == 0xff00 && ximage.blue_mask == 0xff) {
      return SpaVideoFormat_e::BGR;
    } else if (ximage.red_mask == 0xff && ximage.green_mask == 0xff00 && ximage.blue_mask == 0xff0000) {
      return SpaVideoFormat_e::RGB;
    } else {
      return SpaVideoFormat_e::INVALID;
    }
  }
    
}
