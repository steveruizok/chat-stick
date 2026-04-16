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

  _settings.init();

  _display.init();
  _display.setBrightness(_settings.brightness());

  _powerManager.setSavedBrightness(_settings.brightness());
  configureCallbacks();

  _wifi.init();

  if (!_audio.init()) {
    setErrorState(ErrorCategory::Startup, "Startup failed",
                  "Audio buffer unavailable");
    renderIfNeeded();
    return;
  }

  _audio.setVolume(_settings.volume());
  _chatId = _settings.chatId();
  _live.setChatId(_chatId);

  LiveSessionCallbacks callbacks;
  callbacks.onActivity = [this]() { _powerManager.registerActivity(); };
  callbacks.onStatus = [this](const String &status) {
    if (_appState != AppState::Error) {
      setAppState(AppState::Connecting, status);
    }
  };
  callbacks.onReady = [this]() { setAppState(AppState::Ready, "Ready"); };
  callbacks.onTurnComplete = [this]() { _turnComplete = true; };
  callbacks.onChatId = [this](const String &chatId) {
    _chatId = chatId;
    _settings.setChatId(chatId);
    _live.setChatId(chatId);
    _screenDirty = true;
  };
  callbacks.onShowText = [this](const String &text) {
    _toolText = text;
    _screenDirty = true;
  };
  callbacks.onError = [this](const String &category, const String &error) {
    const ErrorCategory mapped = category == "gemini_unavailable"
                                     ? ErrorCategory::GeminiUnavailable
                                     : ErrorCategory::ServerRefused;
    setErrorState(mapped, "Server error", error);
  };
  callbacks.onIgnoredAudio = [this](const String &reason) {
    _turnComplete = false;
    _audio.stopPlayback();
    setAppState(AppState::Ready, "Ready");
    _toolText =
        reason == "silent" ? "Ignored silent clip" : "Ignored short clip";
    _screenDirty = true;
  };
  callbacks.onAudio = [this](const uint8_t *data, size_t len) {
    if (_appState != AppState::Recording) {
      _audio.queuePlayback(data, len);
    }
  };
  callbacks.onBrightness = [this](int level) {
    _display.setBrightness(level);
    _powerManager.setSavedBrightness(level);
    _settings.setBrightness(level);
    _screenDirty = true;
  };
  callbacks.onVolume = [this](int level) {
    _audio.setVolume(level);
    _settings.setVolume(level);
  };
  callbacks.onPlaySound = [this](const String &sound) {
    _powerManager.registerActivity();
    return _audio.playNamedSound(sound);
  };
  callbacks.onPlayMelody = [this](const String &notes) {
    _powerManager.registerActivity();
    return _audio.playMelody(notes);
  };
  callbacks.onPowerOff = [this]() {
    _live.disconnect();
    _wifi.disconnect();
    delay(100);
    M5.Power.powerOff();
  };
  callbacks.onPowerTimeouts =
      [this](unsigned long dimMs, unsigned long screenOffMs,
             unsigned long lightSleepMs, unsigned long powerOffMs) {
        _powerManager.setTimeouts({.dimMs = dimMs,
                                   .screenOffMs = screenOffMs,
                                   .lightSleepMs = lightSleepMs,
                                   .powerOffMs = powerOffMs});
      };
  callbacks.getDeviceStatusJson = [this]() { return deviceStatusJson(); };

  _live.init(callbacks);

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
    setErrorState(ErrorCategory::WiFiTimeout, "WiFi failed",
                  "Timed out on known networks");
    return;
  }

  restoreSessionPreview();
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
  if (state != AppState::Error) {
    _errorCategory = ErrorCategory::None;
  }
  if (!status.isEmpty()) {
    _statusText = status;
  }
  _errorText = error;
  _screenDirty = true;
}

void AppController::setErrorState(ErrorCategory category, const String &status,
                                  const String &error) {
  _errorCategory = category;
  _appState = AppState::Error;
  _statusText = status;
  _errorText = error;
  _screenDirty = true;
}

void AppController::clearToolText() { _toolText = ""; }

void AppController::restoreSessionPreview() {
  if (_chatId.isEmpty()) {
    return;
  }

  String lastMessage;
  if (_live.fetchLastAssistantMessage(lastMessage) && !lastMessage.isEmpty()) {
    _toolText = lastMessage;
    _statusText = "Restored";
    _screenDirty = true;
  }
}

void AppController::handleButtons() {
  if (_appState == AppState::ConfirmReset) {
    if (M5.BtnA.wasPressed()) {
      beginFactoryReset();
      return;
    }

    if (M5.BtnB.wasPressed()) {
      _appState = _resetReturnState;
      _statusText = _resetReturnStatus;
      _errorText = _resetReturnError;
      _errorCategory = _resetReturnCategory;
      _screenDirty = true;
      return;
    }
  }

  if (M5.BtnA.isPressed() && M5.BtnB.isPressed()) {
    if (_resetHoldStartMs == 0) {
      _resetHoldStartMs = millis();
    } else if (millis() - _resetHoldStartMs >= kResetHoldMs &&
               _appState != AppState::Recording &&
               _appState != AppState::ConfirmReset) {
      _resetReturnState = _appState;
      _resetReturnStatus = _statusText;
      _resetReturnError = _errorText;
      _resetReturnCategory = _errorCategory;
      setAppState(AppState::ConfirmReset, "Factory reset?");
      _errorText = "A confirm  B cancel";
      _screenDirty = true;
      return;
    }
  } else {
    _resetHoldStartMs = 0;
  }

  if (M5.BtnA.wasPressed()) {
    if (_powerManager.isInterruptible()) {
      _powerManager.beginWaking();
      _screenDirty = true;
      return;
    }

    _powerManager.registerActivity();
    if (_appState == AppState::Recording && _recordStopPending) {
      _recordStopPending = false;
      _statusText = "Listening...";
      _screenDirty = true;
      return;
    }
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
      _recordStopPending = true;
      _recordStopDeadlineMs = millis() + kRecordingGraceMs;
      _statusText = "Release A to send";
      _screenDirty = true;
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
  _recordingStartMs = millis();
  _recordStopPending = false;
  _recordStopDeadlineMs = 0;
  _live.sendStart();
  setAppState(AppState::Recording, "Listening...");
}

void AppController::stopRecording() {
  Serial.printf("[Rec] === STOP RECORDING === (sent %d chunks)\n",
                _audioChunksSent);
  _audio.stopRecording();
  _recordStopPending = false;
  _recordStopDeadlineMs = 0;
  _live.sendStop();
  _thinkingStartMs = millis();
  setAppState(AppState::Thinking, "Thinking...");
}

void AppController::processRecording() {
  if (_appState != AppState::Recording) {
    return;
  }

  if (millis() - _recordingStartMs >= kMaxRecordingMs) {
    Serial.println("[Rec] Max recording time reached");
    stopRecording();
    return;
  }

  if (_recordStopPending && millis() >= _recordStopDeadlineMs) {
    stopRecording();
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
  _screenDirty = true;
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
  state.showRecordingProgress = _appState == AppState::Recording;
  state.recordingProgress = recordingProgress();
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
    if (_recordStopPending) {
      return "Listening\nRelease A to send\nPress A to keep going";
    }
    return "Listening\nRelease A to send";

  case AppState::Thinking:
    return "Thinking\nWaiting for reply";

  case AppState::Playing:
    return "Speaking\nPress A to interrupt";

  case AppState::ConfirmReset:
    return "Factory reset?\nA confirm\nB cancel";

  case AppState::Error:
    if (_errorText.isEmpty()) {
      return String(errorCategoryLabel()) + "\n" + _statusText;
    }
    return String(errorCategoryLabel()) + "\n" + _statusText + "\n" +
           _errorText;
  }

  return "";
}

float AppController::recordingProgress() const {
  if (_appState != AppState::Recording || kMaxRecordingMs == 0) {
    return 0.0f;
  }

  return static_cast<float>(millis() - _recordingStartMs) /
         static_cast<float>(kMaxRecordingMs);
}

String AppController::deviceStatusJson() const {
  JsonDocument status;
  status["battery_percent"] = M5.Power.getBatteryLevel();
  status["volume"] = _audio.volume();
  status["brightness"] = M5.Display.getBrightness();
  status["wifi_network"] = _wifi.isConnected() ? _wifi.ssid() : "disconnected";
  status["uptime_seconds"] = millis() / 1000;
  status["power_timeouts"]["dim_ms"] = _powerManager.timeouts().dimMs;
  status["power_timeouts"]["screen_off_ms"] =
      _powerManager.timeouts().screenOffMs;
  status["power_timeouts"]["light_sleep_ms"] =
      _powerManager.timeouts().lightSleepMs;
  status["power_timeouts"]["power_off_ms"] =
      _powerManager.timeouts().powerOffMs;

  String json;
  serializeJson(status, json);
  return json;
}

void AppController::beginFactoryReset() {
  Serial.println("[Reset] Clearing device preferences");
  _live.disconnect();
  _wifi.reset();
  _settings.reset();
  delay(100);
  ESP.restart();
}

const char *AppController::errorCategoryLabel() const {
  switch (_errorCategory) {
  case ErrorCategory::Startup:
    return "Startup";
  case ErrorCategory::WiFiTimeout:
    return "WiFi timeout";
  case ErrorCategory::ServerRefused:
    return "Server refused";
  case ErrorCategory::GeminiUnavailable:
    return "Gemini down";
  case ErrorCategory::None:
  default:
    return "Error";
  }
}
