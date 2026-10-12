#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) return 1;
  const bool expect_native = argc == 3 && std::strcmp(argv[2], "native") == 0;
  const char* meeting = std::getenv("WEMEET_XWAYLAND");
  if (!meeting || std::strcmp(meeting, "1") != 0) return 1;
  void* module = dlopen(argv[1], RTLD_NOW | RTLD_DEEPBIND);
  if (!module) { std::fprintf(stderr, "%s\n", dlerror()); return 1; }
  auto native = reinterpret_cast<bool(*)()>(dlsym(module, "uses_native_portal"));
  auto environment = reinterpret_cast<const char*(*)(const char*)>(dlsym(module, "module_environment"));
  if (!native || !environment) return 1;
  for (int attempt = 0; attempt < 3; ++attempt) {
    const char* share = environment("WEMEET_XWAYLAND");
    if (native() != expect_native || !share || std::strcmp(share, expect_native ? "1" : "0") != 0) return 1;
  }
  const char* session = environment("XDG_SESSION_TYPE");
  if (!session || std::strcmp(session, "x11") != 0 || environment("WAYLAND_DISPLAY")) return 1;
  if (std::strcmp(std::getenv("WEMEET_XWAYLAND"), "1") != 0) return 1;
  dlclose(module);
  std::printf("module PLT selects %s on every attempt; meeting environment remains 1/x11\n",
              expect_native ? "native portal" : "X11 bridge");
}
