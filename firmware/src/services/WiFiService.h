#pragma once

#include <Arduino.h>
#include <Preferences.h>

class WiFiService {
public:
  void init();
  bool connectKnownNetworks();
  void disconnect();

  bool isConnected() const;
  String ssid() const;
  String localIp() const;

private:
  struct SavedNetwork {
    String ssid;
    String password;
    String label;
  };

  static constexpr int kMaxSavedNetworks = 5;
  static constexpr const char *kPrefsNamespace = "wifi";

  Preferences _prefs;
  bool _prefsReady = false;
  SavedNetwork _savedNetworks[kMaxSavedNetworks];
  int _savedNetworkCount = 0;

  void loadSavedNetworks();
  void writeSavedNetworks();
  void rememberNetwork(const String &ssid, const String &password,
                       const String &label);
  bool connectToNetwork(const String &ssid, const String &password,
                        const String &label);
};
