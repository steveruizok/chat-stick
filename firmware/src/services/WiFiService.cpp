#include "WiFiService.h"

#include "../Config.h"
#include "../credentials.h"
#include <WiFi.h>

void WiFiService::init() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);
}

bool WiFiService::connectKnownNetworks() {
  WiFi.mode(WIFI_STA);

  for (int i = 0; i < WIFI_NETWORK_COUNT; i++) {
    Serial.printf("[WiFi] Trying %s (%s)...\n", WIFI_NETWORKS[i].ssid,
                  WIFI_NETWORKS[i].label);

    WiFi.begin(WIFI_NETWORKS[i].ssid, WIFI_NETWORKS[i].password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED &&
           attempts < WIFI_CONNECT_TIMEOUT_SEC * 4) {
      delay(250);
      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WiFi] Connected to %s — %s\n", WIFI_NETWORKS[i].ssid,
                    WiFi.localIP().toString().c_str());
      WiFi.setSleep(WIFI_PS_MIN_MODEM);
      return true;
    }

    WiFi.disconnect();
  }

  Serial.println("[WiFi] All networks failed");
  return false;
}

void WiFiService::disconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

bool WiFiService::isConnected() const { return WiFi.status() == WL_CONNECTED; }

String WiFiService::ssid() const { return isConnected() ? WiFi.SSID() : ""; }

String WiFiService::localIp() const {
  return isConnected() ? WiFi.localIP().toString() : "";
}
