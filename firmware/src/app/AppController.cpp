#include "AppController.h"

#include "../Config.h"
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <WiFi.h>

void AppController::setup() {
  const unsigned long bootStartMs = millis();
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
  callbacks.onReady = [this]() {
    _appRegion = AppRegion::Chat;
    setAppState(AppState::Ready, "Ready");
  };
  callbacks.onTurnComplete = [this]() { _turnComplete = true; };
  callbacks.onChatId = [this](const String &chatId) {
    _chatId = chatId;
    _settings.setChatId(chatId);
    _live.setChatId(chatId);
    _screenDirty = true;
  };
  callbacks.onShowText = [this](const String &text) {
    _toolText = text;
    resetBodyPage();
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
    resetBodyPage();
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

  renderIfNeeded();
  const unsigned long bootElapsedMs = millis() - bootStartMs;
  if (bootElapsedMs < kMinBootDisplayMs) {
    delay(kMinBootDisplayMs - bootElapsedMs);
  }

  connectNetworkStack();
  renderIfNeeded();
}

void AppController::loop() {
  M5.update();

  if (millis() - _lastHeartbeatMs > 3000) {
    _lastHeartbeatMs = millis();
    Serial.printf("[Loop] state=%d region=%d power=%s ws=%d sleep=%d\n",
                  static_cast<int>(_appState), static_cast<int>(_appRegion),
                  powerStateName(_powerManager.getState()), _live.isConnected(),
                  _powerManager.getState() == PowerState::LightSleep);
  }

  if (millis() - _lastHeaderRefreshMs > 30000) {
    _lastHeaderRefreshMs = millis();
    _screenDirty = true;
  }

  _wifi.poll();
  _live.poll();
  _live.reconnectIfNeeded(_wifi.isConnected() &&
                          _powerManager.getState() != PowerState::LightSleep &&
                          _appState != AppState::Error);

  handleButtons();
  processRecording();
  processPlayback();
  processThinkingTimeout();
  processPower();
  processCaptivePortal();
  processAnimations();
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
  _appRegion = AppRegion::Chat;
  setAppState(AppState::Connecting, "Connecting...");
  if (!_wifi.connectKnownNetworks()) {
    setErrorState(ErrorCategory::WiFiTimeout, "WiFi failed",
                  "A retry  B hold menu");
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
  _appState = state;
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
  _errorCategory = category;
  _appState = AppState::Error;
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

void AppController::clearToolText() {
  _toolText = "";
  resetBodyPage();
}

void AppController::resetBodyPage() { _bodyPageIndex = 0; }

void AppController::restoreSessionPreview() {
  if (_chatId.isEmpty()) {
    return;
  }

  String lastMessage;
  if (_live.fetchLastAssistantMessage(lastMessage) && !lastMessage.isEmpty()) {
    _toolText = lastMessage;
    _statusText = "Restored";
    resetBodyPage();
    _screenDirty = true;
  }
}

void AppController::handleButtons() {
  const unsigned long now = millis();
  _buttonA.update(M5.BtnA.isPressed(), now);
  _buttonB.update(M5.BtnB.isPressed(), now);

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

void AppController::handleChatButtons() {
  if (_appState == AppState::Error && _buttonA.consumeClick()) {
    retryAfterError();
    return;
  }

  if (_buttonB.consumeHoldStart() && _appState != AppState::Recording) {
    openMenu(_appState == AppState::Error ? MenuState::Device : MenuState::Home);
    return;
  }

  if (_buttonB.consumeClick()) {
    _powerManager.registerActivity();
    const int pageCount = currentBodyPageCount();
    if (pageCount > 1) {
      _bodyPageIndex = (_bodyPageIndex + 1) % pageCount;
    } else if (!_toolText.isEmpty()) {
      clearToolText();
    }
    _screenDirty = true;
  }

  if (_buttonA.consumeHoldStart() &&
      (_appState == AppState::Ready || _appState == AppState::Playing ||
       _appState == AppState::Thinking)) {
    startRecording();
    return;
  }

  if ((_buttonA.consumeReleased() || _buttonA.consumeHoldRelease()) &&
      _appState == AppState::Recording) {
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
  _appRegion = AppRegion::Menu;
  _menuState = state;
  _menuSelection = 0;
  if (state == MenuState::ResumeChat) {
    loadConversationHistory();
  }
  _screenDirty = true;
}

void AppController::closeMenu() {
  _appRegion = AppRegion::Chat;
  _menuState = MenuState::Home;
  _menuSelection = 0;
  _screenDirty = true;
}

void AppController::navigateBackFromMenu() {
  if (_menuState == MenuState::Home) {
    closeMenu();
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
    switch (_menuSelection) {
    case 0:
      openMenu(MenuState::Home);
      return;
    case 1:
      closeMenu();
      startCaptivePortalFlow();
      return;
    case 2:
      closeMenu();
      checkForUpdates();
      return;
    case 3:
      _live.disconnect();
      _wifi.disconnect();
      delay(100);
      M5.Power.powerOff();
      return;
    default:
      return;
    }

  case MenuState::ResumeChat:
    if (_menuSelection == 0) {
      openMenu(MenuState::Home);
      return;
    }
    if (_historyCount == 0) {
      return;
    }
    resumeConversation(_menuSelection - 1);
    return;
  }
}

int AppController::menuItemCount() const {
  switch (_menuState) {
  case MenuState::Home:
  case MenuState::Device:
    return 4;
  case MenuState::ResumeChat:
    return _historyCount > 0 ? 1 + _historyCount : 2;
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
    return "Resume chat";
  }
  return "Menu";
}

String AppController::menuItemLabel(int index) const {
  switch (_menuState) {
  case MenuState::Home:
    switch (index) {
    case 0:
      return "Go back";
    case 1:
      return "New chat";
    case 2:
      return "Resume chat";
    case 3:
      return "Device";
    default:
      return "";
    }

  case MenuState::Device:
    switch (index) {
    case 0:
      return "Go back";
    case 1:
      return "Set up WiFi";
    case 2:
      return "Check updates";
    case 3:
      return "Turn off";
    default:
      return "";
    }

  case MenuState::ResumeChat:
    if (index == 0) {
      return "Go back";
    }
    if (_historyCount == 0 && index == 1) {
      return _toolText.isEmpty() ? "No saved chats" : _toolText.substring(0, 26);
    }
    if (index - 1 < _historyCount) {
      const ConversationSummary &entry = _history[index - 1];
      const String preview =
          entry.lastMessage.isEmpty() ? entry.chatId : entry.lastMessage;
      return preview.substring(0, 26);
    }
    return "";
  }

  return "";
}

void AppController::loadConversationHistory() {
  _historyCount = 0;
  if (!_wifi.isConnected()) {
    _toolText = "Connect WiFi first";
    resetBodyPage();
    return;
  }

  if (!_live.fetchConversationHistory(_history, kMaxConversationHistory,
                                      _historyCount)) {
    _toolText = "History unavailable";
    resetBodyPage();
    return;
  }

  if (_historyCount == 0) {
    _toolText = "No saved chats";
    resetBodyPage();
  }
}

void AppController::resumeConversation(int index) {
  if (index < 0 || index >= _historyCount) {
    return;
  }

  const ConversationSummary &entry = _history[index];
  _chatId = entry.chatId;
  _settings.setChatId(_chatId);
  _live.setChatId(_chatId);
  _toolText = entry.lastMessage;
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
    _toolText = "Join AP\n" + _wifi.captivePortalSsid() + "\nOpen " +
                _wifi.captivePortalIp() + "\nSubmit WiFi form";
  } else {
    setErrorState(ErrorCategory::WiFiTimeout, "Portal failed",
                  "Could not start setup AP");
  }
  resetBodyPage();
  _screenDirty = true;
}

void AppController::checkForUpdates() {
  FirmwareUpdateInfo info;
  if (!_wifi.isConnected()) {
    _toolText = "Offline\nCannot check updates";
  } else if (_live.checkFirmwareUpdate(info)) {
    if (info.available) {
      _toolText =
          "Update available\nv" + String(info.latestVersion) + "\n" + info.notes;
    } else {
      _toolText = "Up to date\nv" + String(FIRMWARE_VERSION);
    }
  } else {
    _toolText = "Update check failed";
  }
  resetBodyPage();
  _screenDirty = true;
}

void AppController::startRecording() {
  Serial.println("[Rec] === START RECORDING ===");
  _powerManager.registerActivity();
  clearToolText();
  _audio.stopPlayback();
  _audio.startRecording();
  _turnComplete = false;
  _audioChunksSent = 0;
  _recordingStartMs = millis();
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

  if (millis() - _recordingStartMs >= kMaxRecordingMs) {
    Serial.println("[Rec] Max recording time reached");
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
  if (_appRegion != AppRegion::Menu && _appState == AppState::Ready) {
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

  _toolText = "Saved WiFi\n" + ssid + "\nReconnecting...";
  resetBodyPage();
  _screenDirty = true;
  connectNetworkStack();
}

void AppController::processAnimations() {
  if (_appRegion == AppRegion::Menu) {
    return;
  }

  if (_appState != AppState::Connecting && _appState != AppState::Thinking) {
    return;
  }

  if (millis() - _lastAnimationMs < kAnimationFrameMs) {
    return;
  }

  _lastAnimationMs = millis();
  _animationPhase = (_animationPhase + 1) % 6;
  _screenDirty = true;
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
  state.pageIndex = _bodyPageIndex;
  state.pageCount = currentBodyPageCount();
  state.animationPhase = _animationPhase;
  state.showMenu = _appRegion == AppRegion::Menu;
  if (state.showMenu) {
    state.menuTitle = menuTitle();
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
  if (!_toolText.isEmpty()) {
    return _toolText;
  }

  if (_wifi.isCaptivePortalActive()) {
    return "WiFi setup\nJoin " + _wifi.captivePortalSsid() + "\nOpen " +
           _wifi.captivePortalIp();
  }

  switch (_appState) {
  case AppState::Connecting:
    if (_wifi.isConnected()) {
      return _statusText + "\n" + _wifi.ssid() + "\n" + _wifi.localIp();
    }
    return _statusText + "\nSearching WiFi";

  case AppState::Ready:
    return "Ready\nHold A to talk\nHold B for menu";

  case AppState::Recording:
    return "Listening\nRelease A to send";

  case AppState::Thinking:
    return "Thinking\nWaiting for reply";

  case AppState::Playing:
    return "Speaking\nHold A to interrupt";

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

int AppController::currentBodyPageCount() const {
  return _display.pageCountForText(buildBodyText());
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
