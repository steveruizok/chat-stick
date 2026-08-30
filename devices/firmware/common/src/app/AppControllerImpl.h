#include "AppController.h"

#include "Config.h"
#include "diag/Log.h"
#include "hal/Board.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <string.h>
#include <time.h>

namespace {
/**
 * @brief Human-readable label for a deep-sleep wake reason.
 * @param reason Wake reason to describe.
 * @return Static wake reason name.
 */
const char *deepSleepWakeReasonName(DeepSleepWakeReason reason) {
  switch (reason) {
  case DeepSleepWakeReason::None:
    return "none";
  case DeepSleepWakeReason::Button:
    return "button";
  case DeepSleepWakeReason::Timer:
    return "timer";
  case DeepSleepWakeReason::Other:
  default:
    return "other";
  }
}
} // namespace

void AppController::setup() {
  Serial.begin(115200);
  if (Board::capabilities().bootDisplay) {
    Log::setSink(&AppController::bootLogTrampoline, this);
  }
  Log::client("Boot", "%s Live Voice Assistant", FIRMWARE_DEVICE);
  Serial.flush();

  Board::init();
  setCpuFrequencyMhz(CPU_ACTIVE_MHZ);
  Log::client("Setup", "CPU clock set to %lu MHz", getCpuFrequencyMhz());

  setenv("TZ", LOCAL_TZ, 1);
  tzset();

  _settings.init();
  _pendingFirmwareUpdateAtBoot = _settings.pendingFirmwareUpdate();
  _timers.init();

  const DeepSleepWakeReason wakeReason = Board::deepSleepWakeReason();
  if (wakeReason != DeepSleepWakeReason::None) {
    Log::client("Boot", "wake from deep sleep cause=%s",
                deepSleepWakeReasonName(wakeReason));
  }
  if (shouldPowerOffAfterIdleDeepSleep(wakeReason)) {
    Log::client("Power", "idle deep sleep window elapsed; powering off");
    shutdownHardware();
    while (true) {
      delay(1000);
    }
  }

  _display.init();
  _display.setBrightness(_settings.brightness());
  _displayReady = true;
  _startupPowerDone = true;
  _screenDirty = true;
  renderIfNeeded();

  _powerManager.setSavedBrightness(_settings.brightness());
  configureCallbacks();

  _wifi.init();

  if (Board::capabilities().externalSpeakerGain) {
    _audio.setExternalSpeakerGain(_settings.externalSpeakerGain());
  }
  if (Board::capabilities().externalSpeakerSwitch) {
    _audio.setUseExternalSpeaker(_settings.useExternalSpeaker());
  }
  if (!_audio.init()) {
    setErrorState(ErrorCategory::Startup, "Startup failed",
                  "Audio buffer unavailable");
    renderIfNeeded();
    return;
  }

  _audio.setVolume(_settings.volume());
  if (!_settings.chatId().isEmpty()) {
    _settings.clearChatId();
  }
  _chatId = "";
  _live.setChatId("");
  _live.setVoice(_settings.voice());
  if (Board::capabilities().endpointPreference) {
    _live.setPreferredEndpointIndex(_settings.serverEndpointIndex());
  }
  _wifi.onLog([](const char *topic, const char *message) {
    Log::client(topic, "%s", message);
  });

  LiveSessionCallbacks callbacks;
  callbacks.onLog = [](char side, const char *topic, const char *message) {
    if (side == 'S') {
      Log::server(topic, "%s", message);
    } else {
      Log::client(topic, "%s", message);
    }
  };
  callbacks.onActivity = [this]() { _powerManager.registerActivity(); };
  callbacks.onStatus = [this](const String &status) {
    if (_appState != AppState::Error) {
      setAppState(AppState::Connecting, status);
    }
  };
  callbacks.onServerReady = [this]() {
    exitBootMode();
    if (_appState == AppState::Connecting) {
      handleInternetReady();
    }
  };
  callbacks.onReady = [this]() {
    exitBootMode();
    if (_appState == AppState::Connecting) {
      handleInternetReady();
    }
  };
  callbacks.onTurnComplete = [this]() {
    // Ignore turnComplete signals that arrive before any audio for the current
    // turn — they're stale from a prior turn that the user interrupted.
    _turn.noteTurnComplete();
  };
  callbacks.onDropAudio = [this]() {
    // Gemini detected a user interrupt mid-response. Flush any queued tail of
    // the prior turn so it doesn't play into the new one. Don't change state —
    // the user is mid-recording; release will drive the next transition.
    _audio.stopPlayback();
    _turn.clearResponse();
  };
  callbacks.onChatId = [this](const String &chatId) {
    _chatId = chatId;
    _settings.setChatId(chatId);
    _live.setChatId(chatId);
    _screenDirty = true;
  };
  callbacks.onConversationReset = [this]() {
    _audio.stopPlayback();
    setToolTextImmediate("");
    if (_imagePresent) {
      _display.clearImage();
      _imagePresent = false;
    }
    _turn.clearResponse();
    _turn.clearPendingReset();
    resetBodyPage();
    setAppState(AppState::Ready, "Ready");
    _screenDirty = true;
  };
  callbacks.onShowText = [this](const String &text) {
    _turn.clearPendingReset();
    startToolTextReveal(text);
  };
  auto applyPendingTurnReset = [this]() {
    if (!_turn.pendingReset()) {
      return;
    }
    setToolTextImmediate("");
    if (_imagePresent) {
      _display.clearImage();
      _imagePresent = false;
    }
    _turn.clearPendingReset();
  };
  callbacks.onShowImagePending = [this, applyPendingTurnReset]() {
    applyPendingTurnReset();
    _screenDirty = true;
  };
  callbacks.onShowImage = [this, applyPendingTurnReset](
                              const uint8_t *packed, size_t packedLen,
                              int width, int height) {
    applyPendingTurnReset();
    if (_display.setImage(packed, packedLen, width, height)) {
      _imagePresent = true;
      resetBodyPage();
      _screenDirty = true;
    }
  };
  callbacks.onShowImageFailed = [this]() {
    if (_imagePresent) {
      _display.clearImage();
      _imagePresent = false;
      resetBodyPage();
      _screenDirty = true;
    }
  };
  callbacks.onTranscript = [this, applyPendingTurnReset](
                               const String &source, const String &text) {
    if (source != "model") {
      return;
    }
    applyPendingTurnReset();
    const String basis =
        _toolTextRevealTarget.isEmpty() ? _toolText : _toolTextRevealTarget;
    const String delta = _turn.transcriptDelta(basis, text);
    if (delta.isEmpty()) {
      return;
    }
    appendToolTextReveal(delta);
    serviceUi();
  };
  callbacks.onError = [this](const String &category, const String &error) {
    const ErrorCategory mapped = category == "gemini_unavailable"
                                     ? ErrorCategory::GeminiUnavailable
                                     : ErrorCategory::ServerRefused;
    setErrorState(mapped, "Server error", error);
  };
  callbacks.onIgnoredAudio = [this](const String &reason) {
    Log::client("Recording",
                "ignored by server reason=%s chunks=%d failed=%d bytes=%u "
                "capture_failures=%d",
                reason.c_str(), _turn.audioChunksSent(),
                _turn.audioChunksFailed(),
                static_cast<unsigned>(_turn.audioBytesSent()),
                _turn.captureFailures());
    _turn.clearResponse();
    _turn.clearPendingReset();
    _audio.stopPlayback();
    setDebugText(String("DBG ignored:") +
                 (reason.isEmpty() ? String("unknown") : reason));
    setAppState(AppState::Ready, "Ready");
  };
  callbacks.onAudio = [this](const uint8_t *data, size_t len) {
    if (_appState != AppState::Recording) {
      clearDebugText();
      _audio.queuePlayback(data, len);
      _turn.noteAudioReceived();
    }
    // Audio frames arrive back-to-back inside a single websocket poll, so the
    // main loop can be stalled here for the whole turn. Keep the reveal and
    // the screen moving from inside the burst.
    processPlayback();
    serviceUi();
  };
  callbacks.onBrightness = [this](int level) {
    _display.setBrightness(level);
    _powerManager.setSavedBrightness(level);
    _settings.setBrightness(level);
    _screenDirty = true;
  };
  callbacks.onVolume = [this](int level) {
    _audio.setVolume(level);
    // Persist what the audio service actually applied (it may clamp into a
    // calibrated window) so the stored value matches the device state.
    _settings.setVolume(_audio.volume());
  };
  callbacks.onSetSpeaker = [this](const String &mode) {
    if (!Board::capabilities().externalSpeakerSwitch) {
      return false;
    }
    if (mode.equalsIgnoreCase("internal")) {
      _settings.setUseExternalSpeaker(false);
      _audio.setUseExternalSpeaker(false);
      _screenDirty = true;
      return true;
    }
    if (mode.equalsIgnoreCase("external")) {
      _settings.setUseExternalSpeaker(true);
      _audio.setUseExternalSpeaker(true);
      _screenDirty = true;
      return true;
    }
    return false;
  };
  callbacks.onSetExternalGain = [this](int gain) {
    if (!Board::capabilities().externalSpeakerGain ||
        gain < SettingsStore::kMinExternalGain ||
        gain > SettingsStore::kMaxExternalGain) {
      return false;
    }
    _settings.setExternalSpeakerGain(gain);
    _audio.setExternalSpeakerGain(gain);
    return true;
  };
  callbacks.onPlaySound = [this](const String &sound) {
    _powerManager.registerActivity();
    return _audio.playNamedSound(sound);
  };
  callbacks.onPlayMelody = [this](const String &notes) {
    _powerManager.registerActivity();
    return _audio.playMelody(notes);
  };
  callbacks.onPowerOff = [this]() { performPowerOff(); };
  callbacks.onPowerTimeouts =
      [this](unsigned long dimMs, unsigned long screenOffMs,
             unsigned long lightSleepMs, unsigned long powerOffMs) {
        _powerManager.setTimeouts({.dimMs = dimMs,
                                   .screenOffMs = screenOffMs,
                                   .lightSleepMs = lightSleepMs,
                                   .powerOffMs = powerOffMs});
      };
  callbacks.getDeviceStatusJson = [this]() { return deviceStatusJson(); };
  callbacks.onVoiceChanged = [this](const String &voice) {
    _settings.setVoice(voice);
  };
  callbacks.onEndpointIndexChanged = [this](int endpointIndex) {
    if (Board::capabilities().endpointPreference) {
      _settings.setServerEndpointIndex(endpointIndex);
    }
  };
  callbacks.onSetTimer = [this](int duration, const String &name) {
    return handleSetTimerTool(duration, name);
  };
  callbacks.onListTimers = [this]() { return handleListTimersTool(); };
  callbacks.onCancelTimer = [this](const TimerRef &ref, bool all) {
    return handleCancelTimerTool(ref, all);
  };
  callbacks.onExtendTimer = [this](int delta, const TimerRef &ref) {
    return handleExtendTimerTool(delta, ref);
  };

  _live.init(callbacks);

  Log::client("Audio", "capture=%d Hz chunk=%d ms bytes=%u",
              MIC_SAMPLE_RATE, MIC_CHUNK_MS,
              static_cast<unsigned>(_audio.captureBytes()));
  Log::client("Audio", "playback=%d Hz max=%d s", PLAY_SAMPLE_RATE,
              MAX_PLAYBACK_SEC);

  renderIfNeeded();

  checkTimerExpiry();
  if (_appState != AppState::Alarm) {
    connectNetworkStack();
    renderIfNeeded();
  } else {
    _bootHadExpiredAlarm = true;
  }
}

void AppController::loop() {
  Board::update();

#ifdef FRAMEBUFFER_DEBUG_COMMANDS
  while (Serial.available()) {
    const int command = Serial.read();
    if (command == 'D') {
      _display.dumpFramebuffer(Serial);
    } else if (command == 'R') {
      Log::client("Display", "forced full repaint");
      _display.forceFullRepaint();
    }
  }
#endif

  processPlayback();

  const bool audioActive = _appState == AppState::Playing;

  if (!audioActive && millis() - _lastPowerPollMs > 3000) {
    _lastPowerPollMs = millis();
    const uint16_t vbat = Board::batteryVoltageMv();
    const uint16_t vin = Board::vbusVoltageMv();
    const int level = Board::batteryLevel();
    Log::client("Power", "vbat=%u mV level=%d vbus=%u src=%s heap=%uK",
                vbat, level, vin, Board::powerSourceLabel(),
                static_cast<unsigned>(ESP.getFreeHeap() / 1024));
  }

  if (!audioActive && millis() - _lastHeartbeatMs > 3000) {
    _lastHeartbeatMs = millis();
    Log::client("Loop", "state=%d region=%d power=%s ws=%d sleep=%d",
                static_cast<int>(_appState), static_cast<int>(_appRegion),
                powerStateName(_powerManager.getState()), _live.isConnected(),
                _powerManager.getState() == PowerState::LightSleep);
  }

  if (!audioActive && millis() - _lastHeaderRefreshMs > 30000) {
    _lastHeaderRefreshMs = millis();
    _screenDirty = true;
  }

  if (!audioActive) {
    _wifi.poll();
  }
  _live.poll();
  processPlayback();
  if (_appState != AppState::Playing) {
    _live.reconnectIfNeeded(_wifi.isConnected() &&
                            _powerManager.getState() !=
                                PowerState::LightSleep &&
                            _appState != AppState::Error);
  }

  checkTimerExpiry();
  handleButtons();
  if (_appState == AppState::Alarm) {
    serviceAlarmTrill();
    renderIfNeeded();
    delay(1);
    return;
  }
  processRecording();
  processThinkingTimeout();
  processTextReveal();
  processConnectingTimeout();
  processMenuFetches();
  processPower();
  processCaptivePortal();
  renderIfNeeded();
  if (_appState != AppState::Playing) {
    delay(1);
  }
}

void AppController::configureCallbacks() {
  _powerManager.onLog([](char side, const char *topic, const char *message) {
    if (side == 'S') {
      Log::server(topic, "%s", message);
    } else {
      Log::client(topic, "%s", message);
    }
  });

  _powerManager.onBrightnessChange(
      [this](int brightness) { _display.setBrightness(brightness); });

  _powerManager.onWiFiStateChange(
      [this](bool enabled) { setNetworkEnabled(enabled); });

  _powerManager.onPowerOff([this]() { performPowerOff(true); });
}

void AppController::connectNetworkStack() {
  _networkStackStarted = true;
  _appRegion = AppRegion::Chat;
  _startupChecklistVisible = true;
  _startupPowerDone = true;
  _startupWifiDone = false;
  _startupInternetDone = false;
  setAppState(AppState::Connecting, "Starting...");
  renderIfNeeded();

  if (!_wifi.connectKnownNetworks()) {
    _startupChecklistVisible = false;
    setErrorState(ErrorCategory::WiFiTimeout, "WiFi failed",
                  "A retry  B hold menu");
    return;
  }

  _startupWifiDone = true;
  _screenDirty = true;
  renderIfNeeded();

  _live.connect();
}

void AppController::handleInternetReady() {
  _startupInternetDone = true;
  _startupChecklistVisible = false;
  _appRegion = AppRegion::Chat;

  if (installPendingFirmwareUpdate()) {
    return;
  }

  setAppState(AppState::Ready, "Ready");
  startAutomaticFirmwareUpdateCheck();
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
  const unsigned long now = millis();
  _appState = state;
  _connectingSinceMs = state == AppState::Connecting ? now : 0;
  if (state != AppState::Connecting) {
    _startupChecklistVisible = false;
  }
  if (state == AppState::Thinking) {
    _lastWaitingIndicatorRefreshMs = now;
    _waitingIndicatorFrame = 0;
  } else {
    _lastWaitingIndicatorRefreshMs = 0;
    _waitingIndicatorFrame = 0;
  }
  if (state != AppState::Error) {
    _errorCategory = ErrorCategory::None;
  }
  if (!status.isEmpty()) {
    _statusText = status;
  }
  _errorText = error;
  resetBodyPage();
  _screenDirty = true;
}

void AppController::setErrorState(ErrorCategory category, const String &status,
                                  const String &error) {
  _startupChecklistVisible = false;
  _errorCategory = category;
  _appState = AppState::Error;
  _connectingSinceMs = 0;
  _statusText = status;
  _errorText = error;
  _appRegion = AppRegion::Chat;
  resetBodyPage();
  _screenDirty = true;
}

void AppController::retryAfterError() {
  switch (_errorCategory) {
  case ErrorCategory::Startup:
    ESP.restart();
    return;
  case ErrorCategory::WiFiTimeout:
  case ErrorCategory::ServerRefused:
  case ErrorCategory::GeminiUnavailable:
  case ErrorCategory::None:
  default:
    connectNetworkStack();
    return;
  }
}

void AppController::performPowerOff(bool allowIdleDeepSleep) {
  if (enterDeepSleepForTimerOrIdle(allowIdleDeepSleep)) {
    return;
  }
  shutdownHardware();
}

void AppController::shutdownHardware() {
  Log::client("Power", "powering off");
  _live.disconnect();
  _wifi.disconnect();
  _audio.stopPlayback();
  delay(100);
  Board::powerOff();
}

bool AppController::shouldPowerOffAfterIdleDeepSleep(
    DeepSleepWakeReason wakeReason) const {
  return IDLE_DEEP_SLEEP_ENABLED && wakeReason == DeepSleepWakeReason::Timer &&
         _timers.count() == 0;
}

bool AppController::enterDeepSleepForTimerOrIdle(
    bool includeIdleShutdownDeadline) {
  if (!IDLE_DEEP_SLEEP_ENABLED) {
    return false;
  }

  uint64_t sleepUs = 0;
  const char *reason = nullptr;

  if (_timers.count() == 0) {
    if (!includeIdleShutdownDeadline) {
      return false;
    }
    sleepUs =
        static_cast<uint64_t>(IDLE_DEEP_SLEEP_SHUTDOWN_SEC) * 1000000ULL;
    reason = "idle shutdown window";
  } else {
    const time_t now = time(nullptr);
    const time_t deadline = _timers.nextDeadline();
    if (now < TIMER_MIN_VALID_EPOCH || deadline == 0) {
      return false;
    }

    if (deadline <= now) {
      checkTimerExpiry();
      if (_appState == AppState::Alarm) {
        _powerManager.restoreActive();
        return true;
      }
      return false;
    }

    sleepUs = static_cast<uint64_t>(deadline - now) * 1000000ULL;
    reason = "next timer";
  }

  if (sleepUs == 0) {
    return false;
  }

  Log::client("Power", "deep sleep %llu us until %s",
              static_cast<unsigned long long>(sleepUs), reason);

  _live.disconnect();
  _wifi.disconnect();
  _audio.stopPlayback();
  Board::setDisplayBrightness(BRIGHTNESS_OFF);
  delay(100);

  Board::enterDeepSleep(sleepUs);
  return true;
}

void AppController::clearToolText() {
  setToolTextImmediate("");
  if (_imagePresent) {
    _display.clearImage();
    _imagePresent = false;
  }
  resetBodyPage();
}

void AppController::setToolTextImmediate(const String &text) {
  cancelToolTextReveal();
  _toolText = text;
  _toolTextRevealTarget = text;
  _toolTextRevealLayout = _display.layoutTextForReveal(text);
  _toolTextRevealIndex = static_cast<int>(_toolTextRevealLayout.length());
  resetBodyPage();
  _screenDirty = true;
}

void AppController::startToolTextReveal(const String &text) {
  _toolTextRevealTarget = text;
  rebuildToolTextRevealLayout();
  _toolText = "";
  _toolTextRevealIndex = 0;
  _lastTextRevealMs = 0;
  resetBodyPage();
  _screenDirty = true;
}

void AppController::appendToolTextReveal(const String &text) {
  if (text.isEmpty()) {
    return;
  }

  if (_toolTextRevealTarget.isEmpty() && !_toolText.isEmpty()) {
    _toolTextRevealTarget = _toolText;
  }
  _toolTextRevealTarget += text;
  rebuildToolTextRevealLayout();
  _screenDirty = true;
}

void AppController::rebuildToolTextRevealLayout() {
  _toolTextRevealLayout = _display.layoutTextForReveal(_toolTextRevealTarget);
  const int targetLen = static_cast<int>(_toolTextRevealLayout.length());
  _toolTextRevealIndex = constrain(_toolTextRevealIndex, 0, targetLen);
  _toolText = _toolTextRevealLayout.substring(0, _toolTextRevealIndex);
}

void AppController::completeToolTextReveal() {
  if (_toolTextRevealIndex >= static_cast<int>(_toolTextRevealLayout.length())) {
    return;
  }

  _toolTextRevealIndex = static_cast<int>(_toolTextRevealLayout.length());
  _toolText = _toolTextRevealLayout;
  _screenDirty = true;
}

void AppController::cancelToolTextReveal() {
  _toolTextRevealTarget = "";
  _toolTextRevealLayout = "";
  _toolTextRevealIndex = 0;
  _lastTextRevealMs = 0;
}

void AppController::setDebugText(const String &text) {
  if (_debugText == text) {
    return;
  }
  _debugText = text;
  _screenDirty = true;
}

void AppController::clearDebugText() {
  if (_debugText.isEmpty()) {
    return;
  }
  _debugText = "";
  _screenDirty = true;
}

void AppController::resetBodyPage() { _bodyPageIndex = 0; }

void AppController::bootLogTrampoline(void *ctx, char side, const char *topic,
                                      const char *message) {
  (void)side;
  static_cast<AppController *>(ctx)->appendBootLog(topic, message);
}

void AppController::appendBootLog(const char *topic, const char *message) {
  if (!_bootMode) {
    return;
  }

  String entry;
  entry.reserve(strlen(topic) + 2 + strlen(message));
  entry += topic;
  entry += ": ";
  entry += message;

  if (!_bootLog.isEmpty()) {
    _bootLog += '\n';
  }
  _bootLog += entry;

  // Drop oldest entries until the wrapped log fits on one body page.
  while (_displayReady &&
         _display.wrappedRowCount(_bootLog) > TextDisplay::kChatRows) {
    const int cut = _bootLog.indexOf('\n');
    if (cut < 0) {
      break;
    }
    _bootLog = _bootLog.substring(cut + 1);
  }

  _screenDirty = true;
  renderIfNeeded();
}

void AppController::exitBootMode() {
  if (!_bootMode) {
    return;
  }
  _bootMode = false;
  _bootLog = "";
  Log::clearSink();
  _screenDirty = true;
}

void AppController::restoreSessionPreview() {
  if (_chatId.isEmpty()) {
    return;
  }

  String lastMessage;
  if (_live.fetchLastAssistantMessage(lastMessage) && !lastMessage.isEmpty()) {
    setToolTextImmediate(lastMessage);
    _statusText = "Restored";
    resetBodyPage();
    _screenDirty = true;
  }
}

void AppController::clearButtonEvents() {
  _buttonA.clearEvents();
  _buttonB.clearEvents();
}

void AppController::handleButtons() {
  const unsigned long now = millis();
  _buttonA.update(Board::buttonAIsPressed(), now);
  _buttonB.update(Board::buttonBIsPressed(), now);

  if (_appState == AppState::ConfirmReset) {
    if (_buttonA.consumeClick()) {
      beginFactoryReset();
      return;
    }

    if (_buttonB.consumeClick()) {
      _appState = _resetReturnState;
      _statusText = _resetReturnStatus;
      _errorText = _resetReturnError;
      _errorCategory = _resetReturnCategory;
      _appRegion = AppRegion::Chat;
      _screenDirty = true;
      return;
    }
  }

  if (_appState == AppState::Alarm) {
    handleAlarmButtons();
    return;
  }

  if (_buttonA.isPressed() && _buttonB.isPressed()) {
    if (_resetHoldStartMs == 0) {
      _resetHoldStartMs = now;
    } else if (now - _resetHoldStartMs >= kResetHoldMs &&
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

  if (_powerManager.isInterruptible() &&
      (_buttonA.consumePressed() || _buttonB.consumePressed())) {
    completeToolTextReveal();
    _powerManager.beginWaking();
    _screenDirty = true;
  }

  if (_powerManager.isWaking()) {
    if (_buttonA.consumeReleased() || _buttonB.consumeReleased()) {
      _powerManager.finishWaking();
      _screenDirty = true;
    }
    _buttonA.clearEvents();
    _buttonB.clearEvents();
    return;
  }

  if (_appRegion == AppRegion::Menu) {
    handleMenuButtons();
    return;
  }

  handleChatButtons();
}

bool AppController::handleAlarmButtons() {
  if (_buttonA.consumePressed() || _buttonB.consumePressed() ||
      _buttonA.consumeClick() || _buttonB.consumeClick()) {
    _buttonA.clearEvents();
    _buttonB.clearEvents();
    exitAlarmState();
    return true;
  }
  return false;
}

void AppController::handleChatButtons() {
  if (_appState == AppState::Error && _buttonA.consumeClick()) {
    retryAfterError();
    return;
  }

  if (_buttonB.consumeHoldStart() && _appState != AppState::Recording) {
    completeToolTextReveal();
    openMenu(_appState == AppState::Error ? MenuState::Device : MenuState::Home);
    return;
  }

  if (_buttonB.consumeClick()) {
    _powerManager.registerActivity();
    completeToolTextReveal();
    const int pageCount = currentBodyPageCount();
    if (pageCount > 1) {
      _bodyPageIndex = (_bodyPageIndex + 1) % pageCount;
    } else if (!_toolText.isEmpty() || _imagePresent) {
      clearToolText();
    }
    _screenDirty = true;
  }

  if (_buttonA.consumePressed() &&
      (_appState == AppState::Ready || _appState == AppState::Playing ||
       _appState == AppState::Thinking)) {
    completeToolTextReveal();
    startRecording();
    return;
  }

  if (_buttonA.consumeReleased() && _appState == AppState::Recording) {
    stopRecording();
  }
}

void AppController::handleMenuButtons() {
  if (_buttonB.consumeClick()) {
    _powerManager.registerActivity();
    cycleMenuSelection();
    return;
  }

  if (_buttonB.consumeHoldStart()) {
    _powerManager.registerActivity();
    navigateBackFromMenu();
    return;
  }

  if (_buttonA.consumeClick()) {
    _powerManager.registerActivity();
    selectCurrentMenuItem();
  }
}

void AppController::openMenu(MenuState state) {
  completeToolTextReveal();
  if (_appState == AppState::Recording) {
    _audio.stopRecording();
    if (_live.isConnected() && _recordingCommitted) {
      _live.sendStop();
    }
    _turn.clearPendingReset();
    _recordingCommitted = false;
    _preCommitAudioChunkCount = 0;
    setAppState(AppState::Ready, "Ready");
  }
  _appRegion = AppRegion::Menu;
  _menuState = state;
  _menuSelection = 0;
  clearButtonEvents();
  if (state == MenuState::ResumeChat) {
    startConversationHistoryLoad();
  } else if (state == MenuState::Updates) {
    startFirmwareUpdateCheck();
  }
  _screenDirty = true;
}

void AppController::closeMenu() {
  _appRegion = AppRegion::Chat;
  _menuState = MenuState::Home;
  _menuSelection = 0;
  clearButtonEvents();
  _screenDirty = true;
}

void AppController::navigateBackFromMenu() {
  if (_menuState == MenuState::Home) {
    closeMenu();
    return;
  }

  if (_menuState == MenuState::Updates) {
    openMenu(MenuState::Device);
    return;
  }

  openMenu(MenuState::Home);
}

void AppController::cycleMenuSelection() {
  const int count = menuItemCount();
  if (count <= 0) {
    return;
  }
  _menuSelection = (_menuSelection + 1) % count;
  _screenDirty = true;
}

void AppController::selectCurrentMenuItem() {
  switch (_menuState) {
  case MenuState::Home:
    switch (_menuSelection) {
    case 0:
      closeMenu();
      return;
    case 1:
      closeMenu();
      startFreshConversation();
      return;
    case 2:
      openMenu(MenuState::ResumeChat);
      return;
    case 3:
      openMenu(MenuState::Device);
      return;
    default:
      return;
    }

  case MenuState::Device:
    if (_menuSelection == 0) {
      openMenu(MenuState::Home);
      return;
    }
    if (_menuSelection == 1) {
      closeMenu();
      startCaptivePortalFlow();
      return;
    }
    if (_menuSelection == 2) {
      openMenu(MenuState::Updates);
      return;
    }
    if (Board::capabilities().externalSpeakerSwitch && _menuSelection == 3) {
      const bool next = !_settings.useExternalSpeaker();
      _settings.setUseExternalSpeaker(next);
      _audio.setUseExternalSpeaker(next);
      _screenDirty = true;
      return;
    }
    if (_menuSelection ==
        (Board::capabilities().externalSpeakerSwitch ? 4 : 3)) {
      performPowerOff();
      return;
    }
    return;

  case MenuState::ResumeChat:
    if (_menuSelection == 0) {
      openMenu(MenuState::Home);
      return;
    }
    if (_historyLoadStatus == MenuLoadStatus::Loading) {
      return;
    }
    if (_historyCount == 0) {
      return;
    }
    resumeConversation(_menuSelection - 1);
    return;

  case MenuState::Updates:
    if (_menuSelection == 0) {
      openMenu(MenuState::Device);
      return;
    }
    if (_firmwareCheckStatus == MenuLoadStatus::Loading ||
        _firmwareCheckStatus == MenuLoadStatus::Idle) {
      return;
    }
    if (_menuSelection == 1 && _firmwareCheckStatus == MenuLoadStatus::Loaded &&
        _firmwareInfo.available && !_firmwareInfo.downloadUrl.isEmpty()) {
      installFirmwareUpdate();
    }
    return;
  }
}

int AppController::menuItemCount() const {
  switch (_menuState) {
  case MenuState::Home:
    return 4;
  case MenuState::Device:
    return Board::capabilities().externalSpeakerSwitch ? 5 : 4;
  case MenuState::ResumeChat:
    return _historyCount > 0 ? 1 + _historyCount : 2;
  case MenuState::Updates:
    if (_firmwareCheckStatus == MenuLoadStatus::Loaded &&
        _firmwareInfo.available && !_firmwareInfo.notes.isEmpty()) {
      return 3;
    }
    return 2;
  }
  return 0;
}

String AppController::menuTitle() const {
  switch (_menuState) {
  case MenuState::Home:
    return "Menu";
  case MenuState::Device:
    return "Device";
  case MenuState::ResumeChat:
    return "Resume";
  case MenuState::Updates:
    return "Updates";
  }
  return "";
}

String AppController::menuItemLabel(int index) const {
  switch (_menuState) {
  case MenuState::Home:
    switch (index) {
    case 0:
      return "Go back";
    case 1:
      return "New conversation";
    case 2:
      return "Resume chat";
    case 3:
      return "Device";
    default:
      return "";
    }

  case MenuState::Device:
    if (index == 0) {
      return "Go back";
    }
    if (index == 1) {
      return "Set up WiFi";
    }
    if (index == 2) {
      return "Check for updates";
    }
    if (Board::capabilities().externalSpeakerSwitch && index == 3) {
      return _settings.useExternalSpeaker() ? "Speaker: external"
                                            : "Speaker: internal";
    }
    if (index == (Board::capabilities().externalSpeakerSwitch ? 4 : 3)) {
      return "Turn off";
    }
    return "";

  case MenuState::ResumeChat:
    if (index == 0) {
      return "Go back";
    }
    if (_historyLoadStatus == MenuLoadStatus::Loading ||
        _historyLoadStatus == MenuLoadStatus::Idle) {
      return "Loading...";
    }
    if (_historyCount == 0 && index == 1) {
      return _historyLoadMessage.isEmpty() ? String("No saved chats")
                                           : _historyLoadMessage.substring(0, 26);
    }
    if (index - 1 < _historyCount) {
      const ConversationSummary &entry = _history[index - 1];
      const String preview =
          entry.lastMessage.isEmpty() ? entry.chatId : entry.lastMessage;
      return preview.substring(0, 26);
    }
    return "";

  case MenuState::Updates:
    if (index == 0) {
      return "Go back";
    }
    if (_firmwareCheckStatus == MenuLoadStatus::Loading ||
        _firmwareCheckStatus == MenuLoadStatus::Idle) {
      return "Checking...";
    }
    if (_firmwareCheckStatus == MenuLoadStatus::Failed) {
      return _firmwareCheckMessage.isEmpty()
                 ? String("Update check failed")
                 : _firmwareCheckMessage.substring(0, 26);
    }
    if (!_firmwareInfo.available) {
      return "Up to date v" + String(FIRMWARE_VERSION);
    }
    if (_firmwareInfo.downloadUrl.isEmpty()) {
      return "Update unavailable";
    }
    if (index == 1) {
      return "Install v" + String(_firmwareInfo.latestVersion);
    }
    return _firmwareInfo.notes.substring(0, 26);
  }

  return "";
}

void AppController::startConversationHistoryLoad() {
  _historyCount = 0;
  _historyLoadMessage = "";
  _historyLoadStatus = MenuLoadStatus::Loading;

  if (_historyFetchTask != nullptr) {
    return;
  }

  if (!_wifi.isConnected()) {
    _historyLoadStatus = MenuLoadStatus::Failed;
    _historyLoadMessage = "Connect WiFi first";
    return;
  }

  _historyFetchDone = false;
  _historyFetchOk = false;
  _historyFetchCount = 0;
  _historyFetchMessage = "";

  const BaseType_t ok =
      xTaskCreatePinnedToCore(&AppController::conversationHistoryLoadTaskTrampoline,
                              "menu_history", kMenuFetchTaskStack, this, 1,
                              &_historyFetchTask, 1);
  if (ok != pdPASS) {
    _historyFetchTask = nullptr;
    _historyLoadStatus = MenuLoadStatus::Failed;
    _historyLoadMessage = "History unavailable";
    Log::client("Menu", "failed to start history fetch task");
  }
}

void AppController::conversationHistoryLoadTaskTrampoline(void *ctx) {
  static_cast<AppController *>(ctx)->conversationHistoryLoadTask();
}

void AppController::conversationHistoryLoadTask() {
  int count = 0;
  _historyFetchOk = _live.fetchConversationHistory(
      _historyFetchResults, kMaxConversationHistory, count);
  _historyFetchCount = count;
  if (!_historyFetchOk) {
    _historyFetchMessage = "History unavailable";
  } else if (count == 0) {
    _historyFetchMessage = "No saved chats";
  } else {
    _historyFetchMessage = "";
  }
  _historyFetchDone = true;
  vTaskDelete(nullptr);
}

void AppController::finishConversationHistoryLoad() {
  _historyFetchTask = nullptr;
  _historyCount = 0;
  if (_historyFetchOk) {
    _historyLoadStatus = MenuLoadStatus::Loaded;
    _historyCount = min(_historyFetchCount, kMaxConversationHistory);
    for (int i = 0; i < _historyCount; i++) {
      _history[i] = _historyFetchResults[i];
    }
  } else {
    _historyLoadStatus = MenuLoadStatus::Failed;
  }

  _historyLoadMessage = _historyFetchMessage;
  if (_historyLoadMessage.isEmpty() && _historyCount == 0) {
    _historyLoadMessage = "No saved chats";
  }
  _screenDirty = true;
}

void AppController::resumeConversation(int index) {
  if (index < 0 || index >= _historyCount) {
    return;
  }

  const ConversationSummary &entry = _history[index];
  _chatId = entry.chatId;
  _settings.setChatId(_chatId);
  _live.setChatId(_chatId);
  setToolTextImmediate(entry.lastMessage);
  if (_imagePresent) {
    _display.clearImage();
    _imagePresent = false;
  }
  resetBodyPage();
  closeMenu();
  _live.disconnect();
  setAppState(AppState::Connecting, "Restoring...");
  _live.connect();
}

void AppController::startFreshConversation() {
  _chatId = "";
  _settings.clearChatId();
  _live.setChatId("");
  clearToolText();
  _live.disconnect();
  setAppState(AppState::Connecting, "New chat...");
  _live.connect();
}

void AppController::startCaptivePortalFlow() {
  _live.disconnect();
  if (_wifi.startCaptivePortal()) {
    setAppState(AppState::Connecting, "WiFi setup");
    setToolTextImmediate("Join AP\n" + _wifi.captivePortalSsid() + "\nOpen " +
                         _wifi.captivePortalIp() + "\nSubmit WiFi form");
  } else {
    setErrorState(ErrorCategory::WiFiTimeout, "Portal failed",
                  "Could not start setup AP");
  }
  resetBodyPage();
  _screenDirty = true;
}

void AppController::startFirmwareUpdateCheck() {
  _firmwareInfo = FirmwareUpdateInfo{};
  _firmwareCheckMessage = "";
  _firmwareCheckStatus = MenuLoadStatus::Loading;

  if (_firmwareFetchTask != nullptr) {
    _firmwareFetchReason = FirmwareCheckReason::Menu;
    return;
  }

  if (!_wifi.isConnected()) {
    _firmwareCheckStatus = MenuLoadStatus::Failed;
    _firmwareCheckMessage = "Offline";
    return;
  }

  _firmwareFetchDone = false;
  _firmwareFetchOk = false;
  _firmwareFetchInfo = FirmwareUpdateInfo{};
  _firmwareFetchMessage = "";
  _firmwareFetchReason = FirmwareCheckReason::Menu;

  const BaseType_t ok =
      xTaskCreatePinnedToCore(&AppController::firmwareUpdateCheckTaskTrampoline,
                              "menu_update", kMenuFetchTaskStack, this, 1,
                              &_firmwareFetchTask, 1);
  if (ok != pdPASS) {
    _firmwareFetchTask = nullptr;
    _firmwareFetchReason = FirmwareCheckReason::None;
    _firmwareCheckStatus = MenuLoadStatus::Failed;
    _firmwareCheckMessage = "Update check failed";
    Log::client("Menu", "failed to start firmware check task");
  }
}

void AppController::startAutomaticFirmwareUpdateCheck() {
  if (_automaticFirmwareCheckStarted || _settings.pendingFirmwareUpdate() ||
      _firmwareFetchTask != nullptr || !_wifi.isConnected()) {
    return;
  }

  _automaticFirmwareCheckStarted = true;
  _firmwareFetchDone = false;
  _firmwareFetchOk = false;
  _firmwareFetchInfo = FirmwareUpdateInfo{};
  _firmwareFetchMessage = "";
  _firmwareFetchReason = FirmwareCheckReason::Automatic;

  const BaseType_t ok =
      xTaskCreatePinnedToCore(&AppController::firmwareUpdateCheckTaskTrampoline,
                              "ota_check", kMenuFetchTaskStack, this, 1,
                              &_firmwareFetchTask, 1);
  if (ok != pdPASS) {
    _firmwareFetchTask = nullptr;
    _firmwareFetchReason = FirmwareCheckReason::None;
    _automaticFirmwareCheckStarted = false;
    Log::client("OTA", "failed to start background firmware check task");
  }
}

void AppController::firmwareUpdateCheckTaskTrampoline(void *ctx) {
  static_cast<AppController *>(ctx)->firmwareUpdateCheckTask();
}

void AppController::firmwareUpdateCheckTask() {
  _firmwareFetchInfo = FirmwareUpdateInfo{};
  _firmwareFetchOk = _live.checkFirmwareUpdate(_firmwareFetchInfo);
  if (!_firmwareFetchOk) {
    _firmwareFetchMessage = "Update check failed";
  } else if (_firmwareFetchInfo.available &&
             _firmwareFetchInfo.downloadUrl.isEmpty()) {
    _firmwareFetchMessage = "No download URL";
  } else {
    _firmwareFetchMessage = "";
  }
  _firmwareFetchDone = true;
  vTaskDelete(nullptr);
}

void AppController::finishFirmwareUpdateCheck() {
  const FirmwareCheckReason reason = _firmwareFetchReason;
  _firmwareFetchTask = nullptr;
  _firmwareFetchReason = FirmwareCheckReason::None;

  if (_firmwareFetchOk) {
    if (_firmwareFetchInfo.available && !_firmwareFetchInfo.downloadUrl.isEmpty() &&
        _firmwareFetchInfo.latestVersion > FIRMWARE_VERSION) {
      _settings.setPendingFirmwareUpdate(_firmwareFetchInfo.latestVersion,
                                         _firmwareFetchInfo.downloadUrl);
      Log::client("OTA", "deferred firmware update v%d",
                  _firmwareFetchInfo.latestVersion);
    } else {
      _settings.clearPendingFirmwareUpdate();
      Log::client("OTA", "no deferred firmware update available");
    }
  }

  if (reason != FirmwareCheckReason::Menu) {
    return;
  }

  if (_firmwareFetchOk) {
    _firmwareCheckStatus = MenuLoadStatus::Loaded;
    _firmwareInfo = _firmwareFetchInfo;
  } else {
    _firmwareCheckStatus = MenuLoadStatus::Failed;
    _firmwareInfo = FirmwareUpdateInfo{};
  }
  _firmwareCheckMessage = _firmwareFetchMessage;
  _screenDirty = true;
}

void AppController::installFirmwareUpdate() {
  installFirmwareUpdate(_firmwareInfo);
}

void AppController::installFirmwareUpdate(const FirmwareUpdateInfo &info) {
  if (!info.available || info.downloadUrl.isEmpty()) {
    return;
  }

  closeMenu();
  setAppState(AppState::Connecting, "Updating...");
  setToolTextImmediate("Downloading update\nv" + String(info.latestVersion) +
                       "\nPlease wait");
  renderIfNeeded();

  String error;
  if (_live.downloadAndApplyFirmwareUpdate(info.downloadUrl, error)) {
    setToolTextImmediate("Update installed\nRestarting...");
    renderIfNeeded();
    delay(500);
    ESP.restart();
  }

  setToolTextImmediate("Update failed\n" + error);
  setAppState(AppState::Ready, "Ready");
  _screenDirty = true;
}

bool AppController::installPendingFirmwareUpdate() {
  if (!_pendingFirmwareUpdateAtBoot || _pendingFirmwareInstallAttempted ||
      !_settings.pendingFirmwareUpdate()) {
    return false;
  }

  const int version = _settings.pendingFirmwareVersion();
  const String downloadUrl = _settings.pendingFirmwareDownloadUrl();
  if (version <= FIRMWARE_VERSION || downloadUrl.isEmpty()) {
    _settings.clearPendingFirmwareUpdate();
    return false;
  }

  _pendingFirmwareInstallAttempted = true;
  FirmwareUpdateInfo info;
  info.available = true;
  info.latestVersion = version;
  info.downloadUrl = downloadUrl;
  Log::client("OTA", "installing deferred firmware update v%d", version);
  installFirmwareUpdate(info);
  return true;
}

void AppController::startRecording() {
  Log::client("Recording", "start ws=%d state=%d", _live.isConnected(),
              static_cast<int>(_appState));
  if (_appRegion == AppRegion::Menu) {
    Log::client("Recording", "start rejected: menu open");
    return;
  }
  _powerManager.registerActivity();
  _audio.stopPlayback();
  clearDebugText();
  if (!_live.isConnected()) {
    Log::client("Recording", "start rejected: websocket disconnected");
    setDebugText("DBG websocket offline");
    setAppState(AppState::Ready, "Ready");
    return;
  }
  if (!_audio.startRecording()) {
    Log::client("Recording", "audio start failed");
    setDebugText("DBG mic start failed");
    setAppState(AppState::Ready, "Ready");
    return;
  }
  _turn.beginRecording(millis());
  _recordingCommitted = false;
  _preCommitAudioChunkCount = 0;
  setAppState(AppState::Recording, "Listening...");
  renderIfNeeded();
}

void AppController::stopRecording() {
  const unsigned long now = millis();
  const unsigned long durationMs = _turn.recordingDurationMs(now);
  Log::client("Recording",
              "stop duration=%lu sent=%d failed=%d bytes=%u "
              "capture_failures=%d",
              durationMs, _turn.audioChunksSent(), _turn.audioChunksFailed(),
              static_cast<unsigned>(_turn.audioBytesSent()),
              _turn.captureFailures());
  _audio.stopRecording();
  if (!_recordingCommitted) {
    discardRecording(durationMs < kRecordingCommitMs ? "short press"
                                                     : "uncommitted release");
    return;
  }
  if (_turn.audioChunksSent() == 0) {
    Log::client("Recording", "no audio chunks sent before stop");
    setDebugText("DBG no audio sent");
  }
  if (!_live.sendStop()) {
    Log::client("Recording", "stop send failed");
    setDebugText("DBG stop send fail");
    _turn.clearPendingReset();
    setAppState(AppState::Ready, "Ready");
    return;
  }
  _recordingCommitted = false;
  _preCommitAudioChunkCount = 0;
  _turn.beginThinking(millis());
  setAppState(AppState::Thinking, "Thinking...");
}

bool AppController::commitRecording() {
  if (_recordingCommitted) {
    return true;
  }

  if (!_live.sendStart()) {
    Log::client("Recording", "start send failed");
    setDebugText("DBG start send fail");
    _recordingCommitted = false;
    _preCommitAudioChunkCount = 0;
    _audio.stopRecording();
    setAppState(AppState::Ready, "Ready");
    return false;
  }

  _recordingCommitted = true;
  for (int i = 0; i < _preCommitAudioChunkCount; i++) {
    sendAudioChunk(_preCommitAudio + i * kCaptureChunkSamples,
                   kCaptureChunkSamples * sizeof(int16_t));
  }
  _preCommitAudioChunkCount = 0;
  return true;
}

void AppController::discardRecording(const String &reason) {
  Log::client("Recording", "discarding uncommitted recording: %s",
              reason.c_str());
  _turn.clearPendingReset();
  _turn.clearResponse();
  _recordingCommitted = false;
  _preCommitAudioChunkCount = 0;
  setAppState(AppState::Ready, "Ready");
}

void AppController::bufferPreCommitAudio(const int16_t *data) {
  if (!data) {
    return;
  }

  if (_preCommitAudioChunkCount >= kPreCommitAudioChunks) {
    memmove(_preCommitAudio, _preCommitAudio + kCaptureChunkSamples,
            (kPreCommitAudioChunks - 1) * kCaptureChunkSamples *
                sizeof(int16_t));
    _preCommitAudioChunkCount = kPreCommitAudioChunks - 1;
  }

  memcpy(_preCommitAudio + _preCommitAudioChunkCount * kCaptureChunkSamples,
         data, kCaptureChunkSamples * sizeof(int16_t));
  _preCommitAudioChunkCount++;
}

bool AppController::sendAudioChunk(const int16_t *data, size_t bytes) {
  const bool sent = _live.sendAudio(data, bytes);
  _powerManager.registerActivity();
  if (!sent) {
    const int failed = _turn.noteAudioSendFailed();
    Log::client("Recording", "audio chunk send failed count=%d ws=%d",
                failed, _live.isConnected());
    setDebugText("DBG audio send fail");
    return false;
  }

  const int sentCount = _turn.noteAudioChunkSent(bytes);
  if (sentCount <= 5 || sentCount % 10 == 0) {
    Log::client("Recording",
                "#%d sent bytes=%u total=%u avg_abs=%d peak=%d ch=%d "
                "l_avg=%d r_avg=%d",
                sentCount, static_cast<unsigned>(bytes),
                static_cast<unsigned>(_turn.audioBytesSent()),
                _audio.lastCaptureAverageAbs(), _audio.lastCapturePeak(),
                _audio.lastCaptureChannel(), _audio.lastCaptureLeftAverageAbs(),
                _audio.lastCaptureRightAverageAbs());
  }
  return true;
}

void AppController::processRecording() {
  if (_appState != AppState::Recording) {
    return;
  }

  if (_appRegion == AppRegion::Menu) {
    Log::client("Recording", "stopping capture because menu opened");
    _audio.stopRecording();
    if (_live.isConnected() && _recordingCommitted) {
      _live.sendStop();
    }
    discardRecording("menu opened");
    return;
  }

  if (_turn.recordingTimedOut(millis(), kMaxRecordingMs)) {
    Log::client("Recording", "max recording time reached");
    stopRecording();
    return;
  }

  if (!_audio.captureChunk()) {
    const unsigned long now = millis();
    const TurnController::CaptureFailureReport failure =
        _turn.noteCaptureFailure(now, kCaptureFailureLogIntervalMs);
    if (failure.shouldLog) {
      Log::client("Recording", "mic capture returned false count=%d",
                  failure.count);
    }
    if (failure.firstFailure) {
      setDebugText("DBG mic read false");
    }
    return;
  }

  if (!_recordingCommitted) {
    bufferPreCommitAudio(_audio.captureData());
    _powerManager.registerActivity();
    if (_turn.recordingDurationMs(millis()) < kRecordingCommitMs ||
        !Board::buttonAIsPressed()) {
      return;
    }
    commitRecording();
    return;
  }

  sendAudioChunk(_audio.captureData(), _audio.captureBytes());
}

void AppController::processPlayback() {
  if (_appState != AppState::Thinking && _appState != AppState::Playing) {
    return;
  }

  const int buffered = _audio.bufferedPlaybackBytes();
  const bool hasEnoughBuffered = buffered >= kMinPlaybackBytes;
  const bool hasCompleteShortReply = _turn.complete() && buffered > 0;
  if (!_audio.playbackStarted() &&
      (hasEnoughBuffered || hasCompleteShortReply)) {
    _audio.markPlaybackStarted();
    if (_appState != AppState::Playing) {
      setAppState(AppState::Playing, "Speaking...");
    }
  }

  // Only exit to Ready from Playing — a turnComplete that arrives while we're
  // still Thinking (e.g. a stale signal from a prior, interrupted turn) must
  // not short-circuit waiting for the new response's audio.
  if (_appState == AppState::Playing && _turn.complete() &&
      _audio.playbackIdle()) {
    _turn.clearResponse();
    _audio.stopPlayback();
    setAppState(AppState::Ready, "Ready");
  }
}

void AppController::processThinkingTimeout() {
  if (_appState != AppState::Thinking) {
    return;
  }

  if (_turn.thinkingTimedOut(millis(), kThinkingTimeoutMs)) {
    Log::client("Loop",
                "thinking timeout chunks=%d failed=%d bytes=%u "
                "has_audio=%d turn_complete=%d buffered=%d",
                _turn.audioChunksSent(), _turn.audioChunksFailed(),
                static_cast<unsigned>(_turn.audioBytesSent()),
                _turn.hasAudio(), _turn.complete(),
                _audio.bufferedPlaybackBytes());
    _turn.clearResponse();
    _audio.stopPlayback();
    setDebugText("DBG response timeout");
    setAppState(AppState::Ready, "Ready");
  }
}

void AppController::processTextReveal() {
  const int targetLen = static_cast<int>(_toolTextRevealLayout.length());
  if (_toolTextRevealIndex >= targetLen) {
    return;
  }

  const bool canReveal =
      _appRegion == AppRegion::Chat &&
      (_appState == AppState::Ready || _appState == AppState::Thinking ||
       _appState == AppState::Playing);
  if (!canReveal) {
    completeToolTextReveal();
    return;
  }

  const unsigned long now = millis();
  if (_lastTextRevealMs == 0) {
    _lastTextRevealMs = now;
  }
  const unsigned long elapsed = now - _lastTextRevealMs;
  if (elapsed < kTextRevealFrameMs) {
    return;
  }

  // Reveal by elapsed time rather than one character per call. The main loop
  // stalls for long stretches while a response streams in, and a per-call step
  // would make the reveal crawl at the loop rate instead of the frame rate.
  const int steps = static_cast<int>(elapsed / kTextRevealFrameMs);
  _lastTextRevealMs += static_cast<unsigned long>(steps) * kTextRevealFrameMs;

  _toolTextRevealIndex = min(targetLen, _toolTextRevealIndex + steps);
  _toolText = _toolTextRevealLayout.substring(0, _toolTextRevealIndex);
  _screenDirty = true;
}

void AppController::serviceUi() {
  processTextReveal();
  renderIfNeeded();
}

void AppController::processWaitingIndicator() {
  if (_appState != AppState::Thinking || _appRegion != AppRegion::Chat) {
    return;
  }

  const unsigned long now = millis();
  if (_lastWaitingIndicatorRefreshMs == 0) {
    _lastWaitingIndicatorRefreshMs = now;
    _screenDirty = true;
    return;
  }
  if (now - _lastWaitingIndicatorRefreshMs < kWaitingIndicatorRefreshMs) {
    return;
  }

  _lastWaitingIndicatorRefreshMs = now;
  _waitingIndicatorFrame = (_waitingIndicatorFrame + 1) % 4;
  _screenDirty = true;
}

void AppController::processConnectingTimeout() {
  if (_appState != AppState::Connecting || !_live.isConnected() ||
      _connectingSinceMs == 0) {
    return;
  }

  const unsigned long now = millis();
  if (now - _connectingSinceMs < kConnectingReadyTimeoutMs) {
    return;
  }

  Log::client("WS", "ready timeout after %lu ms; reconnecting",
              now - _connectingSinceMs);
  _live.disconnect();
  setAppState(AppState::Connecting, "Retrying AI...");
}

void AppController::processMenuFetches() {
  if (_historyFetchTask != nullptr && _historyFetchDone) {
    finishConversationHistoryLoad();
  }

  if (_firmwareFetchTask != nullptr && _firmwareFetchDone) {
    finishFirmwareUpdateCheck();
  }
}

void AppController::processPower() {
  if (_appRegion != AppRegion::Menu && _appState == AppState::Ready &&
      (_timers.count() == 0 || IDLE_DEEP_SLEEP_ENABLED)) {
    _powerManager.update();
  }
}

void AppController::processCaptivePortal() {
  if (!_wifi.isCaptivePortalActive()) {
    return;
  }

  String ssid;
  if (!_wifi.consumeProvisioningSuccess(ssid)) {
    return;
  }

  setToolTextImmediate("Saved WiFi\n" + ssid + "\nReconnecting...");
  connectNetworkStack();
}

void AppController::renderIfNeeded() {
  if (!_screenDirty || _renderInProgress || !_displayReady) {
    return;
  }

  const unsigned long now = millis();
  // Cap animation paints at kAnimationRenderFrameMs whenever audio is
  // buffering or playing, or a reveal is mid-flight. Thinking is included so
  // the paints
  // triggered per websocket audio frame (via onAudio -> serviceUi) can't slow
  // the pre-playback buffering that time-to-first-audio depends on. Menu
  // interaction (non-Chat region) stays unthrottled unless the speaker is
  // actually running.
  const bool revealAnimating =
      _toolTextRevealIndex < static_cast<int>(_toolTextRevealLayout.length());
  const bool throttled =
      _audio.playbackStarted() || _audio.speakerBusy() ||
      (_appRegion == AppRegion::Chat &&
       (_appState == AppState::Thinking || _appState == AppState::Playing ||
        revealAnimating));
  if (throttled) {
    if (_lastAnimationRenderMs != 0 &&
        now - _lastAnimationRenderMs < kAnimationRenderFrameMs) {
      return;
    }
    _lastAnimationRenderMs = now;
  } else {
    _lastAnimationRenderMs = 0;
  }

  _renderInProgress = true;
  _screenDirty = false;
#ifdef FRAMEBUFFER_DEBUG_COMMANDS
  {
    static unsigned long lastPaintMs = 0;
    const unsigned long paintStart = millis();
    _display.render(buildDisplayState());
    const unsigned long paintEnd = millis();
    Log::client("Render", "interval=%lu took=%lu throttled=%d",
                lastPaintMs == 0 ? 0 : paintStart - lastPaintMs,
                paintEnd - paintStart, throttled ? 1 : 0);
    lastPaintMs = paintStart;
  }
#else
  _display.render(buildDisplayState());
#endif
  _renderInProgress = false;
}

DisplayState AppController::buildDisplayState() const {
  DisplayState state;
  state.appState = _appState;

  if (_appState == AppState::Alarm) {
    state.alarmActive = true;
    state.alarmTitle = _alarmTitle;
    state.alarmDetail = _alarmDetail;
    return state;
  }

  const bool homeMenuVisible =
      _appRegion == AppRegion::Menu && _menuState == MenuState::Home;
  if (homeMenuVisible) {
    state.headerLeft = currentTimeString();
    const int battery = Board::batteryLevel();
    if (battery >= 0 && battery <= 100) {
      state.headerRight = String(battery) + "%";
    }
  }

  state.bodyText = buildBodyText();
  state.bodyDim = _appState == AppState::Recording ||
                  _appState == AppState::Thinking;
  // While a reveal is still running, the newest characters ramp up through a
  // few brightness steps so streamed text fades in rather than popping.
  const bool revealing =
      !_bootMode && !_toolText.isEmpty() &&
      _toolTextRevealIndex < static_cast<int>(_toolTextRevealLayout.length());
  state.bodyFadeChars = revealing ? kTextRevealFadeChars : 0;
  state.imagePresent = _imagePresent;
  state.pageIndex = _bodyPageIndex;
  state.pageCount = currentBodyPageCount();
  if (Board::capabilities().debugDisplay && !_debugText.isEmpty() &&
      _appRegion == AppRegion::Chat) {
    state.footerLeft = _debugText;
    if (state.pageCount > 1) {
      state.footerRight =
          String(constrain(state.pageIndex + 1, 1, state.pageCount)) + "/" +
          String(state.pageCount);
    }
  }
  state.showMenu = _appRegion == AppRegion::Menu;
  if (state.showMenu) {
    const int count = menuItemCount();
    const int visibleCount = min(MAX_MENU_VISIBLE_ITEMS, count);
    const int pageStart =
        count <= MAX_MENU_VISIBLE_ITEMS
            ? 0
            : min(_menuSelection, count - MAX_MENU_VISIBLE_ITEMS);
    state.menuItemCount = visibleCount;
    state.menuSelectedIndex = _menuSelection - pageStart;
    state.menuHasMoreAbove = pageStart > 0;
    state.menuHasMoreBelow = pageStart + visibleCount < count;
    for (int i = 0; i < visibleCount; i++) {
      state.menuItems[i] = menuItemLabel(pageStart + i);
    }
  }
  return state;
}

String AppController::buildBodyText() const {
  if (Board::capabilities().bootDisplay && _bootMode) {
    return _bootLog.isEmpty() ? String("Starting...") : _bootLog;
  }

  if (!_toolText.isEmpty()) {
    return _toolText;
  }

  // When the model has shown an image but no text, suppress the default
  // greeting/status copy so the image stands alone as a single page.
  if (_imagePresent) {
    return "";
  }

  if (_wifi.isCaptivePortalActive()) {
    return "To set up a new WiFi network, connect your phone to the WiFi "
           "network named " +
           _wifi.captivePortalSsid() +
           " and enter your network's name and password.";
  }

  switch (_appState) {
  case AppState::Connecting:
    if (_startupChecklistVisible) {
      return buildStartupChecklistText();
    }
    return _statusText.isEmpty() ? String("Starting...") : _statusText;

  case AppState::Ready:
    return "Hi, how can I help? Hold the big button and speak to get a "
           "response.";

  case AppState::Recording:
    return "Hi, how can I help? Hold the big button and speak to get a "
           "response.";

  case AppState::Thinking:
    return "Thinking...";

  case AppState::Playing:
    return "";

  case AppState::ConfirmReset:
    return "Are you sure? Reset will remove data and restart into the last "
           "working version. WiFi credentials are kept.";

  case AppState::Error:
    switch (_errorCategory) {
    case ErrorCategory::Startup:
      return "Could not start device.";
    case ErrorCategory::WiFiTimeout:
      return "Could not connect to the internet.";
    case ErrorCategory::ServerRefused:
    case ErrorCategory::GeminiUnavailable:
    case ErrorCategory::None:
    default:
      return "Sorry, that didn't work.";
    }

  case AppState::Alarm:
    return "";
  }

  return "";
}

String AppController::buildStartupChecklistText() const {
  auto line = [](bool done, const char *label) {
    return String(done ? "[x] " : "[ ] ") + label;
  };

  return String("Starting...\n") + line(_startupPowerDone, "Powering on") +
         "\n" + line(_startupWifiDone, "WiFi") + "\n" +
         line(_startupInternetDone, "Internet");
}

String AppController::waitingIndicatorText() const {
  String text = "Waiting";
  const int dotCount = _waitingIndicatorFrame % 4;
  for (int i = 0; i < dotCount; i++) {
    text += '.';
  }
  return text;
}

int AppController::currentBodyPageCount() const {
  const String body = buildBodyText();
  const int textPages = body.isEmpty() ? 0 : _display.pageCountForText(body);
  const int total = (_imagePresent ? 1 : 0) + textPages;
  return max(1, total);
}

String AppController::currentTimeString() const {
  time_t now = time(nullptr);
  struct tm local;
  if (!localtime_r(&now, &local)) {
    return "";
  }
  if (local.tm_year + 1900 < 2024) {
    return "";
  }
  int hour12 = local.tm_hour % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }
  const char *suffix = local.tm_hour < 12 ? "AM" : "PM";
  char buf[10];
  snprintf(buf, sizeof(buf), "%d:%02d%s", hour12, local.tm_min, suffix);
  return String(buf);
}

String AppController::deviceStatusJson() const {
  JsonDocument status;
  status["firmware_version"] = FIRMWARE_VERSION;
  status["battery_percent"] = Board::batteryLevel();
  status["battery_voltage_mv"] = Board::batteryVoltageMv();
  status["vbus_voltage_mv"] = Board::vbusVoltageMv();
  status["power_source"] = Board::powerSourceLabel();
  status["volume"] = _audio.volume();
  status["brightness"] = Board::displayBrightness();
  status["voice"] = _settings.voice();
  status["server_endpoint"] = _live.activeEndpointLabel();
  status["wifi_network"] = _wifi.isConnected() ? _wifi.ssid() : "disconnected";
  status["uptime_seconds"] = millis() / 1000;
  status["active_timers"] = _timers.count();
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
  Log::client("Reset", "clearing device preferences");
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

void AppController::onTimersChanged() { _screenDirty = true; }

void AppController::checkTimerExpiry() {
  TimerRecord expired[kMaxExpiredPerWake];
  const int n =
      _timers.harvestExpired(time(nullptr), expired, kMaxExpiredPerWake);
  if (n <= 0) {
    return;
  }

  String namedDetail;
  for (int i = 0; i < n; i++) {
    if (expired[i].name.isEmpty()) {
      continue;
    }
    if (!namedDetail.isEmpty()) {
      namedDetail += ", ";
    }
    namedDetail += expired[i].name;
  }

  Log::client("Timers", "expired count=%d names=\"%s\"", n,
              namedDetail.c_str());

  if (_appState == AppState::Alarm) {
    if (!namedDetail.isEmpty()) {
      _alarmDetail = _alarmDetail.isEmpty() ? namedDetail
                                            : _alarmDetail + ", " + namedDetail;
    }
    _screenDirty = true;
    return;
  }

  enterAlarmState("ALARM", namedDetail);
}

void AppController::enterAlarmState(const String &title,
                                    const String &detail) {
  _alarmReturnState =
      _appState == AppState::Connecting ? AppState::Ready : _appState;
  _alarmReturnStatus = _statusText;
  _alarmTitle = title;
  _alarmDetail = detail;
  _lastAlarmSoundMs = 0;

  _audio.stopPlayback();
  if (_appState == AppState::Recording) {
    _audio.stopRecording();
    if (_live.isConnected() && _recordingCommitted) {
      _live.sendStop();
    }
    _recordingCommitted = false;
    _preCommitAudioChunkCount = 0;
  }

  _powerManager.registerActivity();
  _display.setBrightness(_settings.brightness());
  _appState = AppState::Alarm;
  _appRegion = AppRegion::Chat;
  _statusText = "Timer";
  _errorText = "";
  resetBodyPage();
  _screenDirty = true;
}

void AppController::exitAlarmState() {
  _audio.stopPlayback();
  _alarmTitle = "";
  _alarmDetail = "";
  _lastAlarmSoundMs = 0;

  if (!_networkStackStarted) {
    connectNetworkStack();
    return;
  }

  setAppState(_alarmReturnState, _alarmReturnStatus);
}

void AppController::serviceAlarmTrill() {
  if (_appState != AppState::Alarm) {
    return;
  }

  const unsigned long now = millis();
  if (_lastAlarmSoundMs != 0 && now - _lastAlarmSoundMs < kAlarmRepeatMs) {
    return;
  }

  _lastAlarmSoundMs = now;
  _audio.playAlarmMelody("A5:120 R:40 A5:120 R:40 A5:220");
}

String AppController::handleSetTimerTool(int durationSeconds,
                                         const String &name) {
  TimerRecord created;
  const TimerService::Result rc = _timers.addTimer(
      static_cast<uint32_t>(max(0, durationSeconds)), name, created);
  onTimersChanged();
  return formatTimerSummary(time(nullptr)) +
         (rc == TimerService::Result::Ok
              ? String(" | started")
              : String(" | error: ") + TimerService::describeResult(rc));
}

String AppController::handleListTimersTool() {
  return _timers.describeAll(time(nullptr));
}

String AppController::handleCancelTimerTool(const TimerRef &ref, bool all) {
  if (all) {
    const int n = _timers.cancelAll();
    onTimersChanged();
    return formatTimerSummary(time(nullptr)) + " | cancelled " + n +
           " timer(s)";
  }

  TimerRecord cancelled;
  const TimerService::Result rc = _timers.cancel(ref, cancelled);
  onTimersChanged();
  return formatTimerSummary(time(nullptr)) +
         (rc == TimerService::Result::Ok
              ? String(" | cancelled ") +
                    (cancelled.name.isEmpty() ? String("timer")
                                              : cancelled.name)
              : String(" | error: ") + TimerService::describeResult(rc));
}

String AppController::handleExtendTimerTool(int deltaSeconds,
                                            const TimerRef &ref) {
  TimerRecord adjusted;
  const TimerService::Result rc = _timers.extend(ref, deltaSeconds, adjusted);
  onTimersChanged();
  return formatTimerSummary(time(nullptr)) +
         (rc == TimerService::Result::Ok
              ? String(" | adjusted ") +
                    (adjusted.name.isEmpty() ? String("timer")
                                             : adjusted.name)
              : String(" | error: ") + TimerService::describeResult(rc));
}

String AppController::formatTimerSummary(time_t now) const {
  return _timers.describeAll(now);
}
