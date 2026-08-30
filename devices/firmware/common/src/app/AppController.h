#pragma once

#include "Config.h"
#include "hal/DeviceCapabilities.h"
#include "input/ButtonStateMachine.h"
#include "services/AudioService.h"
#include "services/LiveSessionService.h"
#include "power/PowerManager.h"
#include "services/SettingsStore.h"
#include "services/TimerService.h"
#include "services/WiFiService.h"
#include "state/StateTypes.h"
#include "ui/TextDisplay.h"
#include "app/TurnController.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/**
 * @brief Top-level firmware coordinator for input, networking, audio, power,
 * and UI.
 */
class AppController {
public:
  /// Initialize all services and enter the main application flow.
  void setup();

  /// Run one iteration of the main controller loop.
  void loop();

private:
  /**
   * @brief High-level screen region currently being rendered.
   */
  enum class AppRegion { Initializing, Chat, Menu };

  /**
   * @brief Categories of user-visible error states.
   */
  enum class ErrorCategory {
    None,
    Startup,
    WiFiTimeout,
    ServerRefused,
    GeminiUnavailable,
  };

  /**
   * @brief Menu screens available from the UI overlay.
   */
  enum class MenuState { Home, Device, ResumeChat, Updates };

  /**
   * @brief Async loading status for menu screens backed by network requests.
   */
  enum class MenuLoadStatus { Idle, Loading, Loaded, Failed };

  /**
   * @brief Why the current firmware update check task was started.
   */
  enum class FirmwareCheckReason { None, Menu, Automatic };

  /// Minimum audio buffered before switching into playback state.
  static constexpr int kMinPlaybackBytes =
      PLAY_SAMPLE_RATE * sizeof(int16_t) * 3 / 4;

  /// Maximum time to wait in Thinking before surfacing a timeout.
  static constexpr unsigned long kThinkingTimeoutMs = 15000;

  /// Maximum time to wait for the server to report a ready AI session.
  static constexpr unsigned long kConnectingReadyTimeoutMs = 20000;

  /// Maximum continuous recording duration.
  static constexpr unsigned long kMaxRecordingMs = 30000;

  /// Minimum press duration before a local recording becomes a server turn.
  static constexpr unsigned long kRecordingCommitMs = 150;

  /// Hold duration required to trigger a reset flow.
  static constexpr unsigned long kResetHoldMs = 1500;

  /// Rate limit for repeated capture-failure logs.
  static constexpr unsigned long kCaptureFailureLogIntervalMs = 1000;

  /// Refresh cadence for the on-screen wait indicator.
  static constexpr unsigned long kWaitingIndicatorRefreshMs = 500;

  /// Cadence for the M5-style tool text reveal animation.
  static constexpr unsigned long kTextRevealFrameMs = 54;

  /// Trailing characters still ramping to full brightness during a reveal.
  static constexpr int kTextRevealFadeChars = 3;

  /// Animation paint cadence (10 fps) while audio is buffering or playing, or
  /// while a text reveal is running. Painting is the most expensive thing the
  /// main loop does, and it shares that loop with the websocket pump that
  /// keeps the playback ring buffer fed.
  static constexpr unsigned long kAnimationRenderFrameMs = 100;

  /// Samples captured in one microphone chunk.
  static constexpr int kCaptureChunkSamples =
      MIC_SAMPLE_RATE * MIC_CHUNK_MS / 1000;

  /// Number of chunks retained while deciding whether a press is intentional.
  static constexpr int kPreCommitAudioChunks =
      (kRecordingCommitMs + MIC_CHUNK_MS - 1) / MIC_CHUNK_MS;

  /// Maximum number of conversation history entries retained locally.
  static constexpr int kMaxConversationHistory = 10;

  /// Stack size used by short-lived menu fetch tasks.
  static constexpr uint32_t kMenuFetchTaskStack = 8192;

  /// Maximum expired timers to handle in one loop pass.
  static constexpr int kMaxExpiredPerWake = MAX_TIMERS;

  /// Cadence for repeating the timer alarm trill.
  static constexpr unsigned long kAlarmRepeatMs = 2200;

  /// Current high-level screen region.
  AppRegion _appRegion = AppRegion::Initializing;

  /// Current application state.
  AppState _appState = AppState::Connecting;

  /// Current menu screen when the menu overlay is open.
  MenuState _menuState = MenuState::Home;

  /// Primary status text shown to the user.
  String _statusText = "Starting...";

  /// Detailed error text shown to the user.
  String _errorText;

  /// Active conversation id.
  String _chatId;

  /// Most recent tool-generated body text.
  String _toolText;

  /// Full target text currently being revealed.
  String _toolTextRevealTarget;

  /// Wrapped text layout used by the reveal animation.
  String _toolTextRevealLayout;

  /// Auxiliary debug text shown in the UI.
  String _debugText;

  /// Boot-time log text accumulated before normal UI mode.
  String _bootLog;

  /// Whether the app is still showing the boot UI.
  bool _bootMode = true;

  /// Whether the display subsystem finished initialization.
  bool _displayReady = false;

  /// Whether a render is currently in progress.
  bool _renderInProgress = false;

  /// Whether the current recording has been committed to the server.
  bool _recordingCommitted = false;

  /// Whether the startup checklist should be shown in Connecting state.
  bool _startupChecklistVisible = true;

  /// Startup checklist item: local power/display setup complete.
  bool _startupPowerDone = false;

  /// Startup checklist item: WiFi connected.
  bool _startupWifiDone = false;

  /// Startup checklist item: server/session channel connected.
  bool _startupInternetDone = false;

  /// Whether an image is currently available for the body area.
  bool _imagePresent = false;

  /// Whether the screen contents need a fresh render.
  bool _screenDirty = true;

  /// Timestamp when reset-hold detection started.
  unsigned long _resetHoldStartMs = 0;

  /// Timestamp of the last heartbeat-like activity update.
  unsigned long _lastHeartbeatMs = 0;

  /// Timestamp of the last header refresh.
  unsigned long _lastHeaderRefreshMs = 0;

  /// Timestamp of the last animated wait-indicator refresh.
  unsigned long _lastWaitingIndicatorRefreshMs = 0;

  /// Timestamp of the last text reveal frame.
  unsigned long _lastTextRevealMs = 0;

  /// Timestamp of the last animation-throttled display refresh.
  unsigned long _lastAnimationRenderMs = 0;

  /// Current animated wait-indicator frame.
  int _waitingIndicatorFrame = 0;

  /// Buffered local audio captured before the press passes kRecordingCommitMs.
  int16_t _preCommitAudio[kCaptureChunkSamples * kPreCommitAudioChunks];

  /// Number of valid chunks currently buffered in _preCommitAudio.
  int _preCommitAudioChunkCount = 0;

  /// Current body page index.
  int _bodyPageIndex = 0;

  /// Current reveal index inside _toolTextRevealLayout.
  int _toolTextRevealIndex = 0;

  /// Current selected menu index.
  int _menuSelection = 0;

  /// Number of loaded conversation history entries.
  int _historyCount = 0;

  /// Current load status for the resume-chat menu.
  MenuLoadStatus _historyLoadStatus = MenuLoadStatus::Idle;

  /// User-visible status for the resume-chat menu when no rows are available.
  String _historyLoadMessage;

  /// Current load status for the firmware updates menu.
  MenuLoadStatus _firmwareCheckStatus = MenuLoadStatus::Idle;

  /// User-visible status for the firmware updates menu.
  String _firmwareCheckMessage;

  /// Last firmware update metadata fetched from the server.
  FirmwareUpdateInfo _firmwareInfo;

  /// Current error category, if any.
  ErrorCategory _errorCategory = ErrorCategory::None;

  /// App state to restore after the reset confirmation flow.
  AppState _resetReturnState = AppState::Ready;

  /// Status text to restore after the reset confirmation flow.
  String _resetReturnStatus;

  /// Error text to restore after the reset confirmation flow.
  String _resetReturnError;

  /// Error category to restore after the reset confirmation flow.
  ErrorCategory _resetReturnCategory = ErrorCategory::None;

  /// Cached conversation history for the resume-chat menu.
  ConversationSummary _history[kMaxConversationHistory];

  /// Background task result buffer for conversation history.
  ConversationSummary _historyFetchResults[kMaxConversationHistory];

  /// Number of entries in _historyFetchResults.
  int _historyFetchCount = 0;

  /// Whether the background history request succeeded.
  bool _historyFetchOk = false;

  /// Message produced by the background history request.
  String _historyFetchMessage;

  /// Whether the background history request has completed.
  volatile bool _historyFetchDone = false;

  /// Background task result for firmware update checks.
  FirmwareUpdateInfo _firmwareFetchInfo;

  /// Whether the background firmware check succeeded.
  bool _firmwareFetchOk = false;

  /// Message produced by the background firmware check.
  String _firmwareFetchMessage;

  /// Whether the background firmware check has completed.
  volatile bool _firmwareFetchDone = false;

  /// Why the active firmware check task is running.
  FirmwareCheckReason _firmwareFetchReason = FirmwareCheckReason::None;

  /// State machine for the push-to-talk button.
  ButtonStateMachine _buttonA = ButtonStateMachine(500, 0);

  /// State machine for the menu/power button.
  ButtonStateMachine _buttonB = ButtonStateMachine(1000, 0);

  /// Per-turn voice exchange state.
  TurnController _turn;

  /// UI renderer.
  TextDisplay _display;

  /// Idle and power-state manager.
  PowerManager _powerManager;

  /// WiFi and captive-portal service.
  WiFiService _wifi;

  /// Audio capture and playback service.
  AudioService _audio;

  /// Live chat session transport service.
  LiveSessionService _live;

  /// Persistent settings store.
  SettingsStore _settings;

  /// Local persistent timer store.
  TimerService _timers;

  /// Active background task for loading conversation history.
  TaskHandle_t _historyFetchTask = nullptr;

  /// Active background task for checking firmware updates.
  TaskHandle_t _firmwareFetchTask = nullptr;

  /// Timestamp of the last power-management poll.
  unsigned long _lastPowerPollMs = 0;

  /// Timestamp of the most recent Connecting-state transition.
  unsigned long _connectingSinceMs = 0;

  /// App state to restore after dismissing a timer alarm.
  AppState _alarmReturnState = AppState::Ready;

  /// Status text to restore after dismissing a timer alarm.
  String _alarmReturnStatus;

  /// Alarm title shown by the display.
  String _alarmTitle;

  /// Alarm detail shown by the display.
  String _alarmDetail;

  /// Last time the alarm trill was played.
  unsigned long _lastAlarmSoundMs = 0;

  /// Whether the network stack has been started since boot.
  bool _networkStackStarted = false;

  /// Whether boot went straight into an expired timer alarm.
  bool _bootHadExpiredAlarm = false;

  /// Whether a valid deferred firmware update was already known at boot.
  bool _pendingFirmwareUpdateAtBoot = false;

  /// Whether this boot has already tried to install the deferred update.
  bool _pendingFirmwareInstallAttempted = false;

  /// Whether this boot has already launched its automatic update check.
  bool _automaticFirmwareCheckStarted = false;

  /// Wire service callbacks back into controller state transitions.
  void configureCallbacks();

  /// Bring up WiFi and the live session stack.
  void connectNetworkStack();

  /// React once the device has reached the server after WiFi connection.
  void handleInternetReady();

  /// Enable or disable the network-dependent services.
  void setNetworkEnabled(bool enabled);

  /// Transition the app state and optionally replace status/error text.
  void setAppState(AppState state, const String &status = "",
                   const String &error = "");

  /// Enter an error state with category and user-facing text.
  void setErrorState(ErrorCategory category, const String &status,
                     const String &error);

  /// Retry connection or session setup after an error.
  void retryAfterError();

  /// Shut the device down cleanly.
  void performPowerOff(bool allowIdleDeepSleep = false);

  /// Disconnect services and delegate final power-down to the board HAL.
  void shutdownHardware();

  /// Whether a deep-sleep timer wake has elapsed past the idle shutdown window.
  bool shouldPowerOffAfterIdleDeepSleep(DeepSleepWakeReason wakeReason) const;

  /// Enter deep sleep until the next timer or idle shutdown deadline.
  bool enterDeepSleepForTimerOrIdle(bool includeIdleShutdownDeadline);

  /// Clear transient tool text from the UI.
  void clearToolText();

  /// Replace tool text immediately, bypassing reveal.
  void setToolTextImmediate(const String &text);

  /// Start revealing a new tool text block.
  void startToolTextReveal(const String &text);

  /// Append text to the current reveal target.
  void appendToolTextReveal(const String &text);

  /// Rebuild wrapped reveal layout after target text changes.
  void rebuildToolTextRevealLayout();

  /// Complete the current reveal animation immediately.
  void completeToolTextReveal();

  /// Cancel any in-progress reveal animation.
  void cancelToolTextReveal();

  /// Update transient debug text shown in the UI.
  void setDebugText(const String &text);

  /// Clear transient debug text.
  void clearDebugText();

  /// Reset body pagination to the first page.
  void resetBodyPage();

  /// Append a formatted line to the boot log.
  void appendBootLog(const char *topic, const char *message);

  /// Leave boot-log mode and enter the normal UI.
  void exitBootMode();

  /// Static trampoline used by boot logging callbacks.
  static void bootLogTrampoline(void *ctx, char side, const char *topic,
                                const char *message);

  /// Restore the last known assistant message for the saved chat id.
  void restoreSessionPreview();

  /// Enter the factory reset confirmation flow.
  void beginFactoryReset();

  /// Human-readable label for the current error category.
  const char *errorCategoryLabel() const;

  /// Poll and dispatch button events.
  void handleButtons();

  /// Handle buttons while a timer alarm is active.
  bool handleAlarmButtons();

  /// Clear latched button events when changing input regions.
  void clearButtonEvents();

  /// Handle buttons while in the chat screen.
  void handleChatButtons();

  /// Handle buttons while a menu is open.
  void handleMenuButtons();

  /// Start a new push-to-talk recording turn.
  void startRecording();

  /// Finish the current recording turn.
  void stopRecording();

  /// Commit a long-enough recording press to the server.
  bool commitRecording();

  /// Discard an uncommitted local recording press.
  void discardRecording(const String &reason);

  /// Store one captured chunk before the recording has committed.
  void bufferPreCommitAudio(const int16_t *data);

  /// Send one captured or buffered PCM chunk to the server.
  bool sendAudioChunk(const int16_t *data, size_t bytes);

  /// Capture and send microphone audio while recording.
  void processRecording();

  /// Advance speaker playback and state transitions.
  void processPlayback();

  /// Detect and handle prolonged Thinking timeouts.
  void processThinkingTimeout();

  /// Advance M5-style tool text reveal animation.
  void processTextReveal();

  /// Animate the visible indicator while waiting for a response.
  void processWaitingIndicator();

  /// Recover when the WebSocket opens but the AI session never becomes ready.
  void processConnectingTimeout();

  /// Complete background menu requests when their tasks finish.
  void processMenuFetches();

  /// Poll and react to power-management state changes.
  void processPower();

  /// Service captive-portal state transitions.
  void processCaptivePortal();

  /// Render the UI when needed.
  void renderIfNeeded();

  /**
   * @brief Advance the reveal animation and repaint outside the main loop.
   *
   * WebsocketsClient::poll() drains every frame that is already buffered
   * before it returns, so a streaming response can keep the main loop inside
   * a single poll() call for the whole turn. Calling this from the websocket
   * callbacks keeps text appearing while audio is still arriving.
   */
  void serviceUi();

  /// Total number of body pages available for the current content.
  int currentBodyPageCount() const;

  /// Open the menu overlay at a given menu screen.
  void openMenu(MenuState state = MenuState::Home);

  /// Close the menu overlay and return to chat.
  void closeMenu();

  /// Navigate one level back from the current menu screen.
  void navigateBackFromMenu();

  /// Move the current menu selection to the next item.
  void cycleMenuSelection();

  /// Activate the currently selected menu item.
  void selectCurrentMenuItem();

  /// Number of items in the current menu screen.
  int menuItemCount() const;

  /// Title string for the current menu screen.
  String menuTitle() const;

  /// Label for a menu item by index.
  String menuItemLabel(int index) const;

  /// Load conversation history for the resume-chat menu.
  void startConversationHistoryLoad();

  /// Background worker body for conversation history fetches.
  void conversationHistoryLoadTask();

  /// FreeRTOS trampoline for conversationHistoryLoadTask().
  static void conversationHistoryLoadTaskTrampoline(void *ctx);

  /// Apply a finished conversation history fetch to menu state.
  void finishConversationHistoryLoad();

  /// Resume a conversation from the history list.
  void resumeConversation(int index);

  /// Start a new blank conversation.
  void startFreshConversation();

  /// Switch into WiFi provisioning mode.
  void startCaptivePortalFlow();

  /// Check for an available firmware update.
  void startFirmwareUpdateCheck();

  /// Check for an available firmware update without interrupting the UI.
  void startAutomaticFirmwareUpdateCheck();

  /// Background worker body for firmware update checks.
  void firmwareUpdateCheckTask();

  /// FreeRTOS trampoline for firmwareUpdateCheckTask().
  static void firmwareUpdateCheckTaskTrampoline(void *ctx);

  /// Apply a finished firmware update check to menu state.
  void finishFirmwareUpdateCheck();

  /// Download and apply the selected firmware update.
  void installFirmwareUpdate();

  /// Download and apply a specific firmware update.
  void installFirmwareUpdate(const FirmwareUpdateInfo &info);

  /// Install a deferred update that was discovered before this boot.
  bool installPendingFirmwareUpdate();

  /// Build the display snapshot for the current UI state.
  DisplayState buildDisplayState() const;

  /// Build the body text shown in the chat region.
  String buildBodyText() const;

  /// Build the M5-style startup checklist body text.
  String buildStartupChecklistText() const;

  /// Build footer text shown while waiting for a response.
  String waitingIndicatorText() const;

  /// Serialize device status for tool responses.
  String deviceStatusJson() const;

  /// Current local time string for the header.
  String currentTimeString() const;

  /// Mark timer-derived UI as dirty.
  void onTimersChanged();

  /// Harvest expired timers and enter the alarm state when needed.
  void checkTimerExpiry();

  /// Show and sound a local timer alarm.
  void enterAlarmState(const String &title, const String &detail);

  /// Dismiss a local timer alarm.
  void exitAlarmState();

  /// Play the repeating alarm trill while the alarm is active.
  void serviceAlarmTrill();

  /// Device-side implementation of the set_timer tool.
  String handleSetTimerTool(int durationSeconds, const String &name);

  /// Device-side implementation of the list_timers tool.
  String handleListTimersTool();

  /// Device-side implementation of the cancel_timer tool.
  String handleCancelTimerTool(const TimerRef &ref, bool all);

  /// Device-side implementation of the extend_timer tool.
  String handleExtendTimerTool(int deltaSeconds, const TimerRef &ref);

  /// JSON summary for timer tool responses.
  String formatTimerSummary(time_t now) const;
};
