

#include <thread>
#include <vector>
#include <algorithm>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include "payload.hpp"
#include "interface.hpp"

#include "helpers.hpp"

using XWindow_t = Window;


struct CandidateWindowInfo{
  XWindow_t window_id;
  std::string window_name;
  int window_width;
  int window_height;
};


std::vector<CandidateWindowInfo> x11_sanitizer_get_targets(
  Display* display,
  XWindow_t root_window
){
  // hunt for direct children of root window that has override_redirect set to true
  Window root_return, parent_return;
  Window* children_return;
  unsigned int nchildren_return;
  auto xquery_status = XQueryTree(display, root_window, &root_return, &parent_return, &children_return, &nchildren_return);
  if (xquery_status == 0) {
    fprintf(stderr, "%s", red_text("[x11_sanitizer] XQueryTree failed. \n").c_str());
    return {};
  }
  std::vector<CandidateWindowInfo> targets;
  for (int i = 0; i < nchildren_return; i++) {
    XWindow_t cur_window = children_return[i];
    XWindowAttributes window_attributes;
    auto xgetwindow_status = XGetWindowAttributes(display, cur_window, &window_attributes);
    if (xgetwindow_status == 0) {
      fprintf(stderr, "%s", red_text("[x11_sanitizer] XGetWindowAttributes failed. \n").c_str());
      continue;
    }
    if (window_attributes.override_redirect == false) {
      continue;
    }

    // found override_redirect window
    // check its name

    std::string window_name;
    XTextProperty prop;
    Atom wmNameAtom = XInternAtom(display, "_NET_WM_NAME", True);
    if (wmNameAtom && XGetTextProperty(display, cur_window, &prop, wmNameAtom) && prop.value) {
        window_name = std::string(reinterpret_cast<char*>(prop.value));
        XFree(prop.value);
    }

    // check if "wemeet" is in the window name
    if (window_name.find("wemeet") == std::string::npos) {
      continue;
    }


    // found candidate window here, add it to the list

    targets.push_back(CandidateWindowInfo{
      .window_id = cur_window,
      .window_name = window_name,
      .window_width = window_attributes.width,
      .window_height = window_attributes.height
    });
  }
  XFree(children_return);
  return targets;
}


std::vector<std::tuple<int, int>> get_screen_sizes(
  Display* display,
  int screen
){
  std::vector<std::tuple<int, int>> screen_sizes;
  XRRScreenResources *screen_resources = XRRGetScreenResources(display, RootWindow(display, screen));
  if (!screen_resources) return screen_sizes;
  for (int i = 0; i < screen_resources->noutput; i++) {
    RROutput output = screen_resources->outputs[i];
    XRROutputInfo *output_info = XRRGetOutputInfo(display, screen_resources, output);
    if (!output_info) continue;
    if (output_info->connection == RR_Connected) {
      XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(display, screen_resources, output_info->crtc);
      if (crtc_info) {
        screen_sizes.push_back(std::make_tuple(crtc_info->width, crtc_info->height));
        XRRFreeCrtcInfo(crtc_info);
      }
    }
    XRRFreeOutputInfo(output_info);
  }
  XRRFreeScreenResources(screen_resources);
  return screen_sizes;
}

/*
  Below is a *VERY CRAZY* code that get rid of the stupid asshole wemeet screenshare overlay window, which
  prevents the user from clicking anything on the screen.

  we accomplish this by:

  1. First locate the right window to get rid of. This can be very tricky, since wemeet would
  create and destroy multiple windows with override_redirect set to true after you started screenshare.
  If the wrong window is picked, the whole wemeet app GUI would enter a very weird state and will eventually
  crash, LMFAO. However I observed that it would eventually enter a "steady state" where the right window
  would be the one with the largest size. DerryAlex noticed this size should also be the "fullscreen size",
  though with some caveats. He also digged out that the unified strategy XUnmapWindow works well.
  
  So the eventual logic is, well, a little bit over engineered. In every 100ms, we:
    (1) first locate some candidate windows that has "wemeet" in its name and has override_redirect set to true.
    (2) then see if there is any candidate window whose size is "roughly the same" as the screen size.
    (3) if there is one, we just select it and then exit.
    (4) if there isn't one, the loop continues
  This loop will continue until $STEADY_WAIT_TIME has passed. If things turn out like this we use the fallback
  strategy that picks the window with the largest size.

  2. Just hide the window, using the unified strategy "XUnmapWindow" regardless of the DE type.
    However we keep the DE detection logic here in case we need to use different strategies for different DEs...
*/

void x11_sanitizer_main(Interface* interface_handle)
{
  // get the current session type
  // if it's "wayland", then the x11 sanitizer can just exit
  if (get_current_session_type() == SessionType::Wayland) {
    fprintf(stderr, "%s", green_text("[x11_sanitizer] wayland session detected. skipping x11 sanitizer. \n").c_str());
    return;
  }
  
  Display* display = XOpenDisplay(NULL);
  if (!display) return;
  int screen = DefaultScreen(display);
  XWindow_t root_window = DefaultRootWindow(display);

  std::vector<std::tuple<int, int>> screen_sizes = get_screen_sizes(display, screen);
    
  // 2 seconds steady wait time seems to be "generally safe".
  std::chrono::milliseconds STEADY_WAIT_TIME(2000);
  std::chrono::time_point<std::chrono::high_resolution_clock> steady_start_time;
  bool target_first_occurred = false;
  bool target_managed = false;
  XWindow_t picked_window_id = 0;

  while(interface_handle->x11_sanitizer_stop_flag.load(std::memory_order_seq_cst) == false){

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto targets = x11_sanitizer_get_targets(display, root_window);
    if (targets.empty()) continue;
    auto now = std::chrono::high_resolution_clock::now();
    if (targets.size() != 0 && target_first_occurred == false) {
      target_first_occurred = true;
      steady_start_time = now;
    }
    
    // This can be much faster than STEADY_WAIT_TIME, say 200ms
    for (auto target: targets) {

      auto f_fuzzy_matched = [&](){
        for (auto [width, height]: screen_sizes) {
          bool matched = std::abs(target.window_width - width) < 5 && std::abs(target.window_height - height) < 5;
          if (matched) {
            return true;
          }
        }
        return false;
      };

      if (f_fuzzy_matched()) {
        auto duration = std::chrono::duration_cast<std::chrono::duration<long, std::milli>>(now - steady_start_time);
        picked_window_id = target.window_id;
        fprintf(stderr, "%s picked window 0x%lx after %ld ms.\n", green_text("[payload x11 sanitizer]").c_str(), picked_window_id, duration.count());
        now = now + STEADY_WAIT_TIME; // this cheats the following logic to skip the steady wait
        break;
      }
    }
    
    if (target_first_occurred && (now - steady_start_time) < STEADY_WAIT_TIME) {
      // wait for steady state
      continue;
    }

    if (target_first_occurred && (now - steady_start_time) >= STEADY_WAIT_TIME) {
      
      if (!picked_window_id) {
        fprintf(stderr, "%s", yellow_text("[payload x11 sanitizer] steady wait done. \n").c_str());
        auto comparator = [](const CandidateWindowInfo& a, const CandidateWindowInfo& b) {
          int64_t area_a = static_cast<int64_t>(a.window_width) * a.window_height;
          int64_t area_b = static_cast<int64_t>(b.window_width) * b.window_height;
          return area_a > area_b;
        };

        std::sort(targets.begin(), targets.end(), comparator);
        picked_window_id = targets[0].window_id;
        fprintf(stderr, "%s", yellow_text("[payload x11 sanitizer] picked window: 0x" + int_to_hexstr(picked_window_id) + "\n").c_str());
      }
      
      if (!target_managed) {
        DEType de_type = get_current_de_type();
        switch (de_type) {
          case DEType::GNOME:
          case DEType::KDE:
          case DEType::Unknown:
            // this unified strategy works well for most DEs
            XUnmapWindow(display, picked_window_id);
            break;
          case DEType::Hyprland:
            // FIXME: hyprland may need special workarounds
            XUnmapWindow(display, picked_window_id);
            break;
        }
        target_managed = true;
        break;
      }
    }  
  }
  XCloseDisplay(display);
}




CaptureSession::CaptureSession() = default;

CaptureSession::~CaptureSession() {
  stop();
  if (worker_.joinable()) worker_.join();
}

void CaptureSession::start() {
  worker_ = std::thread(&CaptureSession::run, this);
}

void CaptureSession::stop() {
  stop_requested_.store(true);
  interface.x11_sanitizer_stop_flag.store(true);
  state_changed_.notify_all();
}

void CaptureSession::publish(CaptureStatus state) {
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.store(state);
  }
  state_changed_.notify_all();
}

bool CaptureSession::wait_until_started() {
  std::unique_lock<std::mutex> lock(state_mutex_);
  if (!state_changed_.wait_for(lock, std::chrono::seconds(125), [this] {
        return status_.load() != CaptureStatus::Starting || stop_requested_.load();
      })) {
    hook_log("[portal] capture caller timed out");
    lock.unlock();
    stop();
    return false;
  }
  return !stop_requested_.load() && status_.load() == CaptureStatus::Running;
}

void CaptureSession::run() {
  // One worker owns all portal and PipeWire operations. Cancellation never
  // waits for another worker to publish an object or acknowledge a stop.
  GMainContext* context = g_main_context_new();
  g_main_context_push_thread_default(context);
  auto portal = std::make_shared<XdpScreencastPortal>();
  std::unique_ptr<PipewireScreenCast> pipewire;
  std::thread sanitizer;
  auto started = std::chrono::steady_clock::now();
  auto connected = started;
  try {
    portal->arm();
    while (!stop_requested_.load()) {
      for (int i = 0; i < 32 && g_main_context_pending(context); ++i)
        g_main_context_iteration(context, FALSE);
      const auto now = std::chrono::steady_clock::now();
      const auto portal_status = portal->status.load();
      if (portal_status == XdpScreencastPortalStatus::kCancelled) {
        publish(CaptureStatus::Cancelled);
        break;
      }
      if (!pipewire && portal_status == XdpScreencastPortalStatus::kRunning) {
        const int fd = portal->pipewire_fd.exchange(-1);
        pipewire = std::make_unique<PipewireScreenCast>(fd, portal->pipewire_node_ids.at(0), &interface);
        if (!pipewire->valid()) throw std::runtime_error("PipeWire context/registry unavailable");
        connected = now;
        sanitizer = std::thread(x11_sanitizer_main, &interface);
      }
      if (pipewire) {
        int result = pw_loop_iterate(pw_main_loop_get_loop(pipewire->pw_mainloop), 0);
        if (result < 0 || pipewire->failed.load()) throw std::runtime_error("PipeWire stream failed");
        if (interface.frames_ready.load() && status_.load() == CaptureStatus::Starting)
          publish(CaptureStatus::Running);
        if (!interface.frames_ready.load() && now - connected > std::chrono::seconds(15))
          throw std::runtime_error("timed out waiting for first PipeWire frame");
      } else if ((!portal->session.load() && now - started > std::chrono::seconds(10)) ||
                 now - started > std::chrono::seconds(120)) {
        throw std::runtime_error("timed out creating session or waiting for picker");
      }
      std::unique_lock<std::mutex> lock(state_mutex_);
      state_changed_.wait_for(lock, std::chrono::milliseconds(16), [this] { return stop_requested_.load(); });
    }
  } catch (const std::exception& error) {
    hook_log("[portal] capture failed: %s", error.what());
    publish(CaptureStatus::Cancelled);
  }

  interface.frames_ready.store(false);
  interface.x11_sanitizer_stop_flag.store(true);
  if (sanitizer.joinable()) sanitizer.join();
  pipewire.reset();
  portal->cancel();
  // Drain cancelled async completions before dropping the private context.
  // Callback owners keep the portal alive even if a broken service never
  // replies; it cannot access the destroyed capture frame/session.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (portal->pending_requests.load() && std::chrono::steady_clock::now() < deadline) {
    for (int i = 0; i < 32 && g_main_context_pending(context); ++i)
      g_main_context_iteration(context, FALSE);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (portal->pending_requests.load()) hook_log("[portal] cancelled callback still pending");
  portal.reset();
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
  if (status_.load() != CaptureStatus::Cancelled) publish(CaptureStatus::Stopped);
  finished_.store(true);
  state_changed_.notify_all();
  hook_log("[portal] session cleaned up status=%s", status_.load() == CaptureStatus::Cancelled ? "cancelled" : "stopped");
}
