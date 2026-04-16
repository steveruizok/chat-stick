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

void WiFiService::poll() {
  if (!_captivePortalActive) {
    return;
  }

  _dnsServer.processNextRequest();
  _portalServer.handleClient();
}

bool WiFiService::connectKnownNetworks() {
  stopCaptivePortal();
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

bool WiFiService::startCaptivePortal() {
  stopCaptivePortal();
  WiFi.disconnect(true, true);
  delay(100);
  WiFi.mode(WIFI_AP_STA);

  refreshScanResults();

  if (!WiFi.softAP(kCaptivePortalSsid)) {
    Serial.println("[WiFi] Failed to start captive portal AP");
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

  Serial.printf("[WiFi] Captive portal started: SSID=%s IP=%s\n",
                kCaptivePortalSsid, WiFi.softAPIP().toString().c_str());
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
  _portalServer.send(200, "text/html",
                     portalHtml("Saved. Return to the device; it is reconnecting now."));
  Serial.printf("[WiFi] Captive portal saved credentials for %s\n", ssid.c_str());
}

void WiFiService::redirectToPortal() {
  _portalServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString(),
                           true);
  _portalServer.send(302, "text/plain", "");
}
