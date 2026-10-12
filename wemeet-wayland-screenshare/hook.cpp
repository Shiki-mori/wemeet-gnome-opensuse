
#define _GNU_SOURCE
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
void XCompositeRedirectWindow(Display* dpy, Window window, int update);
void* dlopen(const char* file, int flags);
char* getenv(const char* name);
}

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"

static void hook_log(const char* fmt, ...) {
  FILE* f = fopen("/tmp/wemeet-hook.log", "a");
  if (!f) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fflush(f);
  fclose(f);
}

static std::recursive_mutex g_patch_mu;
static std::atomic<bool> g_xcast_patched{false};
static XGetImageType* plt_real_XGetImage = nullptr;
static XShmAttachType* plt_real_XShmAttach = nullptr;
static XShmGetImageType* plt_real_XShmGetImage = nullptr;
static XShmDetachType* plt_real_XShmDetach = nullptr;
static XCopyAreaType* plt_real_XCopyArea = nullptr;
static XShmQueryVersionType* plt_real_XShmQueryVersion = nullptr;
static XShmCreateImageType* plt_real_XShmCreateImage = nullptr;
static XShmCreatePixmapType* plt_real_XShmCreatePixmap = nullptr;
static XCompositeRedirectWindowType* plt_real_XCompositeRedirectWindow = nullptr;

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

static void remember_old(const char* name, void* oldfn, void* ourfn) {
  if (!oldfn || oldfn == ourfn) {
    return;
  }
  if (std::strcmp(name, "XGetImage") == 0) {
    plt_real_XGetImage = reinterpret_cast<XGetImageType*>(oldfn);
  } else if (std::strcmp(name, "XShmAttach") == 0) {
    plt_real_XShmAttach = reinterpret_cast<XShmAttachType*>(oldfn);
  } else if (std::strcmp(name, "XShmGetImage") == 0) {
    plt_real_XShmGetImage = reinterpret_cast<XShmGetImageType*>(oldfn);
  } else if (std::strcmp(name, "XShmDetach") == 0) {
    plt_real_XShmDetach = reinterpret_cast<XShmDetachType*>(oldfn);
  } else if (std::strcmp(name, "XCopyArea") == 0) {
    plt_real_XCopyArea = reinterpret_cast<XCopyAreaType*>(oldfn);
  } else if (std::strcmp(name, "XShmQueryVersion") == 0) {
    plt_real_XShmQueryVersion = reinterpret_cast<XShmQueryVersionType*>(oldfn);
  } else if (std::strcmp(name, "XShmCreateImage") == 0) {
    plt_real_XShmCreateImage = reinterpret_cast<XShmCreateImageType*>(oldfn);
  } else if (std::strcmp(name, "XShmCreatePixmap") == 0) {
    plt_real_XShmCreatePixmap = reinterpret_cast<XShmCreatePixmapType*>(oldfn);
  } else if (std::strcmp(name, "XCompositeRedirectWindow") == 0) {
    plt_real_XCompositeRedirectWindow = reinterpret_cast<XCompositeRedirectWindowType*>(oldfn);
  }
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
    {"XCompositeRedirectWindow", reinterpret_cast<void*>(XCompositeRedirectWindow)},
  };
  int ok = 0;
  for (const auto& it : items) {
    void* oldfn = nullptr;
    int r = plthook_replace(plt, it.name, it.fn, &oldfn);
    if (r == PLTHOOK_SUCCESS) {
      ++ok;
      remember_old(it.name, oldfn, it.fn);
      hook_log("[plt] %s replace %s r=%d old=%p", tag, it.name, r, oldfn);
    }
  }
  plthook_close(plt);
  if (ok > 0) {
    g_xcast_patched.store(true);
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

// The interview on 2026-10-08 used wemeet's own ext_portal. That path is
// skipped while this module can see WEMEET_XWAYLAND=1. Hide it only here.
// The rest of the process must still see 1, or 快速会议 SIGSEGVs.
extern "C" char* getenv(const char* name) {
  using Fn = char*(const char*);
  static Fn* real = nullptr;
  if (!real) {
    real = reinterpret_cast<Fn*>(dlsym(RTLD_NEXT, "getenv"));
  }
  if (!real) {
    return nullptr;
  }
  if (skip_hook_process() || !name) {
    return real(name);
  }
  void* caller = __builtin_return_address(0);
  Dl_info info{};
  const bool from_share = dladdr(caller, &info) != 0 && info.dli_fname &&
      (std::strstr(info.dli_fname, "libscreen_share_module.so") != nullptr ||
       std::strstr(info.dli_fname, "libwayland") != nullptr);
  if (!from_share) {
    return real(name);
  }
  if (std::strcmp(name, "WEMEET_XWAYLAND") == 0) {
    static std::atomic<int> logged{0};
    if (logged.exchange(1) == 0) {
      hook_log("[getenv] hide WEMEET_XWAYLAND from screen share");
    }
    return nullptr;
  }
  if (std::strcmp(name, "WAYLAND_DISPLAY") == 0) {
    if (const char* saved = real("WEMEET_HOOK_WAYLAND_DISPLAY")) {
      return const_cast<char*>(saved);
    }
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
  hook_log("[ctor] loaded pid=%d exe=%s wayland_saved=%s display=%s session=%s",
           getpid(),
           n > 0 ? exe : "?",
           std::getenv("WEMEET_HOOK_WAYLAND_DISPLAY") ? std::getenv("WEMEET_HOOK_WAYLAND_DISPLAY") : "(none)",
           std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(none)",
           std::getenv("XDG_SESSION_TYPE") ? std::getenv("XDG_SESSION_TYPE") : "(none)");
  try_patch_xcast();
  pthread_t tid;
  pthread_create(&tid, nullptr, patcher_thread_main, nullptr);
  pthread_detach(tid);
}

constexpr uint32_t DEFAULT_FRAME_HEIGHT = 1080;
constexpr uint32_t DEFAULT_FRAME_WIDTH = 1920;

void XShmAttachHook(){

  auto& interface_singleton = InterfaceSingleton::getSingleton();
  if (interface_singleton.interface_handle.load() != nullptr) {
    return;
  }

  hook_log("[portal] XShmAttachHook begin");

  // initialize interface singleton:
  // (1) allocate the interface object
  interface_singleton.interface_handle = new Interface(
    DEFAULT_FB_ALLOC_HEIGHT, DEFAULT_FB_ALLOC_WIDTH,
    DEFAULT_FRAME_HEIGHT, DEFAULT_FRAME_WIDTH, SpaVideoFormat_e::RGBA
  );
  // (2) allocate the screencast portal object
  interface_singleton.portal_handle = new XdpScreencastPortal();

  // start the payload thread
  std::thread payload_thread = std::thread(payload_main);
  fprintf(stderr, "%s", green_text("[hook] payload thread started\n").c_str());

  while(interface_singleton.portal_handle.load()->status.load(std::memory_order_seq_cst) == XdpScreencastPortalStatus::kInit ) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  };
  auto payload_status = interface_singleton.portal_handle.load()->status.load(std::memory_order_seq_cst);
  std::string payload_status_str = 
    payload_status == XdpScreencastPortalStatus::kCancelled ? "cancelled" :
    payload_status == XdpScreencastPortalStatus::kRunning ? "running" :
    "unknown";
  if (payload_status == XdpScreencastPortalStatus::kRunning) {
    // things are good
    fprintf(stderr, "%s", green_text("[hook] portal status: " + payload_status_str + "\n").c_str());
  } else {
    // things are bad, we have to de-initialize and exit
    fprintf(stderr, "%s", red_text("[hook] portal status: " + payload_status_str + "\n").c_str());
    fprintf(stderr, "%s", red_text("[hook] <<<!!Please DO NOT cancel screencast!!>> Hook is now exiting.\n").c_str());
    // payload thread should have quitted via g_main_loop_quit
    payload_thread.join();
    delete interface_singleton.interface_handle.load();
    delete interface_singleton.portal_handle.load();
    interface_singleton.interface_handle.store(nullptr);
    interface_singleton.portal_handle.store(nullptr);
    return;
  }


  while(interface_singleton.portal_handle.load()->pipewire_fd.load(std::memory_order_seq_cst) == -1){
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  fprintf(stderr, "%s", green_text("[hook SYNC] pipewire_fd acquired: " + std::to_string(interface_singleton.portal_handle.load()->pipewire_fd.load()) + "\n").c_str());

  interface_singleton.pipewire_handle = new PipewireScreenCast(interface_singleton.portal_handle.load()->pipewire_fd.load(), interface_singleton.portal_handle.load()->pipewire_node_ids.at(0));
  fprintf(stderr, "%s", green_text("[hook SYNC] pipewire screencast object allocated\n").c_str());
  
  payload_thread.detach();

}

static bool looks_like_screen_grab(unsigned width, unsigned height) {
  return width >= 640 && height >= 360;
}

static void ensure_portal() {
  auto& interface_singleton = InterfaceSingleton::getSingleton();
  if (interface_singleton.interface_handle.load() == nullptr) {
    hook_log("[portal] starting xdg-desktop-portal screencast");
    XShmAttachHook();
  }
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


void XShmGetImageHook(XImage& image){

  auto& interface_singleton = InterfaceSingleton::getSingleton();

  if (interface_singleton.interface_handle.load() == nullptr){
    return;
  }
  if (!interface_singleton.interface_handle.load()->frames_ready.load()) {
    return;
  }

  auto ximage_spa_format = ximage_to_spa(image);
  auto ximage_width = image.width;
  auto ximage_height = image.height;
  size_t ximage_bytes_per_line = image.bytes_per_line;

  if (image.data) {
    std::memset(image.data, 0, static_cast<size_t>(ximage_height) * ximage_bytes_per_line);
  }

  auto& framebuffer = interface_singleton.interface_handle.load()->framebuf;
  auto framebuffer_spa_format = framebuffer.format;
  uint32_t framebuffer_width = framebuffer.width;
  uint32_t framebuffer_height = framebuffer.height;
  uint32_t framebuffer_row_byte_stride = framebuffer.row_byte_stride;
  uint8_t* src = framebuffer.data.get();
  if (!src || framebuffer_width == 0 || framebuffer_height == 0) {
    return;
  }
  if (framebuffer.crop_height && framebuffer.crop_width) {
    src += static_cast<size_t>(framebuffer.crop_y) * framebuffer_row_byte_stride
         + static_cast<size_t>(framebuffer.crop_x) * 4;
    framebuffer_width = framebuffer.crop_width;
    framebuffer_height = framebuffer.crop_height;
  }

  auto [ximage_width_offset, ximage_height_offset, target_width, target_height] = get_resize_param(
    ximage_width, ximage_height, framebuffer_width, framebuffer_height
  );
  if (target_width == 0 || target_height == 0 || !image.data) {
    return;
  }

  uint8_t* dst = reinterpret_cast<uint8_t*>(image.data)
               + static_cast<size_t>(ximage_height_offset) * ximage_bytes_per_line
               + static_cast<size_t>(ximage_width_offset) * 4;

  stbir_resize_uint8_linear(
    src, static_cast<int>(framebuffer_width), static_cast<int>(framebuffer_height),
    static_cast<int>(framebuffer_row_byte_stride),
    dst, static_cast<int>(target_width), static_cast<int>(target_height),
    static_cast<int>(ximage_bytes_per_line),
    STBIR_4CHANNEL
  );

  if (needs_rb_swap(framebuffer_spa_format, ximage_spa_format)) {
    for (uint32_t row = 0; row < target_height; ++row) {
      uint8_t* p = dst + static_cast<size_t>(row) * ximage_bytes_per_line;
      for (uint32_t col = 0; col < target_width; ++col) {
        std::swap(p[0], p[2]);
        p += 4;
      }
    }
  }
}



void XShmDetachStopPWLoop(){
  auto& interface_singleton = InterfaceSingleton::getSingleton();
  fprintf(stderr, "%s", green_text("[hook] signal pw stop.\n").c_str());
  interface_singleton.interface_handle.load()->pw_stop_flag.store(true, std::memory_order_seq_cst);
  while(!interface_singleton.interface_handle.load()->payload_pw_stop_confirm.load(std::memory_order_seq_cst)){
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  fprintf(stderr, "%s", green_text("[hook SYNC] pw stop confirmed.\n").c_str());
  return;
}

void XShmDetachStopGIOLoop(){
  auto& interface_singleton = InterfaceSingleton::getSingleton();
  fprintf(stderr, "%s", green_text("[hook] stop gio main loop.\n").c_str());
  g_main_loop_quit(interface_singleton.portal_handle.load()->gio_mainloop);
  while(!interface_singleton.interface_handle.load()->payload_gio_stop_confirm.load(std::memory_order_seq_cst)){
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  fprintf(stderr, "%s", green_text("[hook SYNC] gio stop confirmed.\n").c_str());
  return;
}

void XShmDetachHook(){

  auto& interface_singleton = InterfaceSingleton::getSingleton();
  
  // the interface_handle being non nullptr
  // means that the screencast has been started
  if (interface_singleton.interface_handle != nullptr){

    XShmDetachStopPWLoop();
    XShmDetachStopGIOLoop();

    // normal de-initialize interface singleton:
    // (1) free the interface object
    delete interface_singleton.interface_handle.load();
    interface_singleton.interface_handle.store(nullptr);
    // (2) free the screencast portal object
    delete interface_singleton.portal_handle.load();
    interface_singleton.portal_handle.store(nullptr);
    // (3) free the pipewire screencast object
    delete interface_singleton.pipewire_handle.load();
    interface_singleton.pipewire_handle.store(nullptr);
  } else {
    // we do nothing here since the objects (interface, portal) are already freed,
    // and pipewire object has never been created
    fprintf(stderr, "%s", red_text("[hook] objects are already freed because of cancelled screencast. exiting.\n").c_str());
  }
  

}

extern "C" {

Bool XShmAttach(Display* dpy, XShmSegmentInfo* shminfo){
  auto* real = XShmAttachFunc.func.target<XShmAttachType>();
  return real ? real(dpy, shminfo) : False;
}

Bool XShmGetImage(Display* dpy, Drawable d, XImage* image, int x, int y, unsigned long plane_mask){
  auto* real = XShmGetImageFunc.func.target<XShmGetImageType>();
  return real ? real(dpy, d, image, x, y, plane_mask) : False;
}

Bool XShmDetach(Display* dpy, XShmSegmentInfo* shminfo){
  auto* real = XShmDetachFunc.func.target<XShmDetachType>();
  return real ? real(dpy, shminfo) : False;
}

XImage* XGetImage(Display* dpy, Drawable d, int x, int y, unsigned int width, unsigned int height, unsigned long plane_mask, int format){
  auto* real = XGetImageFunc.func.target<XGetImageType>();
  return real ? real(dpy, d, x, y, width, height, plane_mask, format) : nullptr;
}

Bool XDamageQueryExtension(Display *dpy, int *event_base_return, int *error_base_return) {
  auto* real = XDamageQueryExtensionFunc.func.target<XDamageQueryExtensionType>();
  return real ? real(dpy, event_base_return, error_base_return) : False;
}

int XCopyArea(Display* dpy, Drawable src, Drawable dest, GC gc, int src_x, int src_y, unsigned int width, unsigned int height, int dest_x, int dest_y) {
  auto* real = XCopyAreaFunc.func.target<XCopyAreaType>();
  return real ? real(dpy, src, dest, gc, src_x, src_y, width, height, dest_x, dest_y) : 0;
}

Bool XShmQueryVersion(Display* dpy, int* major, int* minor, Bool* pixmaps) {
  auto* real = XShmQueryVersionFunc.func.target<XShmQueryVersionType>();
  return real ? real(dpy, major, minor, pixmaps) : False;
}

XImage* XShmCreateImage(Display* dpy, Visual* visual, unsigned int depth, int format, char* data, XShmSegmentInfo* shminfo, unsigned int width, unsigned int height) {
  auto* real = XShmCreateImageFunc.func.target<XShmCreateImageType>();
  return real ? real(dpy, visual, depth, format, data, shminfo, width, height) : nullptr;
}

Pixmap XShmCreatePixmap(Display* dpy, Drawable d, char* data, XShmSegmentInfo* shminfo, unsigned int width, unsigned int height, unsigned int depth) {
  auto* real = XShmCreatePixmapFunc.func.target<XShmCreatePixmapType>();
  return real ? real(dpy, d, data, shminfo, width, height, depth) : None;
}

Damage XDamageCreate(Display* dpy, Drawable drawable, int level) {
  auto* real = XDamageCreateFunc.func.target<XDamageCreateType>();
  return real ? real(dpy, drawable, level) : 0;
}

void XCompositeRedirectWindow(Display* dpy, Window window, int update) {
  auto* real = XCompositeRedirectWindowFunc.func.target<XCompositeRedirectWindowType>();
  if (real) {
    real(dpy, window, update);
  }
}

}
