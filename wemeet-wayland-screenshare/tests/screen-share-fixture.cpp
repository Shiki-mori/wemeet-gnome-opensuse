#include <cstdlib>
#include <cstring>

// Synthetic fixture exercising the client's observed flag comparison;
// it contains no client code or binaries.
extern "C" bool uses_native_portal() {
  const char* flag = std::getenv("WEMEET_XWAYLAND");
  return flag && std::strcmp(flag, "1") == 0;
}

extern "C" const char* module_environment(const char* name) {
  return std::getenv(name);
}
