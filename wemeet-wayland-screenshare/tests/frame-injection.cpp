#include "interface.hpp"
#include <X11/Xlib.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

bool XShmGetImageHook(XImage&, const std::shared_ptr<CaptureSession>&);

int main() {
  auto capture = std::make_shared<CaptureSession>();
  auto& frame = capture->interface.framebuf;
  frame.update_param(2, 2, SpaVideoFormat_e::RGBA);
  const std::array<unsigned char, 16> source{
    255, 0, 0, 255, 0, 255, 0, 255,
    0, 0, 255, 255, 255, 255, 255, 255};
  memcpy(frame.data.get(), source.data(), source.size());
  std::array<unsigned char, 24> destination;
  destination.fill(0x7a);
  const auto original = destination;
  XImage image{};
  image.width = image.height = 2;
  image.format = ZPixmap;
  image.bits_per_pixel = 32;
  image.bytes_per_line = 12;
  image.red_mask = 0xff0000;
  image.green_mask = 0xff00;
  image.blue_mask = 0xff;
  image.data = reinterpret_cast<char*>(destination.data());
  if (XShmGetImageHook(image, capture) || destination != original) return 1;
  capture->interface.frames_ready.store(true);
  capture->interface.accepted_frames.store(1);
  if (!XShmGetImageHook(image, capture)) return 1;
  const std::array<unsigned char, 24> expected{
    0, 0, 255, 255, 0, 255, 0, 255, 0, 0, 0, 0,
    255, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 0};
  if (destination != expected) return 1;
  // A later frame must replace the image already read by the client.
  frame.data[0] = 123;
  capture->interface.accepted_frames.store(2);
  if (!XShmGetImageHook(image, capture) || destination[2] != 123 ||
      capture->interface.last_injected_frame.load() != 2 ||
      capture->interface.injected_frames.load() != 2) return 1;
  frame.width = 0;
  destination = original;
  if (XShmGetImageHook(image, capture) || destination != original) return 1;
  std::puts("unready/invalid pixels intact; color conversion, padded rows and subsequent frame replacement pass");
}
