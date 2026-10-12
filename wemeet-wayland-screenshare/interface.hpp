#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "framebuf.hpp"

struct Interface {
  FrameBuffer framebuf{DEFAULT_FB_ALLOC_HEIGHT, DEFAULT_FB_ALLOC_WIDTH,
                       1080, 1920, SpaVideoFormat_e::RGBA};
  std::mutex frame_mutex;
  std::atomic<bool> frames_ready{false};
  std::atomic<bool> x11_sanitizer_stop_flag{false};
};

enum class CaptureStatus { Starting, Running, Cancelled, Stopped };

// Hooks retain a shared owner while reading. The worker exclusively owns
// the portal/PipeWire objects; destruction requests stop and joins it.
class CaptureSession {
public:
  CaptureSession();
  ~CaptureSession();
  CaptureSession(const CaptureSession&) = delete;
  CaptureSession& operator=(const CaptureSession&) = delete;

  void start();
  void stop();
  bool wait_until_started();
  bool finished() const { return finished_.load(); }
  CaptureStatus status() const { return status_.load(); }
  Interface interface;

private:
  void run();
  void publish(CaptureStatus status);
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> finished_{false};
  std::atomic<CaptureStatus> status_{CaptureStatus::Starting};
  std::mutex state_mutex_;
  std::condition_variable state_changed_;
  std::thread worker_;
};
