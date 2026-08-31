#include "WiFiService.h"

#include "Config.h"
#include "credentials.h"
#include <WiFi.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

namespace {
/**
 * @brief Cap radio TX power when the device Config asks for it.
 * Must run after the WiFi driver is started (WiFi.mode / WiFi.begin).
 */
void applyTxPowerCap() {
  if (WIFI_MAX_TX_POWER_QDBM > 0) {
    WiFi.setTxPower(static_cast<wifi_power_t>(WIFI_MAX_TX_POWER_QDBM));
  }
}
} // namespace

void WiFiService::init() {
  _prefsReady = _prefs.begin(kPrefsNamespace, false);
  if (_prefsReady) {
    loadSavedNetworks();
  } else {
    log("Preferences init failed; saved networks disabled");
  }
}

void WiFiService::poll() {
  if (!_captivePortalActive) {
    return;
  }

  _dnsServer.processNextRequest();
  _portalServer.handleClient();
}

bool WiFiService::connectKnownNetworks() {
  stopCaptivePortal();
  if (WIFI_DISABLE_PERSISTENT_STORAGE) {
    WiFi.persistent(false);
  }
  WiFi.mode(WIFI_STA);
  if (WIFI_DISABLE_SLEEP_DURING_CONNECT) {
    WiFi.setSleep(false);
  }
  const unsigned long startMs = millis();

  if (!WIFI_SCAN_BEFORE_FALLBACK_CONNECT) {
    for (int i = 0; i < _savedNetworkCount; i++) {
      const SavedNetwork &network = _savedNetworks[i];
      if (connectToNetwork(network.ssid, network.password, network.label,
                           WIFI_CONNECT_TIMEOUT_SEC * 1000UL)) {
        rememberNetwork(network.ssid, network.password, network.label);
        return true;
      }
    }

    for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
      if (connectToNetwork(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password,
                           WIFI_NETWORKS[i].label,
                           WIFI_CONNECT_TIMEOUT_SEC * 1000UL)) {
        rememberNetwork(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password,
                        WIFI_NETWORKS[i].label);
        return true;
      }
    }

    log("all networks failed");
    return false;
  }

  bool retryPrimaryBySsid = false;
  bool skipFirstConfiguredNetwork = false;
  if (_savedNetworkCount > 0) {
    const SavedNetwork network = _savedNetworks[0];
    const bool hasFastHints =
        WIFI_USE_FAST_CONNECT_HINTS && network.hasBssid && network.channel > 0;
    log("Primary saved network %s fast_hints=%d channel=%ld",
        network.ssid.c_str(), hasFastHints ? 1 : 0,
        static_cast<long>(network.channel));
    const unsigned long timeoutMs =
        hasFastHints ? kPrimaryHintConnectTimeoutMs : kPrimaryConnectTimeoutMs;
    if (connectToNetwork(network.ssid, network.password, network.label,
                         timeoutMs, hasFastHints ? network.channel : 0,
                         hasFastHints ? network.bssid : nullptr)) {
      rememberNetwork(network.ssid, network.password, network.label);
      log("Connected in %lums", millis() - startMs);
      return true;
    }
    retryPrimaryBySsid = hasFastHints;
  } else if (WIFI_NETWORK_COUNT > 0) {
    if (connectToNetwork(WIFI_NETWORKS[0].ssid, WIFI_NETWORKS[0].password,
                         WIFI_NETWORKS[0].label, kPrimaryConnectTimeoutMs)) {
      rememberNetwork(WIFI_NETWORKS[0].ssid, WIFI_NETWORKS[0].password,
                      WIFI_NETWORKS[0].label);
      log("Connected in %lums", millis() - startMs);
      return true;
    }
    skipFirstConfiguredNetwork = true;
  }

  const unsigned long scanStartMs = millis();
  refreshScanResults();
  log("Scan found %d network(s) in %lums", _scanResultCount,
      millis() - scanStartMs);
  const bool hasScanResults = _scanResultCount > 0;
  const int passCount = hasScanResults ? 2 : 1;

  for (int pass = 0; pass < passCount; pass++) {
    const bool visiblePass = pass == 0;

    for (int i = 0; i < _savedNetworkCount; i++) {
      if (i == 0 && !retryPrimaryBySsid) {
        continue;
      }

      const SavedNetwork network = _savedNetworks[i];
      const bool visible = !hasScanResults || isScannedSsid(network.ssid);
      if (hasScanResults && visible != visiblePass) {
        continue;
      }

      const unsigned long timeoutMs =
          visible ? kFallbackConnectTimeoutMs : kUnavailableConnectTimeoutMs;
      if (connectToNetwork(network.ssid, network.password, network.label,
                           timeoutMs)) {
        rememberNetwork(network.ssid, network.password, network.label);
        log("Connected in %lums", millis() - startMs);
        return true;
      }
    }

    for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
      if (i == 0 && skipFirstConfiguredNetwork) {
        continue;
      }

      if (WIFI_SKIP_SAVED_CONFIGURED_DUPLICATES &&
          hasSavedNetwork(WIFI_NETWORKS[i].ssid)) {
        continue;
      }

      const bool visible =
          !hasScanResults || isScannedSsid(WIFI_NETWORKS[i].ssid);
      if (hasScanResults && visible != visiblePass) {
        continue;
      }

      const unsigned long timeoutMs =
          visible ? kFallbackConnectTimeoutMs : kUnavailableConnectTimeoutMs;
      if (connectToNetwork(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password,
                           WIFI_NETWORKS[i].label, timeoutMs)) {
        rememberNetwork(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password,
                        WIFI_NETWORKS[i].label);
        log("Connected in %lums", millis() - startMs);
        return true;
      }
    }
  }

  log("All networks failed after %lums", millis() - startMs);
  return false;
}

bool WiFiService::startCaptivePortal() {
  stopCaptivePortal();
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  applyTxPowerCap();

  refreshScanResults();

  if (!WiFi.softAP(kCaptivePortalSsid)) {
    log("Failed to start captive portal AP");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  delay(100);
  _dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  configureCaptivePortalRoutes();
  _portalServer.begin();
  _captivePortalActive = true;
  _portalProvisioned = false;
  _provisionedSsid = "";

  log("Captive portal started: SSID=%s IP=%s", kCaptivePortalSsid,
      WiFi.softAPIP().toString().c_str());
  return true;
}

void WiFiService::stopCaptivePortal() {
  if (_captivePortalActive) {
    _dnsServer.stop();
    _portalServer.stop();
    _captivePortalActive = false;
  }

  if (WiFi.getMode() != WIFI_OFF) {
    WiFi.softAPdisconnect(true);
  }
}

bool WiFiService::consumeProvisioningSuccess(String &ssid) {
  if (!_portalProvisioned) {
    return false;
  }

  ssid = _provisionedSsid;
  _portalProvisioned = false;
  _provisionedSsid = "";
  stopCaptivePortal();
  return true;
}

void WiFiService::disconnect() {
  stopCaptivePortal();
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

String WiFiService::captivePortalIp() const {
  return _captivePortalActive ? WiFi.softAPIP().toString() : "";
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

    if (WIFI_USE_FAST_CONNECT_HINTS) {
      _savedNetworks[i].channel =
          _prefs.getInt(("chan_" + suffix).c_str(), 0);
      const String bssidKey = "bssid_" + suffix;
      _savedNetworks[i].hasBssid =
          _prefs.getBytesLength(bssidKey.c_str()) == kBssidLength;
      if (_savedNetworks[i].hasBssid) {
        _prefs.getBytes(bssidKey.c_str(), _savedNetworks[i].bssid,
                        kBssidLength);
      }
    }

    if (_savedNetworks[i].label.isEmpty()) {
      _savedNetworks[i].label = "Saved";
    }
  }

  log("Loaded %d saved network(s)", _savedNetworkCount);
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
    const String channelKey = "chan_" + suffix;
    const String bssidKey = "bssid_" + suffix;

    if (i < _savedNetworkCount) {
      _prefs.putString(ssidKey.c_str(), _savedNetworks[i].ssid);
      _prefs.putString(passKey.c_str(), _savedNetworks[i].password);
      _prefs.putString(labelKey.c_str(), _savedNetworks[i].label);
      if (WIFI_USE_FAST_CONNECT_HINTS) {
        _prefs.putInt(channelKey.c_str(), _savedNetworks[i].channel);
        if (_savedNetworks[i].hasBssid) {
          _prefs.putBytes(bssidKey.c_str(), _savedNetworks[i].bssid,
                          kBssidLength);
        } else {
          _prefs.remove(bssidKey.c_str());
        }
      }
    } else {
      _prefs.remove(ssidKey.c_str());
      _prefs.remove(passKey.c_str());
      _prefs.remove(labelKey.c_str());
      if (WIFI_USE_FAST_CONNECT_HINTS) {
        _prefs.remove(channelKey.c_str());
        _prefs.remove(bssidKey.c_str());
      }
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
  if (WIFI_USE_FAST_CONNECT_HINTS && WiFi.status() == WL_CONNECTED &&
      WiFi.SSID() == ssid) {
    network.channel = WiFi.channel();
    uint8_t *connectedBssid = WiFi.BSSID();
    if (connectedBssid) {
      memcpy(network.bssid, connectedBssid, kBssidLength);
      network.hasBssid = true;
    }
  }

  if (existingIndex > 0) {
    for (int i = existingIndex; i > 0; i--) {
      _savedNetworks[i] = _savedNetworks[i - 1];
    }
    _savedNetworks[0] = network;
  } else if (existingIndex == 0) {
    const SavedNetwork &current = _savedNetworks[0];
    if (current.password == network.password && current.label == network.label &&
        (!WIFI_USE_FAST_CONNECT_HINTS ||
         (current.channel == network.channel &&
          current.hasBssid == network.hasBssid &&
          (!network.hasBssid ||
           memcmp(current.bssid, network.bssid, kBssidLength) == 0)))) {
      return;
    }
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
  log("Saved network %s to NVS", ssid.c_str());
}

bool WiFiService::connectToNetwork(const String &ssid, const String &password,
                                   const String &label,
                                   unsigned long timeoutMs, int32_t channel,
                                   const uint8_t *bssid) {
  if (ssid.isEmpty()) {
    return false;
  }

  if (WIFI_LOG_CONNECT_DETAILS) {
    log("Trying %s (%s, timeout=%lums, channel=%ld, bssid=%d)...",
        ssid.c_str(), label.isEmpty() ? "Saved" : label.c_str(), timeoutMs,
        static_cast<long>(channel), bssid ? 1 : 0);
  } else {
    log("trying %s (%s)", ssid.c_str(),
        label.isEmpty() ? "Saved" : label.c_str());
  }

  const unsigned long startMs = millis();
  if (bssid && channel > 0) {
    WiFi.begin(ssid.c_str(), password.c_str(), channel, bssid);
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }
  // Cap TX power for the association burst and everything after — full-power
  // bursts can brown out the battery rail on small-battery devices.
  applyTxPowerCap();

  while (WiFi.status() != WL_CONNECTED && millis() - startMs < timeoutMs) {
    delay(WIFI_CONNECT_POLL_MS);
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (WIFI_LOG_CONNECT_DETAILS) {
      log("Connected to %s in %lums - %s", ssid.c_str(), millis() - startMs,
          WiFi.localIP().toString().c_str());
    } else {
      log("connected to %s ip=%s", ssid.c_str(),
          WiFi.localIP().toString().c_str());
    }
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    configTime(0, 0, NTP_SERVER);
    return true;
  }

  if (WIFI_LOG_CONNECT_DETAILS) {
    log("Failed %s after %lums status=%d", ssid.c_str(), millis() - startMs,
        static_cast<int>(WiFi.status()));
  }
  WiFi.disconnect();
  return false;
}

bool WiFiService::hasSavedNetwork(const String &ssid) const {
  for (int i = 0; i < _savedNetworkCount; i++) {
    if (_savedNetworks[i].ssid == ssid) {
      return true;
    }
  }
  return false;
}

bool WiFiService::isScannedSsid(const String &ssid) const {
  for (int i = 0; i < _scanResultCount; i++) {
    if (_scanResults[i] == ssid) {
      return true;
    }
  }
  return false;
}

void WiFiService::configureCaptivePortalRoutes() {
  if (_portalRoutesConfigured) {
    return;
  }

  _portalServer.on("/", HTTP_GET, [this]() { handlePortalRoot(); });
  _portalServer.on("/save", HTTP_POST, [this]() { handlePortalSave(); });

  _portalServer.on("/generate_204", HTTP_ANY, [this]() { redirectToPortal(); });
  _portalServer.on("/hotspot-detect.html", HTTP_ANY,
                   [this]() { redirectToPortal(); });
  _portalServer.on("/connecttest.txt", HTTP_ANY,
                   [this]() { redirectToPortal(); });
  _portalServer.on("/ncsi.txt", HTTP_ANY, [this]() { redirectToPortal(); });
  _portalServer.on("/fwlink", HTTP_ANY, [this]() { redirectToPortal(); });
  _portalServer.onNotFound([this]() { redirectToPortal(); });
  _portalRoutesConfigured = true;
}

void WiFiService::refreshScanResults() {
  _scanResultCount = 0;
  const int found = WiFi.scanNetworks(false, true);
  if (found <= 0) {
    return;
  }

  for (int i = 0; i < found && _scanResultCount < kMaxScanResults; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) {
      continue;
    }

    bool duplicate = false;
    for (int j = 0; j < _scanResultCount; j++) {
      if (_scanResults[j] == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    _scanResults[_scanResultCount++] = ssid;
  }
  WiFi.scanDelete();
}

String WiFiService::portalHtml(const String &message) const {
  String html;
  html.reserve(2048);
  html += "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>chat-stick setup</title>";
  html += "<style>body{font-family:sans-serif;max-width:32rem;margin:2rem auto;padding:0 1rem;background:#111;color:#f5f5f5}";
  html += "form{display:grid;gap:.75rem}input,select,button{font:inherit;padding:.75rem;border-radius:.5rem;border:1px solid #444}";
  html += "button{background:#fff;color:#111;font-weight:700}small{color:#bbb}.msg{padding:.75rem;border-radius:.5rem;background:#1d3b24;color:#d7ffd7}</style></head><body>";
  html += "<h1>chat-stick WiFi setup</h1>";
  html += "<p>Join this device, then submit your WiFi credentials. The device will save them and reconnect automatically.</p>";
  if (!message.isEmpty()) {
    html += "<div class='msg'>";
    html += message;
    html += "</div>";
  }
  html += "<form method='post' action='/save'>";
  html += "<label>Network<select name='ssid'><option value=''>Choose a network</option>";
  for (int i = 0; i < _scanResultCount; i++) {
    html += "<option value='";
    html += _scanResults[i];
    html += "'>";
    html += _scanResults[i];
    html += "</option>";
  }
  html += "</select></label>";
  html += "<label>Or enter SSID<input name='manual_ssid' placeholder='Network name'></label>";
  html += "<label>Password<input name='password' type='password' placeholder='Password'></label>";
  html += "<button type='submit'>Save and reconnect</button>";
  html += "</form><p><small>Portal IP: ";
  html += WiFi.softAPIP().toString();
  html += "</small></p></body></html>";
  return html;
}

void WiFiService::handlePortalRoot() {
  _portalServer.send(200, "text/html", portalHtml());
}

void WiFiService::handlePortalSave() {
  String selectedSsid = _portalServer.arg("ssid");
  String manualSsid = _portalServer.arg("manual_ssid");
  String password = _portalServer.arg("password");
  selectedSsid.trim();
  manualSsid.trim();
  password.trim();
  const String ssid = manualSsid.isEmpty() ? selectedSsid : manualSsid;

  if (ssid.isEmpty()) {
    _portalServer.send(400, "text/html",
                       portalHtml("Enter an SSID or choose one from the list."));
    return;
  }

  rememberNetwork(ssid, password, "Portal");
  _provisionedSsid = ssid;
  _portalProvisioned = true;
  _portalServer.send(
      200, "text/html",
      portalHtml("Saved. Return to the device; it is reconnecting now."));
  log("Captive portal saved credentials for %s", ssid.c_str());
}

void WiFiService::redirectToPortal() {
  _portalServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString(),
                           true);
  _portalServer.send(302, "text/plain", "");
}

void WiFiService::log(const char *fmt, ...) const {
  char message[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  if (_logCallback) {
    _logCallback("WiFi", message);
  } else {
    Serial.printf("[WiFi] %s\n", message);
  }
}
