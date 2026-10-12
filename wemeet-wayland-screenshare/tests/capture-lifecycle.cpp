#include "interface.hpp"
#include <cstdio>
#include <memory>
#include <thread>

int main() {
  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto start = std::chrono::steady_clock::now();
    auto session = std::make_unique<CaptureSession>();
    session->start();
    if (attempt == 2) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      session->stop();
    }
    if (session->wait_until_started()) {
      std::fprintf(stderr, "Cancelled mock session unexpectedly started\n");
      return 1;
    }
    session.reset();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed > std::chrono::seconds(4)) {
      std::fprintf(stderr, "Cancellation/stop failed to join within 4 seconds\n");
      return 1;
    }
    std::printf("attempt %d: cancelled/stopped and joined\n", attempt + 1);
  }
}
