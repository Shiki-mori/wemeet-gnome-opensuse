#pragma once

#include <X11/Xlib.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xfixes.h>
#include <cstdint>
#include <string>
#include "logging.hpp"

// PipeWire pixels are invisible to Xwayland's normal drawing damage tracker.
// Use a private connection to report those changes to root-window capturers.
// No client Display is used from the worker, and no desktop pixels are drawn.
class RootDamageNotifier {
public:
  RootDamageNotifier(const std::string& display_name, Window root) {
    if (!root) return; // Isolated portal tests do not need an X11 connection.
    display_ = XOpenDisplay(display_name.empty() ? nullptr : display_name.c_str());
    if (!display_) { hook_log("[damage] cannot open capture display"); return; }
    int screen = -1;
    for (int i = 0; i < ScreenCount(display_); ++i)
      if (RootWindow(display_, i) == root) screen = i;
    int major = 1, minor = 1, fixes_major = 2, fixes_minor = 0;
    if (screen < 0 || !XDamageQueryVersion(display_, &major, &minor) ||
        (major < 1 || (major == 1 && minor < 1)) ||
        !XFixesQueryVersion(display_, &fixes_major, &fixes_minor) || fixes_major < 2) {
      hook_log("[damage] root or required XDamage/XFixes version unavailable");
      return;
    }
    Window geometry_root;
    int x, y;
    unsigned width, height, border, depth;
    if (!XGetGeometry(display_, root, &geometry_root, &x, &y, &width, &height, &border, &depth) ||
        !width || !height || width > 65535 || height > 65535) return;
    XRectangle rectangle{0, 0, static_cast<unsigned short>(width), static_cast<unsigned short>(height)};
    region_ = XFixesCreateRegion(display_, &rectangle, 1);
    root_ = root;
    hook_log("[damage] PipeWire frame notifications enabled root=0x%lx size=%ux%u", root, width, height);
  }

  ~RootDamageNotifier() {
    if (region_) XFixesDestroyRegion(display_, region_);
    if (display_) XCloseDisplay(display_);
  }
  RootDamageNotifier(const RootDamageNotifier&) = delete;
  RootDamageNotifier& operator=(const RootDamageNotifier&) = delete;

  bool notify(uint64_t accepted_frame) {
    if (!region_ || !accepted_frame || accepted_frame == last_frame_) return false;
    // DamageAdd also fills the damage region, so the encoder receives a full
    // frame change rather than a stale X11 window's small dirty rectangle.
    XDamageAdd(display_, root_, region_);
    XFlush(display_);
    last_frame_ = accepted_frame;
    ++notifications_;
    return true;
  }
  uint64_t notifications() const { return notifications_; }

private:
  Display* display_{nullptr};
  Window root_{None};
  XserverRegion region_{None};
  uint64_t last_frame_{0};
  uint64_t notifications_{0};
};
