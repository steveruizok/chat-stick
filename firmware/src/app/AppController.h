#pragma once

#include "../Config.h"
#include "../input/ButtonStateMachine.h"
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
  enum class AppRegion { Initializing, Chat, Menu };

  enum class ErrorCategory {
    None,
    Startup,
    WiFiTimeout,
    ServerRefused,
    GeminiUnavailable,
  };

  enum class MenuState { Home, Device, ResumeChat };

  static constexpr int kMinPlaybackBytes = PLAY_SAMPLE_RATE * 2 / 4;
  static constexpr unsigned long kThinkingTimeoutMs = 15000;
  static constexpr unsigned long kMaxRecordingMs = 30000;
  static constexpr unsigned long kResetHoldMs = 1500;
  static constexpr unsigned long kMinBootDisplayMs = 800;
  static constexpr unsigned long kAnimationFrameMs = 120;
  static constexpr int kMaxConversationHistory = 10;

  AppRegion _appRegion = AppRegion::Initializing;
  AppState _appState = AppState::Connecting;
  MenuState _menuState = MenuState::Home;
  String _statusText = "Starting...";
  String _errorText;
  String _chatId;
  String _toolText;
  bool _turnComplete = false;
  bool _screenDirty = true;
  unsigned long _thinkingStartMs = 0;
  unsigned long _recordingStartMs = 0;
  unsigned long _resetHoldStartMs = 0;
  unsigned long _lastHeartbeatMs = 0;
  unsigned long _lastHeaderRefreshMs = 0;
  unsigned long _lastAnimationMs = 0;
  int _audioChunksSent = 0;
  int _bodyPageIndex = 0;
  int _menuSelection = 0;
  int _historyCount = 0;
  uint8_t _animationPhase = 0;
  ErrorCategory _errorCategory = ErrorCategory::None;
  AppState _resetReturnState = AppState::Ready;
  String _resetReturnStatus;
  String _resetReturnError;
  ErrorCategory _resetReturnCategory = ErrorCategory::None;
  ConversationSummary _history[kMaxConversationHistory];

  ButtonStateMachine _buttonA = ButtonStateMachine(500, 350);
  ButtonStateMachine _buttonB = ButtonStateMachine(1000, 350);

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
  void retryAfterError();
  void clearToolText();
  void resetBodyPage();
  void restoreSessionPreview();
  void beginFactoryReset();
  const char *errorCategoryLabel() const;

  void handleButtons();
  void handleChatButtons();
  void handleMenuButtons();
  void startRecording();
  void stopRecording();
  void processRecording();
  void processPlayback();
  void processThinkingTimeout();
  void processPower();
  void processCaptivePortal();
  void processAnimations();
  void renderIfNeeded();
  float recordingProgress() const;
  int currentBodyPageCount() const;

  void openMenu(MenuState state = MenuState::Home);
  void closeMenu();
  void navigateBackFromMenu();
  void cycleMenuSelection();
  void selectCurrentMenuItem();
  int menuItemCount() const;
  String menuTitle() const;
  String menuItemLabel(int index) const;
  void loadConversationHistory();
  void resumeConversation(int index);
  void startFreshConversation();
  void startCaptivePortalFlow();
  void checkForUpdates();

  DisplayState buildDisplayState() const;
  String buildBodyText() const;
  String deviceStatusJson() const;
};
