#include "WiFiService.h"

#include "../Config.h"
#include "../credentials.h"
#include <WiFi.h>

void WiFiService::init() {
  _prefsReady = _prefs.begin(kPrefsNamespace, false);
  if (_prefsReady) {
    loadSavedNetworks();
  } else {
    Serial.println("[WiFi] Preferences init failed; saved networks disabled");
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

bool WiFiService::connectKnownNetworks() {
  WiFi.mode(WIFI_STA);

  for (int i = 0; i < _savedNetworkCount; i++) {
    const SavedNetwork &network = _savedNetworks[i];
    if (connectToNetwork(network.ssid, network.password, network.label)) {
      return true;
    }
  }

  for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
    if (connectToNetwork(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password,
                         WIFI_NETWORKS[i].label)) {
      rememberNetwork(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password,
                      WIFI_NETWORKS[i].label);
      return true;
    }
  }

  Serial.println("[WiFi] All networks failed");
  return false;
}

void WiFiService::disconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void WiFiService::reset() {
  _savedNetworkCount = 0;
  if (_prefsReady) {
    _prefs.clear();
  }
  disconnect();
}

bool WiFiService::isConnected() const { return WiFi.status() == WL_CONNECTED; }

String WiFiService::ssid() const { return isConnected() ? WiFi.SSID() : ""; }

String WiFiService::localIp() const {
  return isConnected() ? WiFi.localIP().toString() : "";
}

void WiFiService::loadSavedNetworks() {
  _savedNetworkCount = constrain(_prefs.getUChar("count", 0), 0,
                                 kMaxSavedNetworks);

  for (int i = 0; i < _savedNetworkCount; i++) {
    const String suffix = String(i);
    _savedNetworks[i].ssid = _prefs.getString(("ssid_" + suffix).c_str(), "");
    _savedNetworks[i].password =
        _prefs.getString(("pass_" + suffix).c_str(), "");
    _savedNetworks[i].label =
        _prefs.getString(("label_" + suffix).c_str(), "");

    if (_savedNetworks[i].label.isEmpty()) {
      _savedNetworks[i].label = "Saved";
    }
  }

  Serial.printf("[WiFi] Loaded %d saved network(s)\n", _savedNetworkCount);
}

void WiFiService::writeSavedNetworks() {
  if (!_prefsReady) {
    return;
  }

  _prefs.putUChar("count", static_cast<uint8_t>(_savedNetworkCount));
  for (int i = 0; i < kMaxSavedNetworks; i++) {
    const String suffix = String(i);
    const String ssidKey = "ssid_" + suffix;
    const String passKey = "pass_" + suffix;
    const String labelKey = "label_" + suffix;

    if (i < _savedNetworkCount) {
      _prefs.putString(ssidKey.c_str(), _savedNetworks[i].ssid);
      _prefs.putString(passKey.c_str(), _savedNetworks[i].password);
      _prefs.putString(labelKey.c_str(), _savedNetworks[i].label);
    } else {
      _prefs.remove(ssidKey.c_str());
      _prefs.remove(passKey.c_str());
      _prefs.remove(labelKey.c_str());
    }
  }
}

void WiFiService::rememberNetwork(const String &ssid, const String &password,
                                  const String &label) {
  if (!_prefsReady || ssid.isEmpty()) {
    return;
  }

  int existingIndex = -1;
  for (int i = 0; i < _savedNetworkCount; i++) {
    if (_savedNetworks[i].ssid == ssid) {
      existingIndex = i;
      break;
    }
  }

  SavedNetwork network{ssid, password, label};
  if (network.label.isEmpty()) {
    network.label = "Saved";
  }

  if (existingIndex > 0) {
    for (int i = existingIndex; i > 0; i--) {
      _savedNetworks[i] = _savedNetworks[i - 1];
    }
    _savedNetworks[0] = network;
  } else if (existingIndex == 0) {
    _savedNetworks[0] = network;
  } else {
    const int limit = min(_savedNetworkCount, kMaxSavedNetworks - 1);
    for (int i = limit; i > 0; i--) {
      _savedNetworks[i] = _savedNetworks[i - 1];
    }
    _savedNetworks[0] = network;
    if (_savedNetworkCount < kMaxSavedNetworks) {
      _savedNetworkCount++;
    }
  }

  writeSavedNetworks();
  Serial.printf("[WiFi] Saved network %s to NVS\n", ssid.c_str());
}

bool WiFiService::connectToNetwork(const String &ssid, const String &password,
                                   const String &label) {
  if (ssid.isEmpty()) {
    return false;
  }

  Serial.printf("[WiFi] Trying %s (%s)...\n", ssid.c_str(),
                label.isEmpty() ? "Saved" : label.c_str());

  WiFi.begin(ssid.c_str(), password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED &&
         attempts < WIFI_CONNECT_TIMEOUT_SEC * 4) {
    delay(250);
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected to %s — %s\n", ssid.c_str(),
                  WiFi.localIP().toString().c_str());
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    return true;
  }

  WiFi.disconnect();
  return false;
}
