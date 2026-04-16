#include "AppController.h"

#include "../Config.h"
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <WiFi.h>

void AppController::setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n\n=== M5 Live Voice Assistant ===");
  Serial.flush();

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);

  setCpuFrequencyMhz(80);
  Serial.printf("[Setup] CPU clock set to %lu MHz\n", getCpuFrequencyMhz());

  _display.init();
  _display.setBrightness(DEFAULT_BRIGHTNESS);

  _powerManager.setSavedBrightness(DEFAULT_BRIGHTNESS);
  configureCallbacks();

  _wifi.init();

  if (!_audio.init()) {
    setAppState(AppState::Error, "Startup failed", "PSRAM alloc failed");
    renderIfNeeded();
    return;
  }

  _live.init({
      .onActivity = [this]() { _powerManager.registerActivity(); },
      .onStatus =
          [this](const String &status) {
            if (_appState != AppState::Error) {
              setAppState(AppState::Connecting, status);
            }
          },
      .onReady =
          [this]() { setAppState(AppState::Ready, "Ready"); },
      .onTurnComplete = [this]() { _turnComplete = true; },
      .onChatId =
          [this](const String &chatId) {
            _chatId = chatId;
            _screenDirty = true;
          },
      .onShowText =
          [this](const String &text) {
            _toolText = text;
            _screenDirty = true;
          },
      .onError =
          [this](const String &error) {
            setAppState(AppState::Error, "Server error", error);
          },
      .onAudio =
          [this](const uint8_t *data, size_t len) {
            if (_appState != AppState::Recording) {
              _audio.queuePlayback(data, len);
            }
          },
      .onBrightness = [this](int level) {
        _display.setBrightness(level);
        _powerManager.setSavedBrightness(level);
        _screenDirty = true;
      },
      .onVolume = [this](int level) { _audio.setVolume(level); },
      .getDeviceStatusJson = [this]() { return deviceStatusJson(); },
  });

  Serial.printf("Capture: %d Hz, %d ms chunks (%u bytes)\n", MIC_SAMPLE_RATE,
                MIC_CHUNK_MS, static_cast<unsigned>(_audio.captureBytes()));
  Serial.printf("Playback: %d Hz, max %d s\n", PLAY_SAMPLE_RATE,
                MAX_PLAYBACK_SEC);

  connectNetworkStack();
  renderIfNeeded();
}

void AppController::loop() {
  M5.update();

  if (millis() - _lastHeartbeatMs > 3000) {
    _lastHeartbeatMs = millis();
    Serial.printf("[Loop] state=%d power=%s ws=%d sleep=%d\n",
                  static_cast<int>(_appState),
                  powerStateName(_powerManager.getState()), _live.isConnected(),
                  _powerManager.getState() == PowerState::LightSleep);
  }

  if (millis() - _lastHeaderRefreshMs > 30000) {
    _lastHeaderRefreshMs = millis();
    _screenDirty = true;
  }

  _live.poll();
  _live.reconnectIfNeeded(_wifi.isConnected() &&
                          _powerManager.getState() != PowerState::LightSleep &&
                          _appState != AppState::Error);

  handleButtons();
  processRecording();
  processPlayback();
  processThinkingTimeout();
  processPower();
  renderIfNeeded();
  delay(1);
}

void AppController::configureCallbacks() {
  _powerManager.onBrightnessChange(
      [this](int brightness) { _display.setBrightness(brightness); });

  _powerManager.onWiFiStateChange(
      [this](bool enabled) { setNetworkEnabled(enabled); });

  _powerManager.onPowerOff([this]() {
    Serial.println("[Power] Powering off");
    _live.disconnect();
    _wifi.disconnect();
    delay(100);
    M5.Power.powerOff();
  });
}

void AppController::connectNetworkStack() {
  setAppState(AppState::Connecting, "Connecting...");
  if (!_wifi.connectKnownNetworks()) {
    setAppState(AppState::Error, "WiFi failed", "No known network");
    return;
  }

  _screenDirty = true;
  _live.connect();
}

void AppController::setNetworkEnabled(bool enabled) {
  if (enabled) {
    setAppState(AppState::Connecting, "Waking...");
    connectNetworkStack();
    return;
  }

  _live.disconnect();
  _wifi.disconnect();
}

void AppController::setAppState(AppState state, const String &status,
                                const String &error) {
  if (_appState != state) {
    clearToolText();
  }

  _appState = state;
  if (!status.isEmpty()) {
    _statusText = status;
  }
  _errorText = error;
  _screenDirty = true;
}

void AppController::clearToolText() { _toolText = ""; }

void AppController::handleButtons() {
  if (M5.BtnA.wasPressed()) {
    if (_powerManager.isInterruptible()) {
      _powerManager.beginWaking();
      _screenDirty = true;
      return;
    }

    _powerManager.registerActivity();
    if (_appState == AppState::Ready || _appState == AppState::Playing ||
        _appState == AppState::Thinking) {
      startRecording();
    }
  }

  if (M5.BtnA.wasReleased()) {
    if (_powerManager.isWaking()) {
      _powerManager.finishWaking();
      _screenDirty = true;
      return;
    }

    if (_appState == AppState::Recording) {
      stopRecording();
    }
  }

  if (M5.BtnB.wasPressed()) {
    if (_powerManager.isInterruptible()) {
      _powerManager.beginWaking();
      _screenDirty = true;
      return;
    }

    _powerManager.registerActivity();
    clearToolText();
    _screenDirty = true;
  }

  if (M5.BtnB.wasReleased() && _powerManager.isWaking()) {
    _powerManager.finishWaking();
    _screenDirty = true;
  }
}

void AppController::startRecording() {
  Serial.println("[Rec] === START RECORDING ===");
  _powerManager.registerActivity();
  clearToolText();
  _audio.startRecording();
  _turnComplete = false;
  _audioChunksSent = 0;
  _live.sendStart();
  setAppState(AppState::Recording, "Listening...");
}

void AppController::stopRecording() {
  Serial.printf("[Rec] === STOP RECORDING === (sent %d chunks)\n",
                _audioChunksSent);
  _audio.stopRecording();
  _live.sendStop();
  _thinkingStartMs = millis();
  setAppState(AppState::Thinking, "Thinking...");
}

void AppController::processRecording() {
  if (_appState != AppState::Recording) {
    return;
  }

  if (!_audio.captureChunk()) {
    Serial.println("[Rec] Mic.record() returned false");
    return;
  }

  const bool sent =
      _live.sendAudio(_audio.captureData(), _audio.captureBytes());
  _powerManager.registerActivity();
  _audioChunksSent++;
  if (_audioChunksSent <= 5 || _audioChunksSent % 10 == 0) {
    Serial.printf("[Rec] #%d sent=%d\n", _audioChunksSent, sent);
  }
}

void AppController::processPlayback() {
  if (_appState != AppState::Thinking && _appState != AppState::Playing) {
    return;
  }

  const int buffered = _audio.bufferedPlaybackBytes();
  if (!_audio.playbackStarted() && buffered >= kMinPlaybackBytes) {
    _audio.markPlaybackStarted();
    setAppState(AppState::Playing, "Speaking...");
    _audio.advancePlayback();
  }

  if (_audio.playbackStarted()) {
    _audio.advancePlayback();
  }

  if (_turnComplete && _audio.playbackIdle()) {
    _turnComplete = false;
    _audio.stopPlayback();
    setAppState(AppState::Ready, "Ready");
  }
}

void AppController::processThinkingTimeout() {
  if (_appState != AppState::Thinking) {
    return;
  }

  if (millis() - _thinkingStartMs > kThinkingTimeoutMs) {
    Serial.println("[Loop] Thinking timeout");
    _turnComplete = false;
    _audio.stopPlayback();
    setAppState(AppState::Ready, "Ready");
  }
}

void AppController::processPower() {
  if (_appState == AppState::Ready) {
    _powerManager.update();
  }
}

void AppController::renderIfNeeded() {
  if (!_screenDirty) {
    return;
  }

  _screenDirty = false;
  _display.render(buildDisplayState());
}

DisplayState AppController::buildDisplayState() const {
  DisplayState state;
  state.appState = _appState;
  state.headerLeft = _live.activeEndpointLabel();

  const int battery = M5.Power.getBatteryLevel();
  if (battery >= 0 && battery <= 100) {
    state.headerRight = String(battery) + "%";
  }

  state.bodyText = buildBodyText();
  state.footerLeft = _wifi.isConnected() ? _wifi.ssid() : "offline";
  state.footerRight = _chatId.isEmpty() ? "" : _chatId.substring(0, 8);
  return state;
}

String AppController::buildBodyText() const {
  if (!_toolText.isEmpty()) {
    return _toolText;
  }

  switch (_appState) {
  case AppState::Connecting:
    if (_wifi.isConnected()) {
      return _statusText + "\n" + _wifi.ssid() + "\n" + _wifi.localIp();
    }
    return _statusText + "\nSearching WiFi";

  case AppState::Ready:
    return "Ready\nHold A to talk";

  case AppState::Recording:
    return "Listening\nRelease A to send";

  case AppState::Thinking:
    return "Thinking\nWaiting for reply";

  case AppState::Playing:
    return "Speaking\nPress A to interrupt";

  case AppState::Error:
    if (_errorText.isEmpty()) {
      return _statusText;
    }
    return _statusText + "\n" + _errorText;
  }

  return "";
}

String AppController::deviceStatusJson() const {
  JsonDocument status;
  status["battery_percent"] = M5.Power.getBatteryLevel();
  status["volume"] = _audio.volume();
  status["brightness"] = M5.Display.getBrightness();
  status["wifi_network"] = _wifi.isConnected() ? _wifi.ssid() : "disconnected";
  status["uptime_seconds"] = millis() / 1000;

  String json;
  serializeJson(status, json);
  return json;
}
