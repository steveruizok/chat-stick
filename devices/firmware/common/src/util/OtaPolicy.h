#pragma once
#include <cstdio>
#include <string>

namespace OtaPolicy {
inline bool isLanHost(const std::string &host) {
  if (host == "localhost" ||
      (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0)) return true;
  unsigned a, b, c, d;
  char tail;
  if (std::sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 ||
      a > 255 || b > 255 || c > 255 || d > 255) return false;
  return a == 10 || a == 127 || (a == 192 && b == 168) ||
         (a == 172 && b >= 16 && b <= 31);
}
// Match the entire configured origin and route, including device and port.
// This deliberately accepts neither alternate authorities nor redirects.
inline bool allows(const std::string &url, const std::string &origin,
                   const std::string &host, bool hasCa, const char *device) {
  const bool tls = origin.compare(0, 8, "https://") == 0;
  const bool lan = origin.compare(0, 7, "http://") == 0 && isLanHost(host);
  return ((tls && hasCa) || lan) &&
         url == origin + "/firmware/download?device=" + device;
}
}
