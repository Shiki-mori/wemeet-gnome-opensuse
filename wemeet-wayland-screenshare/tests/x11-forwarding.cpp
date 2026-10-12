#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <sys/shm.h>
#include <cstdio>

int main() {
  Display* display = XOpenDisplay(nullptr);
  if (!display) return 77;
  const int screen = DefaultScreen(display);
  const Window root = RootWindow(display, screen);
  int event = 0, error = 0, major = 0, minor = 0;
  Bool pixmaps = False;
  if (!XDamageQueryExtension(display, &event, &error) ||
      !XShmQueryVersion(display, &major, &minor, &pixmaps)) return 1;
  const Damage damage = XDamageCreate(display, root, XDamageReportNonEmpty);
  if (!damage) return 1;
  // Xwayland's invisible root window is not directly readable. A private
  // pixmap tests forwarding without capturing the user's desktop.
  const Pixmap drawable = XCreatePixmap(display, root, 8, 8, DefaultDepth(display, screen));
  GC gc = XCreateGC(display, drawable, 0, nullptr);
  XSetForeground(display, gc, 0x123456);
  XFillRectangle(display, drawable, gc, 0, 0, 8, 8);
  XImage* image = XGetImage(display, drawable, 0, 0, 8, 8, AllPlanes, ZPixmap);
  if (!image) return 1;
  XDestroyImage(image);

  XShmSegmentInfo shm{};
  image = XShmCreateImage(display, DefaultVisual(display, screen),
    DefaultDepth(display, screen), ZPixmap, nullptr, &shm, 8, 8);
  if (!image) return 1;
  shm.shmid = shmget(IPC_PRIVATE, image->bytes_per_line * image->height, IPC_CREAT | 0600);
  if (shm.shmid < 0) return 1;
  shm.shmaddr = static_cast<char*>(shmat(shm.shmid, nullptr, 0));
  if (shm.shmaddr == reinterpret_cast<char*>(-1)) {
    shmctl(shm.shmid, IPC_RMID, nullptr);
    return 1;
  }
  image->data = shm.shmaddr;
  shm.readOnly = False;
  const Bool attached = XShmAttach(display, &shm);
  XSync(display, False);
  const Bool captured = attached && XShmGetImage(display, drawable, image, 0, 0, AllPlanes);
  const Bool detached = attached && XShmDetach(display, &shm);
  XSync(display, False);
  shmdt(shm.shmaddr);
  shmctl(shm.shmid, IPC_RMID, nullptr);
  image->data = nullptr;
  XDestroyImage(image);
  XFreeGC(display, gc);
  XFreePixmap(display, drawable);
  XDamageDestroy(display, damage);
  XCloseDisplay(display);
  std::printf("XDamage/XShm supported; XGetImage succeeds; shm attach=%d capture=%d detach=%d\n", attached, captured, detached);
  return attached && captured && detached ? 0 : 1;
}
