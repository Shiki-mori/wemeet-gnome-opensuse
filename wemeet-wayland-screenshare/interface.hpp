#pragma once

#include <atomic>

#include "format.hpp"
#include "framebuf.hpp"



struct Interface;
struct XdpScreencastPortal;
struct PipewireScreenCast;

struct InterfaceSingleton{

  using THIS_CLASS = InterfaceSingleton;

  
  inline static InterfaceSingleton& getSingleton(){
    static InterfaceSingleton singleton;
    return singleton;
  }

  // the pointer to the actual interface
  // should be allocated, and freed by the hook
  std::atomic<Interface*> interface_handle{nullptr};

  // the pointer to the screencast portal object
  // should be allocated, and freed by the hook
  std::atomic<XdpScreencastPortal*> portal_handle{nullptr};

  // the pointer to the pipewire screencast object
  // should be allocated, and freed by the hook
  std::atomic<PipewireScreenCast*> pipewire_handle{nullptr};


private:

  InterfaceSingleton() = default;


};

struct Interface {

  Interface() = delete;
  Interface(
    uint32_t fb_allocation_height,
    uint32_t fb_allocation_width,
    uint32_t init_frame_height,
    uint32_t init_frame_width,
    SpaVideoFormat_e const& init_frame_format
  ):framebuf(fb_allocation_height, fb_allocation_width, init_frame_height, init_frame_width, init_frame_format),
    pw_stop_flag(false),
    payload_pw_stop_confirm(false),
    payload_gio_stop_confirm(false),
    x11_sanitizer_stop_flag(false)
  {}
  
  // atomic flag for the hook to stop the payload
  // should only be written by the hook, and read by the payload
  std::atomic<bool> pw_stop_flag;

  // payload pw stop confirmation flag
  // must be set by payload main thread
  std::atomic<bool> payload_pw_stop_confirm;

  // payload gio stop confirmation flag
  // must be set by payload main thread
  std::atomic<bool> payload_gio_stop_confirm;

  // payload x11 redirect sanitizer stop flag
  // managed by the payload, the hook does not need to care
  std::atomic<bool> x11_sanitizer_stop_flag;

  // Set after PipeWire has copied at least one real frame.
  std::atomic<bool> frames_ready{false};

  // the framebuffer between the hook and the payload
  FrameBuffer framebuf;
};