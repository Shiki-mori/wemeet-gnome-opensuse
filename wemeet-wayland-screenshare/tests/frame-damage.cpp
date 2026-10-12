#include "x11-damage.hpp"
#include <X11/Xutil.h>
#include <cstdio>

int main() {
  Display* observer = XOpenDisplay(nullptr);
  if (!observer) return 1;
  const Window root = DefaultRootWindow(observer);
  int event_base, error_base;
  if (!XDamageQueryExtension(observer, &event_base, &error_base)) return 1;
  auto damage = XDamageCreate(observer, root, XDamageReportNonEmpty);
  auto parts = XFixesCreateRegion(observer, nullptr, 0);
  XDamageSubtract(observer, damage, None, None);
  XSync(observer, False);
  while (XPending(observer)) { XEvent event; XNextEvent(observer, &event); }
  XImage* before = XGetImage(observer, root, 0, 0, 64, 48, AllPlanes, ZPixmap);
  if (!before) return 1;
  {
    RootDamageNotifier notifier(DisplayString(observer), root);
    if (notifier.notify(0)) return 1;
    for (uint64_t frame = 1; frame <= 3; ++frame) {
      if (!notifier.notify(frame) || notifier.notify(frame)) return 1;
      // Flush the notifier's private connection through the server before
      // reading the observer connection, as the client does after XPending.
      bool full_damage = false;
      for (int attempt = 0; attempt < 100 && !full_damage; ++attempt) {
        while (XPending(observer)) {
          XEvent event; XNextEvent(observer, &event);
          if (event.type != event_base + XDamageNotify) continue;
          auto* notice = reinterpret_cast<XDamageNotifyEvent*>(&event);
          if (notice->damage != damage) continue;
          XDamageSubtract(observer, damage, None, parts);
          int count = 0; XRectangle bounds{};
          auto* rectangles = XFixesFetchRegionAndBounds(observer, parts, &count, &bounds);
          full_damage = count == 1 && bounds.x == 0 && bounds.y == 0 &&
                        bounds.width == 64 && bounds.height == 48;
          if (rectangles) XFree(rectangles);
        }
        if (!full_damage) {
          struct timespec pause{0, 1000000};
          nanosleep(&pause, nullptr);
        }
      }
      if (!full_damage) return 1;
    }
    if (notifier.notifications() != 3) return 1;
  }
  XImage* after = XGetImage(observer, root, 0, 0, 64, 48, AllPlanes, ZPixmap);
  if (!after) return 1;
  for (int y = 0; y < 48; ++y)
    for (int x = 0; x < 64; ++x)
      if (XGetPixel(before, x, y) != XGetPixel(after, x, y)) return 1;
  XDestroyImage(before); XDestroyImage(after);
  XFixesDestroyRegion(observer, parts);
  XDamageDestroy(observer, damage);
  XCloseDisplay(observer);
  std::puts("each new frame generates full damage; duplicate/zero frames do not; desktop pixels unchanged");
}
