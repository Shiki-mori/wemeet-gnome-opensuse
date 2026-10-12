#pragma once

#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <vector>

#include <glib-2.0/glib.h>
#include <glib-2.0/gio/gio.h>
#include <libportal/portal.h>

#include <memory>
#include <pipewire-0.3/pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/pod.h>
#include <spa/utils/dict.h>
#include <spa/buffer/meta.h>

#include "format.hpp"
#include "interface.hpp"

#include "helpers.hpp"
#include "logging.hpp"
#include <cerrno>
#include <cstring>
#include <unistd.h>

enum class DEType {
  GNOME,
  KDE,
  Hyprland, // Hyprland does not support XDP_CURSOR_MODE_HIDDEN, dang it... 
  Unknown // possibly wlr or some other magical DE
};

inline DEType get_current_de_type(){
  // get the DE type using envvar "XDG_SESSION_DESKTOP"
  char* xdg_session_desktop = std::getenv("XDG_SESSION_DESKTOP");
  if (xdg_session_desktop == nullptr) {
    return DEType::Unknown;
  }
  std::string xdg_session_desktop_str = xdg_session_desktop;
  std::string xdg_session_desktop_lower = toLowerString(xdg_session_desktop_str);
  if (std::string(xdg_session_desktop) == "KDE") {
    return DEType::KDE;
  } else if (std::string(xdg_session_desktop) == "gnome") {
    return DEType::GNOME;
  } else if (xdg_session_desktop_lower == "hyprland") {
    return DEType::Hyprland;
  }
  return DEType::Unknown;
}

enum class SessionType {
  Wayland,
  X11,
  Unknown // heck, unless you did not set the envar properly...
};

inline SessionType get_current_session_type(){
  // get the current session type using envvar "XDG_SESSION_TYPE"
  char* xdg_session_type = std::getenv("XDG_SESSION_TYPE");
  if (xdg_session_type == nullptr) {
    return SessionType::Unknown;
  }
  if (std::string(xdg_session_type) == "wayland") {
    return SessionType::Wayland;
  } else if (std::string(xdg_session_type) == "x11") {
    return SessionType::X11;
  }
  return SessionType::Unknown;
}

enum class XdpScreencastPortalStatus {
  kInit,
  kRunning,
  kCancelled,
};

struct XdpScreencastPortal : std::enable_shared_from_this<XdpScreencastPortal> {
  using THIS_CLASS = XdpScreencastPortal;
  using CallbackOwner = std::shared_ptr<THIS_CLASS>;

  // The worker has already pushed its private thread-default context.
  // Portal screen capture uses D-Bus; do not restore WAYLAND_DISPLAY in
  // the client's process-wide environment.
  void arm() {
    cancellable = g_cancellable_new();
    g_autoptr(GError) error = nullptr;
    portal = xdp_portal_initable_new(&error);
    if (!portal) {
      hook_log("[portal] D-Bus unavailable: %s", error ? error->message : "unknown");
      status.store(XdpScreencastPortalStatus::kCancelled);
      return;
    }
    pending_requests.fetch_add(1);
    xdp_portal_create_screencast_session(
      portal, static_cast<XdpOutputType>(XDP_OUTPUT_MONITOR | XDP_OUTPUT_WINDOW),
      XDP_SCREENCAST_FLAG_NONE, XDP_CURSOR_MODE_EMBEDDED,
      XDP_PERSIST_MODE_NONE, nullptr, cancellable,
      screencast_session_create_cb, new CallbackOwner(shared_from_this()));
  }

  void cancel() {
    if (cancellable) g_cancellable_cancel(cancellable);
    if (session.load()) xdp_session_close(session.load());
  }

  ~XdpScreencastPortal() {
    if (session.load()) {
      if (closed_handler) g_signal_handler_disconnect(session.load(), closed_handler);
      xdp_session_close(session.load());
      g_object_unref(session.load());
    }
    const int fd = pipewire_fd.exchange(-1);
    if (fd >= 0) close(fd);
    if (cancellable) g_object_unref(cancellable);
    if (portal) g_object_unref(portal);
  }

  XdpPortal* portal{nullptr};
  GCancellable* cancellable{nullptr};
  gulong closed_handler{0};
  std::atomic<XdpSession*> session{nullptr};
  std::atomic<int> pipewire_fd{-1};
  std::atomic<unsigned> pending_requests{0};
  std::atomic<XdpScreencastPortalStatus> status{XdpScreencastPortalStatus::kInit};
  std::vector<unsigned> pipewire_node_ids{};

  static void screencast_session_create_cb(GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<CallbackOwner> owner(static_cast<CallbackOwner*>(data));
    auto self = *owner;
    self->pending_requests.fetch_sub(1);
    g_autoptr(GError) error = nullptr;
    self->session = xdp_portal_create_screencast_session_finish(XDP_PORTAL(source), result, &error);
    if (!self->session.load() || g_cancellable_is_cancelled(self->cancellable)) {
      hook_log("[portal] create session failed/cancelled: %s", error ? error->message : "cancelled");
      self->status.store(XdpScreencastPortalStatus::kCancelled);
      return;
    }
    self->closed_handler = g_signal_connect(self->session.load(), "closed",
      G_CALLBACK(+[](XdpSession*, gpointer data) {
        auto* portal = static_cast<THIS_CLASS*>(data);
        hook_log("[portal] session closed externally");
        portal->status.store(XdpScreencastPortalStatus::kCancelled);
      }), self.get());
    hook_log("[portal] session created, starting picker");
    self->pending_requests.fetch_add(1);
    xdp_session_start(self->session.load(), nullptr, self->cancellable,
                      screencast_session_start_cb, new CallbackOwner(self));
  }

  static void screencast_session_start_cb(GObject* source, GAsyncResult* result, gpointer data) {
    std::unique_ptr<CallbackOwner> owner(static_cast<CallbackOwner*>(data));
    auto self = *owner;
    self->pending_requests.fetch_sub(1);
    g_autoptr(GError) error = nullptr;
    if (!xdp_session_start_finish(XDP_SESSION(source), result, &error) ||
        g_cancellable_is_cancelled(self->cancellable)) {
      hook_log("[portal] start failed/cancelled: %s", error ? error->message : "cancelled");
      self->status.store(XdpScreencastPortalStatus::kCancelled);
      return;
    }
    GVariant* streams = xdp_session_get_streams(XDP_SESSION(source));
    if (streams) {
      GVariantIter iter;
      g_variant_iter_init(&iter, streams);
      unsigned node;
      GVariant* properties;
      while (g_variant_iter_next(&iter, "(u@a{sv})", &node, &properties)) {
        self->pipewire_node_ids.push_back(node);
        g_variant_unref(properties);
      }
    }
    hook_log("[portal] picker finished, streams=%zu", self->pipewire_node_ids.size());
    if (self->pipewire_node_ids.empty()) {
      self->status.store(XdpScreencastPortalStatus::kCancelled);
      return;
    }
    const int fd = xdp_session_open_pipewire_remote(XDP_SESSION(source));
    if (fd < 0) {
      hook_log("[portal] open PipeWire remote failed");
      self->status.store(XdpScreencastPortalStatus::kCancelled);
      return;
    }
    // Publish only after nodes and the descriptor have both been validated.
    self->pipewire_fd.store(fd);
    self->status.store(XdpScreencastPortalStatus::kRunning);
  }
};

struct PipewireScreenCast {
  using THIS_CLASS = PipewireScreenCast;

  PipewireScreenCast(int pw_fd, int pw_node_id, Interface* output, double target_framerate = 20.0, uint64_t reporting_interval = 20):
    output(output),
    node_id(pw_node_id),
    target_framerate(target_framerate),
    reporting_interval(reporting_interval),
    processed_frame_count(0)
  {
    reset_last_frame_time();
    static std::once_flag initialized;
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
    pw_mainloop = pw_main_loop_new(nullptr);
    if (!pw_mainloop.load()) { close(pw_fd); return; }
    pw_loop* pw_mainloop_loop = pw_main_loop_get_loop(pw_mainloop);
    context = pw_context_new(pw_mainloop_loop, nullptr, 0);
    if (!context.load()) { close(pw_fd); return; }
    core = pw_context_connect_fd(context, pw_fd, nullptr, 0);
    if (!core.load()) return;
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

    if (!registry) return;
    spa_zero(registry_listener);
    pw_registry_add_listener(registry, &registry_listener, &registry_events, this);
  }

  void init(const char *serial) {
    pw_properties *props = pw_properties_new(PW_KEY_TARGET_OBJECT, serial, NULL);
    stream = pw_stream_new(core, "pipewire-portal-screencast", props);

    if (!stream.load()) { failed.store(true); return; }
    stream_events = {
      .version = PW_VERSION_STREAM_EVENTS,
      .state_changed = THIS_CLASS::on_stream_state_changed,
      .param_changed = THIS_CLASS::on_param_changed,
      .process = THIS_CLASS::on_process,
    };
    pw_stream_add_listener(stream, &listener, &stream_events, this);

    // set up stream params
    this->param_buffer.reset(new uint8_t[param_buffer_size]);
    b = SPA_POD_BUILDER_INIT(param_buffer.get(), param_buffer_size);
    
    auto vidsize_default = SPA_RECTANGLE(320, 240);
    auto vidsize_min = SPA_RECTANGLE(1, 1);
    auto vidsize_max = SPA_RECTANGLE(DEFAULT_FB_ALLOC_WIDTH, DEFAULT_FB_ALLOC_HEIGHT);
    
    auto vidframerate_default = SPA_FRACTION(20, 1);
    auto vidframerate_min = SPA_FRACTION(0, 1);
    auto vidframerate_max = SPA_FRACTION(1000, 1);
    params[0] = reinterpret_cast<spa_pod*>(spa_pod_builder_add_object(&b,
                SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype,    SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format,    SPA_POD_CHOICE_ENUM_Id(5,
                                                SPA_VIDEO_FORMAT_RGBA,
                                                SPA_VIDEO_FORMAT_RGBA,
                                                SPA_VIDEO_FORMAT_BGRA,
                                                SPA_VIDEO_FORMAT_RGBx,
                                                SPA_VIDEO_FORMAT_BGRx
                                                ),
                SPA_FORMAT_VIDEO_size,      SPA_POD_CHOICE_RANGE_Rectangle(
                                                &vidsize_default,
                                                &vidsize_min,
                                                &vidsize_max),
                SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(
                                                &vidframerate_default,
                                                &vidframerate_min,
                                                &vidframerate_max)));
    
    int result = pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
      pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
    if (result < 0) {
      hook_log("[pipewire] connect failed: %s", strerror(-result));
      failed.store(true);
    }
  }

  static void registry_global(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const struct spa_dict *props) {
    THIS_CLASS *this_ptr = reinterpret_cast<THIS_CLASS *>(data);

    if (id != this_ptr->node_id)
      return;

    const spa_dict_item *serial = spa_dict_lookup_item(props, PW_KEY_OBJECT_SERIAL);
    if (!serial || !serial->value) {
      fprintf(stderr, "%s stream %u has no serial\n", red_text("[hook]").c_str(), id);
      return;
    }

    this_ptr->init(serial->value);
  }

  static constexpr pw_registry_events registry_events{ PW_VERSION_REGISTRY_EVENTS, THIS_CLASS::registry_global };

  ~PipewireScreenCast() {
    if (stream) pw_stream_disconnect(stream);
    if (stream) pw_stream_destroy(stream);
    if (core) pw_core_disconnect(core);
    if (context) pw_context_destroy(context);
    if (pw_mainloop) pw_main_loop_destroy(pw_mainloop);

  }

  void reset_last_frame_time() {
    // reset the last frame time to a very old time
    last_frame_time = std::chrono::high_resolution_clock::time_point(
      std::chrono::seconds(1)
    );
  }

  std::atomic<pw_main_loop*> pw_mainloop{nullptr}; // need to be freed using pw_main_loop_destroy
  std::atomic<pw_context*> context{nullptr}; // need to be freed using pw_context_destroy
  std::atomic<pw_core*> core{nullptr}; // need to be freed using pw_core_disconnect
  std::atomic<pw_stream*> stream{nullptr}; // need to be freed using pw_stream_destroy

  bool valid() const { return core.load() && registry; }
  std::atomic<bool> failed{false};

private:
  Interface* output;
  int node_id;
  pw_registry *registry{nullptr};
  spa_hook registry_listener;
  std::unique_ptr<uint8_t[]> param_buffer{nullptr};
  static constexpr size_t param_buffer_size = 1024;
  spa_pod_builder b;
  spa_hook listener;
  const spa_pod* params[1];
  pw_stream_events stream_events;
  std::chrono::time_point<std::chrono::high_resolution_clock> last_frame_time;
  int counter{0};
  double target_framerate;
  uint64_t reporting_interval;
  uint64_t processed_frame_count;

  struct ActualParams {
    uint32_t width{0};
    uint32_t height{0};
    double framerate{0.0f};
    double max_framerate{0.0f};
    SpaVideoFormat_e format{SpaVideoFormat_e::INVALID};
    bool param_good{false};
    
    void update_from_pod(const spa_pod* pod){
      spa_video_info_raw info{};
      auto retval = spa_format_video_raw_parse(pod, &info);
      fprintf(stderr, "%s", yellow_text("[payload pw] spa_format_video_raw_parse retval: " + std::to_string(retval) + "\n").c_str());
      if (retval < 0) { param_good = false; return; }
      width = info.size.width;
      height = info.size.height;
      framerate = static_cast<double>(info.framerate.num) / static_cast<double>(info.framerate.denom);
      max_framerate = static_cast<double>(info.max_framerate.num) / static_cast<double>(info.max_framerate.denom);
      format = SpaVideoFormat_e{static_cast<SpaVideoFormat_e>(info.format)};
      param_good = width > 0 && height > 0 && width <= DEFAULT_FB_ALLOC_WIDTH &&
        height <= DEFAULT_FB_ALLOC_HEIGHT && spa_videoformat_bytesize(format) == 4;
      
      std::string reporting_str = "width: " + std::to_string(width) + " | " +
                                  "height: " + std::to_string(height) + " | " +
                                  "framerate: " + std::to_string(framerate) + " | " +
                                  "max_framerate: " + std::to_string(max_framerate) + " | " +
                                  "format: " + spa_to_string(format) + " | " +
                                  "param_good: " + std::to_string(param_good);
      
      fprintf(stderr, "%s", yellow_text("[payload pw] actual params: " + reporting_str + "\n").c_str());
    }

  } actual_params;


  static void on_stream_state_changed(void* data, pw_stream_state old_state, pw_stream_state state, const char* error_message){
    std::string old_state_str = pw_stream_state_as_string(old_state);
    std::string state_str = pw_stream_state_as_string(state);
    fprintf(stderr, "%s", yellow_text("[payload pw] stream state changed from " + old_state_str + " to " + state_str + "\n").c_str());
    THIS_CLASS* this_ptr = reinterpret_cast<THIS_CLASS*>(data);
    this_ptr->reset_last_frame_time();
    hook_log("[pipewire] stream %s -> %s", old_state_str.c_str(), state_str.c_str());
    if (state == PW_STREAM_STATE_ERROR) {
      this_ptr->failed.store(true);
      hook_log("[pipewire] stream error: %s", error_message ? error_message : "unknown");
      fprintf(stderr, "%s", red_text("[payload pw] stream error: " + std::string(error_message ? error_message : "unknown") + "\n").c_str());
    }
  }
  
  static void on_param_changed(void* data, uint32_t id, const struct spa_pod* param){
    
    THIS_CLASS* this_ptr = reinterpret_cast<THIS_CLASS*>(data);
    this_ptr->reset_last_frame_time();
    std::string param_id_name_str = spa_debug_type_find_name(spa_type_param, id);
    fprintf(stderr, "%s", yellow_text("[payload pw] param changed. received param type: " + param_id_name_str + "\n").c_str());
    if (param == nullptr || id != SPA_PARAM_Format) {
      fprintf(stderr, "%s", yellow_text("[payload pw] ignoring non-format param\n").c_str());
      return;
    }
    // we gather the actual video stream params here
    this_ptr->actual_params.update_from_pod(param);

    uint8_t params_buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const struct spa_pod *params[3];

    int n_params = 0;
    params[n_params++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(&b,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
    params[n_params++] = (struct spa_pod *)(spa_pod_builder_add_object(&b,
      SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
      SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
      SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region))
    ));
    params[n_params++] = (struct spa_pod *)(spa_pod_builder_add_object(&b,
      SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
      SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoTransform),
      SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_videotransform))
    ));
    pw_stream_update_params(this_ptr->stream.load(), params, n_params);
  }

  static void on_process(void* data){
    THIS_CLASS* this_ptr = reinterpret_cast<THIS_CLASS*>(data);
    pw_buffer* b = pw_stream_dequeue_buffer(this_ptr->stream);
    if (b == nullptr) {
      fprintf(stderr, "%s", red_text("[payload pw] received a null buffer on processing. ignoring.\n").c_str());
      return;
    }

    auto cur_frame_time = std::chrono::high_resolution_clock::now();
    auto last_frame_time = this_ptr->last_frame_time;

    if (cur_frame_time - last_frame_time < std::chrono::milliseconds(int(1000 / this_ptr->target_framerate))) {
      // fprintf(stderr, "%s", yellow_text("[payload pw] frame came too fast. dropped.\n").c_str());
      pw_stream_queue_buffer(this_ptr->stream, b);
      return;
    }
    
    // start processing frame
    this_ptr->processed_frame_count++;
    this_ptr->last_frame_time = cur_frame_time;

    if (this_ptr->processed_frame_count % this_ptr->reporting_interval == 0) {
      fprintf(stderr, "%s", yellow_text("[payload pw] processed frame count: " + std::to_string(this_ptr->processed_frame_count) + "\n").c_str());
    }


    // try to write to the frame buffer if the param is good
    if (this_ptr->actual_params.param_good){
      // Negotiate CPU-readable memory and verify source bounds/stride.
      if (b->buffer->n_datas == 0) {
        pw_stream_queue_buffer(this_ptr->stream, b);
        return;
      }
      const auto& plane = b->buffer->datas[0];
      const size_t row_bytes = static_cast<size_t>(this_ptr->actual_params.width) * 4;
      const int stride = plane.chunk ? plane.chunk->stride : 0;
      const size_t offset = plane.chunk ? plane.chunk->offset : 0;
      const size_t needed = stride > 0 ? (this_ptr->actual_params.height - 1) * static_cast<size_t>(stride) + row_bytes : 0;
      if (!plane.data || !plane.chunk || stride < static_cast<int>(row_bytes) ||
          offset > plane.maxsize || needed > plane.maxsize - offset ||
          needed > plane.chunk->size) {
        pw_stream_queue_buffer(this_ptr->stream, b);
        return;
      }
      auto& interface = *this_ptr->output;
      std::lock_guard<std::mutex> frame_lock(interface.frame_mutex);
      auto& framebuffer = interface.framebuf;

      struct spa_meta_region *video_crop;
      if ((video_crop = (struct spa_meta_region *)spa_buffer_find_meta_data(b->buffer, SPA_META_VideoCrop, sizeof(*video_crop))) && spa_meta_region_is_valid(video_crop)) {
        framebuffer.crop_x = video_crop->region.position.x;
        framebuffer.crop_y = video_crop->region.position.y;
        framebuffer.crop_width = video_crop->region.size.width;
        framebuffer.crop_height = video_crop->region.size.height;
      } else {
        framebuffer.crop_width = framebuffer.crop_height = 0;
      }

      framebuffer.update_param(
        this_ptr->actual_params.height,
        this_ptr->actual_params.width,
        this_ptr->actual_params.format
      );
      
      struct spa_meta_videotransform *video_transform;
      if ((video_transform = (struct spa_meta_videotransform *)spa_buffer_find_meta_data(b->buffer, SPA_META_VideoTransform, sizeof(*video_transform)))) {
        switch (video_transform->transform) {
        case SPA_META_TRANSFORMATION_None:
        case SPA_META_TRANSFORMATION_Flipped:
          framebuffer.rotate = 0;
          break;
        case SPA_META_TRANSFORMATION_90:
        case SPA_META_TRANSFORMATION_Flipped90:
          framebuffer.rotate = 90;
          break;
        case SPA_META_TRANSFORMATION_180:
        case SPA_META_TRANSFORMATION_Flipped180:
          framebuffer.rotate = 180;
          break;
        case SPA_META_TRANSFORMATION_270:
        case SPA_META_TRANSFORMATION_Flipped270:
          framebuffer.rotate = -90;
          break;
        default:
          break;
        }
        switch (video_transform->transform) {
        case SPA_META_TRANSFORMATION_None:
        case SPA_META_TRANSFORMATION_90:
        case SPA_META_TRANSFORMATION_180:
        case SPA_META_TRANSFORMATION_270:
          framebuffer.flip = 0;
          break;
        case SPA_META_TRANSFORMATION_Flipped:
        case SPA_META_TRANSFORMATION_Flipped90:
        case SPA_META_TRANSFORMATION_Flipped180:
        case SPA_META_TRANSFORMATION_Flipped270:
          framebuffer.flip = 1;
          break;
        default:
          break;
        }
      } else {
        framebuffer.rotate = framebuffer.flip = 0;
      }

      auto* source = static_cast<const uint8_t*>(plane.data) + offset;
      for (uint32_t row = 0; row < framebuffer.height; ++row) {
        memcpy(framebuffer.data.get() + row * framebuffer.row_byte_stride,
               source + row * static_cast<size_t>(stride), row_bytes);
      }
      if (!interface.frames_ready.exchange(true)) {
        hook_log("[portal] first pipewire frame %ux%u", framebuffer.width, framebuffer.height);
      }
    }

    
    pw_stream_queue_buffer(this_ptr->stream, b);
    return;
  }


};
