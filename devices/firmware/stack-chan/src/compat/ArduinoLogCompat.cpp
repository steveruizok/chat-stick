#include "Config.h"

#if STACK_CHAN_ENABLE_CAMERA
#include <cstdarg>
#include <esp32-hal-log.h>

extern "C" int log_printfv(const char *format, va_list arguments);

// Custom Arduino/IDF library builds retain --wrap=log_printf in a few M5Stack
// objects, but the rebuilt core exports only log_printf/log_printfv.
extern "C" int __wrap_log_printf(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const int result = log_printfv(format, arguments);
  va_end(arguments);
  return result;
}
#endif
