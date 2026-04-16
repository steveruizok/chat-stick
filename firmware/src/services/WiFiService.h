#pragma once

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>

class WiFiService {
public:
  void init();
  void poll();
  bool connectKnownNetworks();
  bool startCaptivePortal();
  void stopCaptivePortal();
  bool isCaptivePortalActive() const { return _captivePortalActive; }
  bool consumeProvisioningSuccess(String &ssid);
  void disconnect();
  void reset();

  bool isConnected() const;
  String ssid() const;
  String localIp() const;
  String captivePortalSsid() const { return kCaptivePortalSsid; }
  String captivePortalIp() const;

private:
  struct SavedNetwork {
    String ssid;
    String password;
    String label;
  };

  static constexpr int kMaxSavedNetworks = 5;
  static constexpr const char *kPrefsNamespace = "wifi";
  static constexpr const char *kCaptivePortalSsid = "chat-stick-setup";
  static constexpr byte kDnsPort = 53;
  static constexpr int kMaxScanResults = 8;

  Preferences _prefs;
  DNSServer _dnsServer;
  WebServer _portalServer{80};
  bool _prefsReady = false;
  bool _captivePortalActive = false;
  bool _portalRoutesConfigured = false;
  bool _portalProvisioned = false;
  String _provisionedSsid;
  SavedNetwork _savedNetworks[kMaxSavedNetworks];
  int _savedNetworkCount = 0;
  String _scanResults[kMaxScanResults];
  int _scanResultCount = 0;

  void loadSavedNetworks();
  void writeSavedNetworks();
  void rememberNetwork(const String &ssid, const String &password,
                       const String &label);
  bool connectToNetwork(const String &ssid, const String &password,
                        const String &label);
  void configureCaptivePortalRoutes();
  void refreshScanResults();
  String portalHtml(const String &message = "") const;
  void handlePortalRoot();
  void handlePortalSave();
  void redirectToPortal();
};
