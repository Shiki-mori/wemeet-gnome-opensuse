#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <unistd.h>

inline void hook_log(const char* fmt, ...) {
  FILE* file = fopen("/tmp/wemeet-hook.log", "a");
  if (!file) return;
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  tm local{};
  localtime_r(&now.tv_sec, &local);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
  fprintf(file, "%s.%03ld pid=%d ", stamp, now.tv_nsec / 1000000, getpid());
  va_list args;
  va_start(args, fmt);
  vfprintf(file, fmt, args);
  va_end(args);
  fputc('\n', file);
  fclose(file);
}
