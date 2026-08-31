#pragma once

#include "Config.h"
#include "services/TimerService.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <Preferences.h>
#include <functional>

/**
 * @brief Callback hooks invoked as live session events arrive from the server.
 */
struct LiveSessionCallbacks {
  /// Optional mirrored log sink: side is 'C' for client, 'S' for server.
  std::function<void(char side, const char *topic, const char *message)> onLog;

  /// Invoked for session or transport errors.
  std::function<void(const String &, const String &)> onError;

  /// Invoked when network activity occurs and idle timers should reset.
  std::function<void()> onActivity;

  /// Invoked when transient status text should be shown.
  std::function<void(const String &)> onStatus;

  /// Invoked once the server-side device channel is ready for user input.
  std::function<void()> onServerReady;

  /// Invoked once the live session is ready for use.
  std::function<void()> onReady;

  /// Invoked when Gemini finishes the current turn.
  std::function<void()> onTurnComplete;

  /// Invoked when buffered audio should be discarded.
  std::function<void()> onDropAudio;

  /// Invoked when the server assigns or changes the chat id.
  std::function<void(const String &)> onChatId;

  /// Invoked when the conversation is reset remotely.
  std::function<void()> onConversationReset;

  /// Invoked when text should be displayed on screen.
  std::function<void(const String &)> onShowText;

  /// Invoked when an image payload should be displayed.
  std::function<void(const uint8_t *, size_t, int, int)> onShowImage;

  /// Additional flipbook animation frame (packed bits, byte length, width,
  /// height, frame index, flip interval ms). Frame 0 arrives via onShowImage;
  /// flipping starts once a second frame lands.
  std::function<void(const uint8_t *, size_t, int, int, int, int)>
      onAnimationFrame;

  /// Invoked while an image request is still in progress.
  std::function<void()> onShowImagePending;

  /// Invoked when an image request fails.
  std::function<void()> onShowImageFailed;

  /// Invoked for transcript updates from user or assistant.
  std::function<void(const String &, const String &)> onTranscript;

  /// Invoked when the server reports audio was ignored.
  std::function<void(const String &)> onIgnoredAudio;

  /// Invoked with PCM audio that should be queued for playback.
  std::function<void(const uint8_t *, size_t)> onAudio;

  /// Invoked when a tool requests brightness changes.
  std::function<void(int)> onBrightness;

  /// Invoked when a tool requests volume changes.
  std::function<void(int)> onVolume;

  /// Invoked when a tool requests speaker routing changes.
  std::function<bool(const String &)> onSetSpeaker;

  /// Invoked when a tool requests external speaker gain changes.
  std::function<bool(int)> onSetExternalGain;

  /// Invoked when a named UI sound should be played.
  std::function<bool(const String &)> onPlaySound;

  /// Invoked when a melody sequence should be played.
  std::function<bool(const String &)> onPlayMelody;

  /// Invoked when a tool requests power-off.
  std::function<void()> onPowerOff;

  /// Invoked when power timeout settings should be updated.
  std::function<void(unsigned long, unsigned long, unsigned long,
                     unsigned long)>
      onPowerTimeouts;

  /// Called when the server requests a JSON device-status payload.
  std::function<String()> getDeviceStatusJson;

  /// Invoked when the voice selection changes remotely.
  std::function<void(const String &)> onVoiceChanged;

  /// Invoked when a server endpoint succeeds and should be preferred later.
  std::function<void(int)> onEndpointIndexChanged;

  /// Invoked when a timer should be created on the device.
  std::function<String(int durationSeconds, const String &name)> onSetTimer;

  /// Invoked when active timers should be listed.
  std::function<String()> onListTimers;

  /// Invoked when one or all timers should be cancelled.
  std::function<String(const TimerRef &ref, bool all)> onCancelTimer;

  /// Invoked when a timer should be extended or shortened.
  std::function<String(int deltaSeconds, const TimerRef &ref)> onExtendTimer;
};

/**
 * @brief Summary entry returned by the conversation history endpoint.
 */
struct ConversationSummary {
  /// Conversation identifier.
  String chatId;

  /// Last assistant message preview.
  String lastMessage;

  /// Timestamp of the last update.
  String updatedAt;
};

/**
 * @brief Firmware update metadata returned by the update endpoint.
 */
struct FirmwareUpdateInfo {
  /// Whether a newer firmware build is available.
  bool available = false;

  /// Latest available firmware version number.
  int latestVersion = 0;

  /// Release notes for the available update.
  String notes;

  /// Download URL for the OTA payload.
  String downloadUrl;
};

/**
 * @brief Maintains the device-side WebSocket session with the chat server.
 */
class LiveSessionService {
public:
  /**
   * @brief Initialize the service with callback hooks.
   * @param callbacks Callback bundle for session events and tool actions.
   */
  void init(const LiveSessionCallbacks &callbacks);

  /// Open the live session WebSocket connection.
  void connect();

  /// Close the live session WebSocket connection.
  void disconnect();

  /// Pump WebSocket client work and timers.
  void poll();

  /**
   * @brief Queue one log line for shipping to the server's device_logs store.
   * @param side 'C' for client-side lines, 'S' for server-echo lines.
   * @param topic Log topic.
   * @param message Formatted log message.
   */
  void noteLog(char side, const char *topic, const char *message);

  /**
   * @brief Reconnect when disconnected and reconnects are allowed.
   * @param enabled Whether reconnect behavior is currently enabled.
   */
  void reconnectIfNeeded(bool enabled);

  /**
   * @brief Update the chat id sent with subsequent requests.
   * @param chatId Conversation identifier.
   */
  void setChatId(const String &chatId) { _chatId = chatId; }

  /**
   * @brief Update the selected voice sent with subsequent requests.
   * @param voice Voice identifier.
   */
  void setVoice(const String &voice) { _voice = voice; }

  /**
   * @brief Prefer a server endpoint for the next connection attempt.
   * @param endpointIndex Index into SERVER_ENDPOINTS.
   */
  void setPreferredEndpointIndex(int endpointIndex);

  /// Current selected voice identifier.
  const String &voice() const { return _voice; }

  /// Whether the live session is currently connected.
  bool isConnected() const { return _connected; }

  /// Index of the currently connected server endpoint.
  int activeServerIndex() const { return _activeServerIndex; }

  /// Human-readable label for the active endpoint.
  String activeEndpointLabel() const;

  /// Verify that at least one configured endpoint is reachable.
  bool pingServer();

  /// Send a start-of-turn message.
  bool sendStart();

  /// Send an end-of-turn message.
  bool sendStop();

  /**
   * @brief Send a chunk of microphone PCM data.
   * @param data Pointer to PCM samples.
   * @param len Number of bytes to send.
   * @return True when the chunk was queued to the socket.
   */
  bool sendAudio(const int16_t *data, size_t len);

  /**
   * @brief Fetch the last assistant message for session restore.
   * @param outMessage Receives the last assistant message.
   * @return True on success.
   */
  bool fetchLastAssistantMessage(String &outMessage);

  /**
   * @brief Fetch recent conversation history summaries.
   * @param outEntries Output buffer for summary entries.
   * @param maxEntries Capacity of the output buffer.
   * @param outCount Receives the number of entries written.
   * @return True on success.
   */
  bool fetchConversationHistory(ConversationSummary outEntries[],
                                int maxEntries, int &outCount);

  /**
   * @brief Check whether a newer firmware version is available.
   * @param outInfo Receives firmware update metadata.
   * @return True on successful request parsing.
   */
  bool checkFirmwareUpdate(FirmwareUpdateInfo &outInfo);

  /**
   * @brief Download and apply an OTA firmware update.
   * @param downloadUrl URL of the OTA binary.
   * @param outError Receives a human-readable error on failure.
   * @return True when the OTA process completes successfully.
   */
  bool downloadAndApplyFirmwareUpdate(const String &downloadUrl,
                                      String &outError);

private:
  /**
   * @brief HTTP GET response returned from a configured endpoint.
   */
  struct HttpGetResponse {
    /// Index into SERVER_ENDPOINTS.
    int endpointIndex = -1;

    /// HTTP status code, or a negative value when the request failed locally.
    int statusCode = -1;

    /// Response body for successful transport-level responses.
    String body;
  };

  /**
   * @brief Caller decision after inspecting a configured-endpoint response.
   */
  enum class HttpGetDecision {
    /// Try the next configured endpoint.
    Continue,

    /// Treat the response as successful and remember the endpoint.
    Success,

    /// Stop retrying and return failure.
    Stop,
  };

  /// Builds a URL for a configured endpoint.
  using EndpointUrlBuilder = std::function<String(const ServerEndpoint &)>;

  /// Handles one configured-endpoint HTTP response.
  using HttpGetHandler =
      std::function<HttpGetDecision(const HttpGetResponse &)>;

  /// Active WebSocket client connection to the server.
  websockets::WebsocketsClient _ws;

  /// Optional internal endpoint preference store used when no app callback owns it.
  Preferences _prefs;

  /// Registered callback bundle.
  LiveSessionCallbacks _callbacks;

  /// Current conversation identifier.
  String _chatId;

  /// Current selected voice identifier.
  String _voice;

  /// Whether the WebSocket is currently connected.
  bool _connected = false;

  /// Ring buffer of log lines queued for shipping to the server. Sized to
  /// hold a full boot's burst of lines until the WebSocket comes up.
  static constexpr int kLogShipCapacity = 64;

  /// Max lines per client_log message.
  static constexpr int kLogShipBatchMax = 8;

  /// Minimum interval between client_log messages.
  static constexpr unsigned long kLogShipIntervalMs = 1000;

  /// Queued log lines (ring buffer; oldest overwritten when full).
  String _logShipBuffer[kLogShipCapacity];

  /// Next write index into _logShipBuffer.
  int _logShipHead = 0;

  /// Number of queued (unshipped) lines.
  int _logShipCount = 0;

  /// Lines lost to ring overflow since the last successful ship.
  unsigned int _logShipDropped = 0;

  /// Timestamp of the last client_log message.
  unsigned long _lastLogShipMs = 0;

  /// Guard so lines logged while shipping don't recurse into the queue.
  bool _shippingLogs = false;

  /// Send a batch of queued log lines to the server (rate-limited).
  void flushClientLogs();

  /// Whether internal Preferences storage is available.
  bool _prefsReady = false;

  /// Whether the app explicitly set the preferred endpoint before init.
  bool _preferredEndpointSetExternally = false;

  /// Timestamp of the last reconnect attempt.
  unsigned long _lastReconnectMs = 0;

  /// Next endpoint index to try when reconnecting.
  int _nextServerIndex = 0;

  /// Index of the currently active endpoint, or -1 when disconnected.
  int _activeServerIndex = -1;

  /// Delay between reconnect attempts.
  static constexpr unsigned long kReconnectMs = 5000;

  /// Preferences namespace for fallback endpoint persistence.
  static constexpr const char *kPrefsNamespace = "live";

  /// Preferences key for the last successful server endpoint.
  static constexpr const char *kLastServerIndexKey = "server";

  /// Handle an incoming WebSocket message.
  void handleMessage(websockets::WebsocketsMessage msg);

  /// Handle a WebSocket connection event.
  void handleEvent(websockets::WebsocketsEvent event, String data);

  /// Dispatch a tool call payload from the server.
  void handleToolCall(const ArduinoJson::JsonDocument &doc);

  /**
   * @brief Send a tool response back to the server.
   * @param name Tool name.
   * @param id Tool call identifier.
   * @param result Serialized tool result payload.
   */
  void sendToolResponse(const char *name, const char *id, const String &result);

  /**
   * @brief Build the HTTP base URL for a configured server endpoint.
   * @param endpoint Endpoint configuration.
   * @return Base URL string.
   */
  String endpointBaseUrl(const ServerEndpoint &endpoint) const;

  /**
   * @brief GET a URL using secure or insecure client setup from the endpoint.
   * @param endpoint Endpoint configuration associated with the URL.
   * @param url Full URL to request.
   * @param statusCode Receives the HTTP status code.
   * @param body Receives the response body when available.
   * @return True when the request was started and completed at HTTP level.
   */
  bool performEndpointGet(const ServerEndpoint &endpoint, const String &url,
                          int &statusCode, String &body) const;

  /**
   * @brief Try a GET request across configured endpoints.
   * @param logAction Phrase logged before each URL, e.g. "fetching from".
   * @param buildUrl Builds the URL for the current endpoint.
   * @param handleResponse Parses or rejects the response.
   * @return True when handleResponse accepts a response.
   */
  bool getFromConfiguredEndpoints(const char *logAction,
                                  const EndpointUrlBuilder &buildUrl,
                                  const HttpGetHandler &handleResponse);

  /**
   * @brief Remember the endpoint that most recently worked.
   * @param endpointIndex Index into SERVER_ENDPOINTS.
   */
  void rememberSuccessfulEndpoint(int endpointIndex);

  /// Emit a client-side log line.
  void logClient(const char *topic, const char *fmt, ...) const;

  /// Emit a server-side log line.
  void logServer(const char *topic, const char *fmt, ...) const;
};
