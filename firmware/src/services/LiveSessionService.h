#pragma once

#include "../Config.h"
#include <ArduinoJson.h>
#include <Arduino.h>
#include <ArduinoWebsockets.h>
#include <functional>

struct LiveSessionCallbacks {
  std::function<void()> onActivity;
  std::function<void(const String &)> onStatus;
  std::function<void()> onReady;
  std::function<void()> onTurnComplete;
  std::function<void(const String &)> onChatId;
  std::function<void(const String &)> onShowText;
  std::function<void(const String &)> onError;
  std::function<void(const String &)> onIgnoredAudio;
  std::function<void(const uint8_t *, size_t)> onAudio;
  std::function<void(int)> onBrightness;
  std::function<void(int)> onVolume;
  std::function<bool(const String &)> onPlaySound;
  std::function<bool(const String &)> onPlayMelody;
  std::function<void()> onPowerOff;
  std::function<String()> getDeviceStatusJson;
};

class LiveSessionService {
public:
  void init(const LiveSessionCallbacks &callbacks);
  void connect();
  void disconnect();
  void poll();
  void reconnectIfNeeded(bool enabled);
  void setChatId(const String &chatId) { _chatId = chatId; }

  bool isConnected() const { return _connected; }
  int activeServerIndex() const { return _activeServerIndex; }
  String activeEndpointLabel() const;

  bool sendStart();
  bool sendStop();
  bool sendAudio(const int16_t *data, size_t len);
  bool fetchLastAssistantMessage(String &outMessage);

private:
  websockets::WebsocketsClient _ws;
  LiveSessionCallbacks _callbacks;
  String _chatId;
  bool _connected = false;
  unsigned long _lastReconnectMs = 0;
  int _nextServerIndex = 0;
  int _activeServerIndex = -1;

  static constexpr unsigned long kReconnectMs = 5000;

  void handleMessage(websockets::WebsocketsMessage msg);
  void handleEvent(websockets::WebsocketsEvent event, String data);
  void handleToolCall(const ArduinoJson::JsonDocument &doc);
  void sendToolResponse(const char *name, const char *id, const String &result);
  String endpointBaseUrl(const ServerEndpoint &endpoint) const;
};
