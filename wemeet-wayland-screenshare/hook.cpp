
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <string>
#include <thread>
#include <tuple>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdlib>
#include <utility>
#include <map>
#include <set>
#include <memory>
#include "logging.hpp"
#include <dlfcn.h>

#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xcomposite.h>

#include "framebuf.hpp"
#include "payload.hpp"
#include "interface.hpp"
#include "helpers.hpp"
#include "hook.hpp"

extern "C" {
#include "plthook.h"
}

extern "C" {
Bool XShmAttach(Display* dpy, XShmSegmentInfo* shminfo);
Bool XShmGetImage(Display* dpy, Drawable d, XImage* image, int x, int y, unsigned long plane_mask);
Bool XShmDetach(Display* dpy, XShmSegmentInfo* shminfo);
XImage* XGetImage(Display* dpy, Drawable d, int x, int y, unsigned int width, unsigned int height, unsigned long plane_mask, int format);
Bool XDamageQueryExtension(Display *dpy, int *event_base_return, int *error_base_return);
int XCopyArea(Display* dpy, Drawable src, Drawable dest, GC gc, int src_x, int src_y, unsigned int width, unsigned int height, int dest_x, int dest_y);
Bool XShmQueryVersion(Display* dpy, int* major, int* minor, Bool* pixmaps);
XImage* XShmCreateImage(Display* dpy, Visual* visual, unsigned int depth, int format, char* data, XShmSegmentInfo* shminfo, unsigned int width, unsigned int height);
Pixmap XShmCreatePixmap(Display* dpy, Drawable d, char* data, XShmSegmentInfo* shminfo, unsigned int width, unsigned int height, unsigned int depth);
Damage XDamageCreate(Display* dpy, Drawable drawable, int level);
void XDamageDestroy(Display* dpy, Damage damage);
int XCloseDisplay(Display* dpy);
void XCompositeRedirectWindow(Display* dpy, Window window, int update);
void* dlopen(const char* file, int flags);
char* getenv(const char* name);
}

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

static std::recursive_mutex g_patch_mu;
static std::atomic<bool> g_xcast_patched{false};

static bool skip_hook_process() {
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n <= 0) {
    return false;
  }
  exe[n] = '\0';
  return std::strstr(exe, "QtWebEngineProcess") != nullptr;
}

static void* real_dlopen_fn(const char* file, int flags) {
  using Fn = void*(const char*, int);
  static Fn* real = nullptr;
  if (!real) {
    real = reinterpret_cast<Fn*>(dlsym(RTLD_NEXT, "dlopen"));
  }
  return real(file, flags);
}

// In this client's screen-share UI, "1" enables its native portal branch.
// Keep the meeting/platform environment at "1", but route this one module
// to X11 capture so each shared session is owned by our portal worker.
static char* screen_share_getenv(const char* name) {
  if (name && std::strcmp(name, "WEMEET_XWAYLAND") == 0) {
    static char value[] = "0";
    static std::atomic<unsigned> queries{0};
    const unsigned query = queries.fetch_add(1) + 1;
    if (query <= 12)
      hook_log("[route] screen-share WEMEET_XWAYLAND=0 query=%u (meeting/platform remains 1)", query);
    return value;
  }
  return getenv(name);
}

static void patch_plts_in_handle(void* handle, const char* tag) {
  std::lock_guard<std::recursive_mutex> lock(g_patch_mu);
  if (!handle) {
    return;
  }
  plthook_t* plt = nullptr;
  if (plthook_open_by_handle(&plt, handle) != PLTHOOK_SUCCESS) {
    hook_log("[plt] open %s failed: %s", tag, plthook_error());
    return;
  }
  struct Item { const char* name; void* fn; };
  const Item items[] = {
    {"XGetImage", reinterpret_cast<void*>(XGetImage)},
    {"XShmAttach", reinterpret_cast<void*>(XShmAttach)},
    {"XShmGetImage", reinterpret_cast<void*>(XShmGetImage)},
    {"XShmDetach", reinterpret_cast<void*>(XShmDetach)},
    {"XDamageQueryExtension", reinterpret_cast<void*>(XDamageQueryExtension)},
    {"XCopyArea", reinterpret_cast<void*>(XCopyArea)},
    {"XShmQueryVersion", reinterpret_cast<void*>(XShmQueryVersion)},
    {"XShmCreateImage", reinterpret_cast<void*>(XShmCreateImage)},
    {"XShmCreatePixmap", reinterpret_cast<void*>(XShmCreatePixmap)},
    {"XDamageCreate", reinterpret_cast<void*>(XDamageCreate)},
    {"XDamageDestroy", reinterpret_cast<void*>(XDamageDestroy)},
    {"XCloseDisplay", reinterpret_cast<void*>(XCloseDisplay)},
    {"XCompositeRedirectWindow", reinterpret_cast<void*>(XCompositeRedirectWindow)},
  };
  int ok = 0;
  for (const auto& it : items) {
    void* oldfn = nullptr;
    int r = plthook_replace(plt, it.name, it.fn, &oldfn);
    if (r == PLTHOOK_SUCCESS) {
      ++ok;
      hook_log("[plt] %s replace %s r=%d old=%p", tag, it.name, r, oldfn);
    }
  }
  if (std::strstr(tag, "libscreen_share_module.so")) {
    void* oldfn = nullptr;
    int result = plthook_replace(plt, "getenv", reinterpret_cast<void*>(screen_share_getenv), &oldfn);
    hook_log("[route] screen-share getenv PLT replace result=%d old=%p", result, oldfn);
    if (result == PLTHOOK_SUCCESS) ++ok;
  }
  plthook_close(plt);
  if (ok > 0) {
    if (std::strstr(tag, "libxcast.so")) g_xcast_patched.store(true);
    hook_log("[plt] %s patched %d symbols", tag, ok);
  }
}

static void try_patch_xcast() {
  std::lock_guard<std::recursive_mutex> lock(g_patch_mu);
  if (g_xcast_patched.load()) {
    return;
  }
  const char* paths[] = {
    "/usr/libexec/wemeet/lib/libxcast.so",
    "/usr/libexec/wemeet/bin/modules/screen_share/libscreen_share_module.so",
  };
  for (const char* path : paths) {
    void* h = real_dlopen_fn(path, RTLD_NOLOAD | RTLD_NOW);
    if (h) {
      hook_log("[plt] found %s handle=%p", path, h);
      patch_plts_in_handle(h, path);
    }
  }
}

static void* patcher_thread_main(void*) {
  for (int i = 0; i < 400 && !g_xcast_patched.load(); ++i) {
    try_patch_xcast();
    if (!g_xcast_patched.load()) {
      usleep(25000);
    }
  }
  hook_log("[plt] patcher done patched=%d", g_xcast_patched.load() ? 1 : 0);
  return nullptr;
}

extern "C" void* dlopen(const char* file, int flags) {
  void* handle = real_dlopen_fn(file, flags);
  if (skip_hook_process() || !file || !handle) {
    return handle;
  }
  if (std::strstr(file, "libxcast.so") || std::strstr(file, "libscreen_share_module.so")) {
    hook_log("[dlopen] %s handle=%p", file, handle);
    patch_plts_in_handle(handle, file);
  }
  return handle;
}

// Preserve the meeting environment. The screen-share module has a separate
// PLT-scoped getter above; its portal-selection flag has different semantics.
static std::atomic<bool> g_lock_client_env{false};

extern "C" char* getenv(const char* name) {
  using Fn = char*(const char*);
  static Fn* real = nullptr;
  if (!real) {
    real = reinterpret_cast<Fn*>(dlsym(RTLD_NEXT, "getenv"));
  }
  if (!real) {
    return nullptr;
  }
  if (!g_lock_client_env.load(std::memory_order_acquire) || skip_hook_process() || !name) {
    return real(name);
  }
  if (std::strcmp(name, "WEMEET_XWAYLAND") == 0) {
    static char value[] = "1";
    return value;
  }
  if (std::strcmp(name, "XDG_SESSION_TYPE") == 0) {
    static char value[] = "x11";
    return value;
  }
  if (std::strcmp(name, "WAYLAND_DISPLAY") == 0) {
    return nullptr;
  }
  return real(name);
}

// Snapshot Wayland display for the portal, then hide it from wemeet so
// 快速会议 does not SIGSEGV in InitWaylandScreenHelper.
__attribute__((constructor))
static void wemeet_hook_ctor() {
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (n > 0) {
    exe[n] = '\0';
  }
  if (skip_hook_process()) {
    return;
  }
  if (const char* wd = std::getenv("WAYLAND_DISPLAY")) {
    if (wd[0]) {
      setenv("WEMEET_HOOK_WAYLAND_DISPLAY", wd, 1);
    }
  }
  unsetenv("WAYLAND_DISPLAY");
  setenv("WEMEET_XWAYLAND", "1", 1);
  setenv("XDG_SESSION_TYPE", "x11", 1);
  g_lock_client_env.store(true, std::memory_order_release);
  hook_log("[chain] x11-capture + gnome-portal revision=capture-v3 pid=%d wayland_saved=%s",
           getpid(),
           std::getenv("WEMEET_HOOK_WAYLAND_DISPLAY") ? std::getenv("WEMEET_HOOK_WAYLAND_DISPLAY") : "(none)");
  try_patch_xcast();
  pthread_t tid;
  pthread_create(&tid, nullptr, patcher_thread_main, nullptr);
  pthread_detach(tid);
}

static std::mutex g_capture_mu;
static Display* g_capture_display = nullptr;
static std::set<Drawable> g_capture_drawables;
using ShmKey = std::pair<Display*, XShmSegmentInfo*>;
using DamageKey = std::pair<Display*, Damage>;
static std::set<ShmKey> g_screen_shm;
static std::set<ShmKey> g_attached_shm;
static std::map<DamageKey, Drawable> g_damage_drawables;
// Destroy the active session before these maps/mutex at process exit.
static std::shared_ptr<CaptureSession> g_capture;

static bool looks_like_screen_grab(unsigned width, unsigned height) {
  return width >= 640 && height >= 360;
}

static void stop_capture(const char* reason, const std::shared_ptr<CaptureSession>& expected = {}) {
  std::shared_ptr<CaptureSession> previous;
  {
    std::lock_guard<std::mutex> lock(g_capture_mu);
    if (expected && g_capture != expected) return;
    previous = std::move(g_capture);
    g_capture_display = nullptr;
    g_capture_drawables.clear();
    g_attached_shm.clear();
  }
  if (previous) {
    hook_log("[portal] stopping capture: %s", reason);
    previous->stop();
  }
  // Join/destruction happens outside the manager lock. Frame readers may
  // still hold an owner; the worker stops immediately either way.
}

static void reset_finished_capture() {
  std::shared_ptr<CaptureSession> previous;
  {
    std::lock_guard<std::mutex> lock(g_capture_mu);
    if (g_capture && g_capture->finished()) {
      previous = std::move(g_capture);
      g_capture_display = nullptr;
      g_capture_drawables.clear();
      g_attached_shm.clear();
    }
  }
}

static std::shared_ptr<CaptureSession> ensure_portal(Display* display, Drawable drawable = None) {
  if (skip_hook_process()) return {};
  std::shared_ptr<CaptureSession> capture;
  {
    std::lock_guard<std::mutex> lock(g_capture_mu);
    if (g_capture && g_capture_display != display) return {};
    if (!g_capture) {
      hook_log("[portal] starting xdg-desktop-portal screencast");
      g_capture = std::make_shared<CaptureSession>();
      g_capture_display = display;
      g_capture->start();
    }
    g_capture_drawables.insert(drawable != None ? drawable : DefaultRootWindow(display));
    capture = g_capture;
  }
  return capture->wait_until_started() ? capture : nullptr;
}

template <typename T>
struct remove_pointer_cvref {
  using type = std::remove_cv_t<std::remove_pointer_t<std::remove_reference_t<T>>>;
};

template <typename T>
using remove_pointer_cvref_t = typename remove_pointer_cvref<T>::type;


// returns: ximage_width_offset, ximage_height_offset, target_width, target_height
std::tuple<uint32_t, uint32_t, uint32_t, uint32_t> get_resize_param(
  uint32_t ximage_width,
  uint32_t ximage_height,
  uint32_t framebuffer_width,
  uint32_t framebuffer_height
){
  // keep the framebuffer aspect ratio
  double framebuffer_aspect_ratio = static_cast<double>(framebuffer_width) / static_cast<double>(framebuffer_height);
  double ximage_aspect_ratio = static_cast<double>(ximage_width) / static_cast<double>(ximage_height);

  uint32_t target_width = 0;
  uint32_t target_height = 0;
  uint32_t ximage_width_offset = 0;
  uint32_t ximage_height_offset = 0;

  if (framebuffer_aspect_ratio > ximage_aspect_ratio) {
    // framebuffer is wider than ximage
    target_width = ximage_width;
    target_height = (ximage_width * framebuffer_height) / framebuffer_width;
    ximage_height_offset = (ximage_height - target_height) / 2;
  } else {
    // framebuffer is taller than ximage
    target_height = ximage_height;
    target_width = ximage_height * framebuffer_width / framebuffer_height;
    ximage_width_offset = (ximage_width - target_width) / 2;
  }

  return std::make_tuple(ximage_width_offset, ximage_height_offset, target_width, target_height);
}


bool XShmGetImageHook(XImage& image, const std::shared_ptr<CaptureSession>& capture) {
  if (!capture) return false;
  auto& interface = capture->interface;
  std::lock_guard<std::mutex> frame_lock(interface.frame_mutex);
  if (!interface.frames_ready.load() || !image.data || image.bits_per_pixel != 32) return false;

  auto ximage_spa_format = ximage_to_spa(image);
  if (ximage_spa_format == SpaVideoFormat_e::INVALID || image.width <= 0 || image.height <= 0 ||
      image.bytes_per_line < image.width * 4) return false;
  auto ximage_width = image.width;
  auto ximage_height = image.height;
  size_t ximage_bytes_per_line = image.bytes_per_line;

  auto& framebuffer = interface.framebuf;
  auto framebuffer_spa_format = framebuffer.format;
  uint32_t framebuffer_width = framebuffer.width;
  uint32_t framebuffer_height = framebuffer.height;
  uint32_t framebuffer_row_byte_stride = framebuffer.row_byte_stride;
  uint8_t* src = framebuffer.data.get();
  if (!src || framebuffer_width == 0 || framebuffer_height == 0) {
    return false;
  }
  if (framebuffer.crop_height > 0 && framebuffer.crop_width > 0 &&
      framebuffer.crop_x >= 0 && framebuffer.crop_y >= 0 &&
      static_cast<uint64_t>(framebuffer.crop_x) + framebuffer.crop_width <= framebuffer_width &&
      static_cast<uint64_t>(framebuffer.crop_y) + framebuffer.crop_height <= framebuffer_height) {
    src += static_cast<size_t>(framebuffer.crop_y) * framebuffer_row_byte_stride
         + static_cast<size_t>(framebuffer.crop_x) * 4;
    framebuffer_width = framebuffer.crop_width;
    framebuffer_height = framebuffer.crop_height;
  }

  auto [ximage_width_offset, ximage_height_offset, target_width, target_height] = get_resize_param(
    ximage_width, ximage_height, framebuffer_width, framebuffer_height
  );
  if (target_width == 0 || target_height == 0 || !image.data) {
    return false;
  }

  std::memset(image.data, 0, static_cast<size_t>(ximage_height) * ximage_bytes_per_line);
  uint8_t* dst = reinterpret_cast<uint8_t*>(image.data)
               + static_cast<size_t>(ximage_height_offset) * ximage_bytes_per_line
               + static_cast<size_t>(ximage_width_offset) * 4;

  if (!stbir_resize_uint8_linear(
    src, static_cast<int>(framebuffer_width), static_cast<int>(framebuffer_height),
    static_cast<int>(framebuffer_row_byte_stride),
    dst, static_cast<int>(target_width), static_cast<int>(target_height),
    static_cast<int>(ximage_bytes_per_line),
    STBIR_4CHANNEL
  )) return false;

  if (needs_rb_swap(framebuffer_spa_format, ximage_spa_format)) {
    for (uint32_t row = 0; row < target_height; ++row) {
      uint8_t* p = dst + static_cast<size_t>(row) * ximage_bytes_per_line;
      for (uint32_t col = 0; col < target_width; ++col) {
        std::swap(p[0], p[2]);
        p += 4;
      }
    }
  }
  return true;
}



extern "C" {

Bool XShmAttach(Display* dpy, XShmSegmentInfo* shminfo) {
  const Bool ok = XShmAttachFunc.func ? XShmAttachFunc(dpy, shminfo) : False;
  bool screen_segment;
  {
    std::lock_guard<std::mutex> lock(g_capture_mu);
    screen_segment = g_screen_shm.count({dpy, shminfo}) != 0;
  }
  if (ok && screen_segment && !skip_hook_process()) {
    ensure_portal(dpy);
    std::lock_guard<std::mutex> lock(g_capture_mu);
    if (g_capture && g_capture_display == dpy) g_attached_shm.insert({dpy, shminfo});
  }
  return ok;
}

Bool XShmGetImage(Display* dpy, Drawable drawable, XImage* image, int x, int y, unsigned long mask) {
  if (image && looks_like_screen_grab(image->width, image->height) && !skip_hook_process())
    return XShmGetImageHook(*image, ensure_portal(dpy, drawable)) ? True : False;
  return XShmGetImageFunc.func ? XShmGetImageFunc(dpy, drawable, image, x, y, mask) : False;
}

Bool XShmDetach(Display* dpy, XShmSegmentInfo* shminfo) {
  std::shared_ptr<CaptureSession> stopping;
  {
    std::lock_guard<std::mutex> lock(g_capture_mu);
    g_screen_shm.erase({dpy, shminfo});
    const bool tracked = g_attached_shm.erase({dpy, shminfo}) != 0;
    if (tracked && g_attached_shm.empty() && g_capture_display == dpy) stopping = g_capture;
  }
  if (stopping) stop_capture("last screen XShm segment detached", stopping);
  return XShmDetachFunc.func ? XShmDetachFunc(dpy, shminfo) : False;
}

XImage* XGetImage(Display* dpy, Drawable drawable, int x, int y, unsigned width, unsigned height, unsigned long mask, int format) {
  if (format == ZPixmap && looks_like_screen_grab(width, height) && !skip_hook_process()) {
    auto capture = ensure_portal(dpy, drawable);
    if (!capture) return nullptr;
    Window root;
    int gx, gy;
    unsigned gw, gh, border, depth;
    if (!XGetGeometry(dpy, drawable, &root, &gx, &gy, &gw, &gh, &border, &depth)) return nullptr;
    // Xwayland's root is invisible and native XGetImage can raise BadMatch.
    // Build an ordinary XImage only after a real portal frame is available.
    XImage* image = XCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), depth,
                                ZPixmap, 0, nullptr, width, height, 32, 0);
    if (!image) return nullptr;
    image->data = static_cast<char*>(calloc(height, image->bytes_per_line));
    if (!image->data || !XShmGetImageHook(*image, capture)) {
      XDestroyImage(image);
      return nullptr;
    }
    return image;
  }
  return XGetImageFunc.func ? XGetImageFunc(dpy, drawable, x, y, width, height, mask, format) : nullptr;
}

Bool XDamageQueryExtension(Display* dpy, int* event, int* error) {
  const Bool ok = XDamageQueryExtensionFunc.func ? XDamageQueryExtensionFunc(dpy, event, error) : False;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) hook_log("[x11] XDamageQueryExtension real=%p result=%d", reinterpret_cast<void*>(XDamageQueryExtensionFunc.func), ok);
  return ok;
}

int XCopyArea(Display* dpy, Drawable src, Drawable dest, GC gc, int x, int y, unsigned width, unsigned height, int dx, int dy) {
  if (looks_like_screen_grab(width, height)) ensure_portal(dpy, src);
  return XCopyAreaFunc.func ? XCopyAreaFunc(dpy, src, dest, gc, x, y, width, height, dx, dy) : 0;
}

Bool XShmQueryVersion(Display* dpy, int* major, int* minor, Bool* pixmaps) {
  const Bool ok = XShmQueryVersionFunc.func ? XShmQueryVersionFunc(dpy, major, minor, pixmaps) : False;
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) hook_log("[x11] XShmQueryVersion real=%p result=%d", reinterpret_cast<void*>(XShmQueryVersionFunc.func), ok);
  return ok;
}

XImage* XShmCreateImage(Display* dpy, Visual* visual, unsigned depth, int format, char* data, XShmSegmentInfo* shminfo, unsigned width, unsigned height) {
  XImage* image = XShmCreateImageFunc.func ? XShmCreateImageFunc(dpy, visual, depth, format, data, shminfo, width, height) : nullptr;
  if (image && looks_like_screen_grab(width, height) && !skip_hook_process()) {
    reset_finished_capture();
    std::lock_guard<std::mutex> lock(g_capture_mu);
    g_screen_shm.insert({dpy, shminfo});
  }
  return image;
}

Pixmap XShmCreatePixmap(Display* dpy, Drawable drawable, char* data, XShmSegmentInfo* shminfo, unsigned width, unsigned height, unsigned depth) {
  Pixmap pixmap = XShmCreatePixmapFunc.func ? XShmCreatePixmapFunc(dpy, drawable, data, shminfo, width, height, depth) : None;
  if (pixmap && looks_like_screen_grab(width, height) && !skip_hook_process()) {
    reset_finished_capture();
    {
      std::lock_guard<std::mutex> lock(g_capture_mu);
      g_screen_shm.insert({dpy, shminfo});
      g_attached_shm.insert({dpy, shminfo});
    }
    ensure_portal(dpy, drawable);
  }
  return pixmap;
}

Damage XDamageCreate(Display* dpy, Drawable drawable, int level) {
  Damage damage = XDamageCreateFunc.func ? XDamageCreateFunc(dpy, drawable, level) : None;
  if (damage && !skip_hook_process()) {
    reset_finished_capture();
    std::lock_guard<std::mutex> lock(g_capture_mu);
    g_damage_drawables[{dpy, damage}] = drawable;
  }
  return damage;
}

void XDamageDestroy(Display* dpy, Damage damage) {
  std::shared_ptr<CaptureSession> stopping;
  {
    std::lock_guard<std::mutex> lock(g_capture_mu);
    auto it = g_damage_drawables.find({dpy, damage});
    if (it != g_damage_drawables.end()) {
      if (g_capture && g_capture_display == dpy && g_capture_drawables.count(it->second)) stopping = g_capture;
      g_damage_drawables.erase(it);
    }
  }
  if (stopping) stop_capture("screen XDamage object destroyed", stopping);
  if (XDamageDestroyFunc.func) XDamageDestroyFunc(dpy, damage);
}

int XCloseDisplay(Display* dpy) {
  std::shared_ptr<CaptureSession> stopping;
  {
    std::lock_guard<std::mutex> lock(g_capture_mu);
    if (g_capture && g_capture_display == dpy) stopping = g_capture;
    for (auto it = g_damage_drawables.begin(); it != g_damage_drawables.end(); )
      if (it->first.first == dpy) it = g_damage_drawables.erase(it); else ++it;
    for (auto it = g_screen_shm.begin(); it != g_screen_shm.end(); )
      if (it->first == dpy) it = g_screen_shm.erase(it); else ++it;
  }
  if (stopping) stop_capture("capture X11 display closed", stopping);
  return XCloseDisplayFunc.func ? XCloseDisplayFunc(dpy) : 0;
}

void XCompositeRedirectWindow(Display* dpy, Window window, int update) {
  if (XCompositeRedirectWindowFunc.func) XCompositeRedirectWindowFunc(dpy, window, update);
}

}
