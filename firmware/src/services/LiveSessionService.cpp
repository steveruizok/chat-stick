#include "LiveSessionService.h"

#include "../Config.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

using namespace websockets;

void LiveSessionService::init(const LiveSessionCallbacks &callbacks) {
  _callbacks = callbacks;
  _ws.onMessage([this](WebsocketsMessage msg) { handleMessage(msg); });
  _ws.onEvent(
      [this](WebsocketsEvent event, String data) { handleEvent(event, data); });
}

void LiveSessionService::connect() {
  _activeServerIndex = _nextServerIndex;
  const ServerEndpoint &endpoint = SERVER_ENDPOINTS[_nextServerIndex];
  _nextServerIndex = (_nextServerIndex + 1) % SERVER_ENDPOINT_COUNT;

  const String path = String(SERVER_PATH) + "?device_id=" + DEVICE_ID;
  const String chatQuery = _chatId.isEmpty() ? "" : "&chat_id=" + _chatId;
  const char *scheme = endpoint.port == 443 ? "wss" : "ws";

  if (_callbacks.onStatus) {
    _callbacks.onStatus("Connecting...");
  }

  Serial.printf("[WS] Connecting to %s://%s:%d%s\n", scheme, endpoint.host,
                endpoint.port, (path + chatQuery).c_str());

  if (endpoint.ca_cert) {
    _ws.setCACert(endpoint.ca_cert);
  } else {
    _ws.setInsecure();
  }

  if (endpoint.port == 443) {
    _connected =
        _ws.connectSecure(endpoint.host, endpoint.port, (path + chatQuery).c_str());
  } else {
    _connected = _ws.connect(endpoint.host, endpoint.port, (path + chatQuery).c_str());
  }

  if (!_connected) {
    Serial.printf("[WS] Connect failed: %s://%s:%d — will retry\n", scheme,
                  endpoint.host, endpoint.port);
  }
}

void LiveSessionService::disconnect() {
  if (_ws.available()) {
    _ws.close();
  }
  _connected = false;
}

void LiveSessionService::poll() {
  if (_connected) {
    _ws.poll();
  }
}

void LiveSessionService::reconnectIfNeeded(bool enabled) {
  if (_connected || !enabled) {
    return;
  }

  const unsigned long now = millis();
  if (now - _lastReconnectMs < kReconnectMs) {
    return;
  }

  _lastReconnectMs = now;
  connect();
}

String LiveSessionService::activeEndpointLabel() const {
  if (_activeServerIndex < 0 || _activeServerIndex >= SERVER_ENDPOINT_COUNT) {
    return "no server";
  }
  // Index 0 is the local dev worker; anything else is treated as prod.
  return _activeServerIndex == 0 ? "dev" : "prod";
}

bool LiveSessionService::sendStart() { return _ws.send("{\"type\":\"start\"}"); }

bool LiveSessionService::sendStop() { return _ws.send("{\"type\":\"stop\"}"); }

bool LiveSessionService::sendAudio(const int16_t *data, size_t len) {
  return _ws.sendBinary(reinterpret_cast<const char *>(data), len);
}

bool LiveSessionService::fetchLastAssistantMessage(String &outMessage) {
  outMessage = "";
  if (_chatId.isEmpty()) {
    return false;
  }

  for (int offset = 0; offset < SERVER_ENDPOINT_COUNT; offset++) {
    const int index = (_nextServerIndex + offset) % SERVER_ENDPOINT_COUNT;
    const ServerEndpoint &endpoint = SERVER_ENDPOINTS[index];
    const String url = endpointBaseUrl(endpoint) + "/session/" + _chatId +
                       "?device_id=" + DEVICE_ID;

    Serial.printf("[HTTP] Restoring session from %s\n", url.c_str());

    int statusCode = -1;
    String body;
    {
      HTTPClient http;
      if (endpoint.port == 443) {
        WiFiClientSecure client;
        if (endpoint.ca_cert) {
          client.setCACert(endpoint.ca_cert);
        } else {
          client.setInsecure();
        }
        if (!http.begin(client, url)) {
          continue;
        }
        statusCode = http.GET();
        if (statusCode > 0) {
          body = http.getString();
        }
        http.end();
      } else {
        WiFiClient client;
        if (!http.begin(client, url)) {
          continue;
        }
        statusCode = http.GET();
        if (statusCode > 0) {
          body = http.getString();
        }
        http.end();
      }
    }

    if (statusCode == 404) {
      return false;
    }

    if (statusCode != 200 || body.isEmpty()) {
      Serial.printf("[HTTP] Session restore failed: status=%d\n", statusCode);
      continue;
    }

    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      Serial.println("[HTTP] Session restore returned invalid JSON");
      continue;
    }

    const char *lastMessage = doc["last_message"];
    if (!lastMessage || !lastMessage[0]) {
      return false;
    }

    outMessage = lastMessage;
    return true;
  }

  return false;
}

void LiveSessionService::handleEvent(WebsocketsEvent event, String data) {
  switch (event) {
  case WebsocketsEvent::ConnectionOpened:
    Serial.println("[WS] Opened");
    _connected = true;
    if (_callbacks.onStatus) {
      _callbacks.onStatus("Waiting for AI...");
    }
    break;

  case WebsocketsEvent::ConnectionClosed:
    Serial.printf("[WS] Closed: %s\n", data.c_str());
    _connected = false;
    if (_callbacks.onStatus) {
      _callbacks.onStatus("Reconnecting...");
    }
    break;

  case WebsocketsEvent::GotPing:
  case WebsocketsEvent::GotPong:
    break;
  }
}

void LiveSessionService::handleMessage(WebsocketsMessage msg) {
  if (_callbacks.onActivity) {
    _callbacks.onActivity();
  }

  if (msg.isBinary()) {
    const auto raw = msg.rawData();
    if (raw.length() >= 16 && _callbacks.onAudio) {
      _callbacks.onAudio(reinterpret_cast<const uint8_t *>(raw.c_str()),
                         raw.length());
    }
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, msg.data())) {
    return;
  }

  const char *type = doc["type"];
  if (!type) {
    return;
  }

  if (strcmp(type, "session") == 0) {
    const char *id = doc["chatId"];
    if (id && _callbacks.onChatId) {
      _callbacks.onChatId(id);
    }
    return;
  }

  if (strcmp(type, "ready") == 0) {
    Serial.println("[Server] Gemini session ready");
    if (_callbacks.onReady) {
      _callbacks.onReady();
    }
    return;
  }

  if (strcmp(type, "turn_complete") == 0) {
    Serial.println("[Server] Turn complete");
    if (_callbacks.onTurnComplete) {
      _callbacks.onTurnComplete();
    }
    return;
  }

  if (strcmp(type, "tool_call") == 0) {
    handleToolCall(doc);
    return;
  }

  if (strcmp(type, "transcript") == 0) {
    Serial.printf("[Transcript] %s: %s\n", doc["source"].as<const char *>(),
                  doc["text"].as<const char *>());
    return;
  }

  if (strcmp(type, "error") == 0) {
    const char *message = doc["message"];
    Serial.printf("[Server] Error: %s\n", message ? message : "unknown");
    if (_callbacks.onError) {
      _callbacks.onError(message ? message : "Server error");
    }
    return;
  }

  if (strcmp(type, "ignore_audio") == 0) {
    const char *reason = doc["reason"];
    if (_callbacks.onIgnoredAudio) {
      _callbacks.onIgnoredAudio(reason ? reason : "ignored");
    }
  }
}

void LiveSessionService::handleToolCall(const JsonDocument &doc) {
  const char *name = doc["name"];
  const char *id = doc["id"];
  if (!name || !id) {
    return;
  }

  String result = "ok";

  if (strcmp(name, "set_brightness") == 0) {
    const int level = doc["args"]["level"].is<int>()
                          ? constrain(doc["args"]["level"].as<int>(), 0, 255)
                          : DEFAULT_BRIGHTNESS;
    if (_callbacks.onBrightness) {
      _callbacks.onBrightness(level);
    }
    result = String("Brightness set to ") + level;
  } else if (strcmp(name, "set_volume") == 0) {
    const int level = doc["args"]["level"].is<int>()
                          ? constrain(doc["args"]["level"].as<int>(), 0, 255)
                          : DEFAULT_VOLUME;
    if (_callbacks.onVolume) {
      _callbacks.onVolume(level);
    }
    result = String("Volume set to ") + level;
  } else if (strcmp(name, "get_device_status") == 0) {
    result = _callbacks.getDeviceStatusJson ? _callbacks.getDeviceStatusJson()
                                            : "{}";
  } else if (strcmp(name, "show_text") == 0) {
    const char *text = doc["args"]["text"];
    if (text && _callbacks.onShowText) {
      _callbacks.onShowText(text);
    }
    result = "Text displayed";
  } else if (strcmp(name, "play_sound") == 0) {
    const char *sound = doc["args"]["sound"];
    const bool ok =
        sound && _callbacks.onPlaySound && _callbacks.onPlaySound(sound);
    result = ok ? String("Played sound: ") + sound : "Unknown sound";
  } else if (strcmp(name, "play_melody") == 0) {
    const char *melody = doc["args"]["notes"];
    const bool ok =
        melody && _callbacks.onPlayMelody && _callbacks.onPlayMelody(melody);
    result = ok ? "Melody played" : "Invalid melody";
  } else if (strcmp(name, "power_off") == 0) {
    if (!_callbacks.onPowerOff) {
      result = "Power off unavailable";
      sendToolResponse(name, id, result);
      return;
    }

    sendToolResponse(name, id, "Powering off");
    delay(100);
    _callbacks.onPowerOff();
    return;
  }

  sendToolResponse(name, id, result);
}

void LiveSessionService::sendToolResponse(const char *name, const char *id,
                                          const String &result) {
  JsonDocument response;
  response["type"] = "tool_response";
  response["name"] = name;
  response["id"] = id;
  response["result"] = result;

  String encoded;
  serializeJson(response, encoded);
  _ws.send(encoded.c_str());
  Serial.printf("[Tool] %s -> %s\n", name, result.c_str());
}

String LiveSessionService::endpointBaseUrl(const ServerEndpoint &endpoint) const {
  const char *scheme = endpoint.port == 443 ? "https" : "http";
  String url = String(scheme) + "://" + endpoint.host;
  if (endpoint.port != 80 && endpoint.port != 443) {
    url += ":" + String(endpoint.port);
  }
  return url;
}
