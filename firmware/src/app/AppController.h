#pragma once

#include "../Config.h"
#include "../power/PowerManager.h"
#include "../services/AudioService.h"
#include "../services/LiveSessionService.h"
#include "../services/SettingsStore.h"
#include "../services/WiFiService.h"
#include "../state/StateTypes.h"
#include "../ui/TextDisplay.h"

class AppController {
public:
  void setup();
  void loop();

private:
  enum class ErrorCategory {
    None,
    Startup,
    WiFiTimeout,
    ServerRefused,
    GeminiUnavailable,
  };

  static constexpr int kMinPlaybackBytes = PLAY_SAMPLE_RATE * 2 / 4;
  static constexpr unsigned long kThinkingTimeoutMs = 15000;
  static constexpr unsigned long kRecordingGraceMs = 500;
  static constexpr unsigned long kMaxRecordingMs = 30000;
  static constexpr unsigned long kResetHoldMs = 1500;

  AppState _appState = AppState::Connecting;
  String _statusText = "Starting...";
  String _errorText;
  String _chatId;
  String _toolText;
  bool _turnComplete = false;
  bool _screenDirty = true;
  unsigned long _thinkingStartMs = 0;
  unsigned long _recordingStartMs = 0;
  unsigned long _recordStopDeadlineMs = 0;
  unsigned long _resetHoldStartMs = 0;
  unsigned long _lastHeartbeatMs = 0;
  unsigned long _lastHeaderRefreshMs = 0;
  int _audioChunksSent = 0;
  bool _recordStopPending = false;
  ErrorCategory _errorCategory = ErrorCategory::None;
  AppState _resetReturnState = AppState::Ready;
  String _resetReturnStatus;
  String _resetReturnError;
  ErrorCategory _resetReturnCategory = ErrorCategory::None;

  TextDisplay _display;
  PowerManager _powerManager;
  WiFiService _wifi;
  AudioService _audio;
  LiveSessionService _live;
  SettingsStore _settings;

  void configureCallbacks();
  void connectNetworkStack();
  void setNetworkEnabled(bool enabled);
  void setAppState(AppState state, const String &status = "",
                   const String &error = "");
  void setErrorState(ErrorCategory category, const String &status,
                     const String &error);
  void clearToolText();
  void restoreSessionPreview();
  void beginFactoryReset();
  const char *errorCategoryLabel() const;

  void handleButtons();
  void startRecording();
  void stopRecording();
  void processRecording();
  void processPlayback();
  void processThinkingTimeout();
  void processPower();
  void renderIfNeeded();
  float recordingProgress() const;

  DisplayState buildDisplayState() const;
  String buildBodyText() const;
  String deviceStatusJson() const;
};
