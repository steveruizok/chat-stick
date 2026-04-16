#pragma once

#include <Arduino.h>

class WiFiService {
public:
  void init();
  bool connectKnownNetworks();
  void disconnect();

  bool isConnected() const;
  String ssid() const;
  String localIp() const;
};
