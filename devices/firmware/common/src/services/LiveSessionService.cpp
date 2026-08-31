#include "LiveSessionService.h"

#include "credentials.h"
#include "hal/BoardPower.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <stdarg.h>

using namespace websockets;

namespace {
/// Timeout for short HTTP metadata requests.
constexpr uint16_t kHttpGetTimeoutMs = 3000;

/**
 * @brief Whether a device-auth token is compiled into credentials.h.
 */
bool hasDeviceAuthToken() { return DEVICE_AUTH_TOKEN && DEVICE_AUTH_TOKEN[0]; }

/**
 * @brief Add the current device-auth header when configured.
 */
void addDeviceAuthHeader(HTTPClient &http) {
  if (hasDeviceAuthToken()) {
    http.addHeader("X-Device-Token", DEVICE_AUTH_TOKEN);
  }
}

/**
 * @brief Emit one log line through the optional callback or Serial fallback.
 */
void writeLog(const LiveSessionCallbacks &callbacks, char side,
              const char *topic, const char *fmt, va_list args) {
  char message[256];
  vsnprintf(message, sizeof(message), fmt, args);
  if (callbacks.onLog) {
    callbacks.onLog(side, topic, message);
  } else {
    Serial.printf("[%s] %s\n", topic, message);
  }
}

/**
 * @brief Emit a client-side log line from a helper function.
 */
void logClient(const LiveSessionCallbacks &callbacks, const char *topic,
               const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  writeLog(callbacks, 'C', topic, fmt, args);
  va_end(args);
}

/**
 * @brief Parse an id/name timer selector from a tool-call payload.
 */
TimerRef parseTimerRef(const JsonDocument &doc) {
  TimerRef ref;
  if (doc["args"]["id"].is<uint32_t>() || doc["args"]["id"].is<int>()) {
    const int raw = doc["args"]["id"].as<int>();
    if (raw > 0) ref.id = static_cast<uint32_t>(raw);
  }
  const char *timerName = doc["args"]["name"];
  if (timerName) ref.name = String(timerName);
  return ref;
}

/**
 * @brief Decode a standard base64 string into a byte buffer.
 * @param in Pointer to the base64 input characters.
 * @param inLen Number of input characters.
 * @param out Output buffer that receives the decoded bytes.
 * @param outCap Capacity of @p out in bytes.
 * @return Number of decoded bytes, or -1 if the input is malformed.
 */
int decodeBase64(const char *in, size_t inLen, uint8_t *out, size_t outCap) {
  static const int8_t T[256] = {
      // build table at runtime once below
      -1};
  static bool tableReady = false;
  static int8_t table[256];
  if (!tableReady) {
    for (int i = 0; i < 256; i++) table[i] = -1;
    const char *abc =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < 64; i++) table[(uint8_t)abc[i]] = i;
    tableReady = true;
  }
  (void)T;

  uint32_t buf = 0;
  int bits = 0;
  size_t outLen = 0;
  for (size_t i = 0; i < inLen; i++) {
    const char c = in[i];
    if (c == '=' || c == '\0') break;
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
    const int8_t v = table[(uint8_t)c];
    if (v < 0) return -1;
    buf = (buf << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (outLen >= outCap) return -1;
      out[outLen++] = (uint8_t)((buf >> bits) & 0xFF);
    }
  }
  return (int)outLen;
}

/**
 * @brief Whether a URL uses the https scheme.
 * @param url URL to inspect.
 * @return True when the URL begins with "https://".
 */
bool urlUsesHttps(const String &url) { return url.startsWith("https://"); }

/**
 * @brief Strip any scheme prefix and path from a configured endpoint host.
 * @param rawHost Host string as configured in ServerEndpoint::host.
 * @return Bare hostname (still possibly including a :port suffix).
 */
String stripEndpointScheme(const char *rawHost) {
  String host = rawHost ? String(rawHost) : String("");
  if (host.startsWith("http://")) {
    host = host.substring(7);
  } else if (host.startsWith("https://")) {
    host = host.substring(8);
  } else if (host.startsWith("ws://")) {
    host = host.substring(5);
  } else if (host.startsWith("wss://")) {
    host = host.substring(6);
  }

  const int pathIndex = host.indexOf('/');
  if (pathIndex >= 0) {
    host = host.substring(0, pathIndex);
  }
  return host;
}

/**
 * @brief Resolve the hostname (without scheme or port) used to dial an endpoint.
 * @param endpoint Endpoint configuration.
 * @return Host string suitable for WebSocket or HTTP connection.
 */
String endpointHostForConnection(const ServerEndpoint &endpoint) {
  String host = stripEndpointScheme(endpoint.host);
  const int colonIndex = host.lastIndexOf(':');
  if (colonIndex > 0 && host.indexOf(':') == colonIndex) {
    bool portIsNumeric = true;
    for (int i = colonIndex + 1; i < host.length(); i++) {
      if (!isDigit(host.charAt(i))) {
        portIsNumeric = false;
        break;
      }
    }
    if (portIsNumeric) {
      host = host.substring(0, colonIndex);
    }
  }
  return host;
}

/**
 * @brief Pick "http" or "https" for an endpoint based on its host and port.
 * @param endpoint Endpoint configuration.
 * @return Scheme literal "http" or "https".
 */
const char *httpSchemeForEndpoint(const ServerEndpoint &endpoint) {
  const String rawHost = endpoint.host ? String(endpoint.host) : String("");
  if (rawHost.startsWith("https://")) {
    return "https";
  }
  if (rawHost.startsWith("http://")) {
    return "http";
  }
  return endpoint.port == 443 ? "https" : "http";
}

/**
 * @brief Pick "ws" or "wss" for an endpoint based on its HTTP scheme.
 * @param endpoint Endpoint configuration.
 * @return Scheme literal "ws" or "wss".
 */
const char *wsSchemeForEndpoint(const ServerEndpoint &endpoint) {
  return strcmp(httpSchemeForEndpoint(endpoint), "https") == 0 ? "wss" : "ws";
}

/**
 * @brief Look up a pinned CA certificate for an https URL.
 * @param url Full URL to dial.
 * @return CA certificate string, or nullptr when no matching endpoint is found.
 */
const char *caCertForUrl(const String &url) {
  for (int i = 0; i < SERVER_ENDPOINT_COUNT; i++) {
    const ServerEndpoint &endpoint = SERVER_ENDPOINTS[i];
    const String httpsPrefix =
        String("https://") + endpointHostForConnection(endpoint);
    if (url == httpsPrefix || url.startsWith(httpsPrefix + "/") ||
        url.startsWith(httpsPrefix + ":")) {
      return endpoint.ca_cert;
    }
  }
  return nullptr;
}

/**
 * @brief Format the most recent OTA error from the Update library.
 * @return Error string, or "unknown update error" when none is reported.
 */
String updateError() {
  const char *error = Update.errorString();
  return error && error[0] ? String(error) : String("unknown update error");
}

/**
 * @brief Stream an OTA payload through the Update library to completion.
 * @param http Active HTTP client positioned at the firmware download.
 * @param outError Receives a human-readable error message on failure.
 * @return True when the firmware was written, ended, and finished cleanly.
 */
bool runFirmwareUpdateDownload(HTTPClient &http, String &outError,
                               const LiveSessionCallbacks &callbacks) {
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    outError = "Download failed: HTTP " + String(statusCode);
    logClient(callbacks, "OTA", "download failed status=%d", statusCode);
    return false;
  }

  const int contentLength = http.getSize();
  if (contentLength <= 0) {
    outError = "Missing firmware size";
    logClient(callbacks, "OTA", "missing content length=%d", contentLength);
    return false;
  }

  logClient(callbacks, "OTA", "downloading firmware bytes=%d", contentLength);
  if (!Update.begin(contentLength, U_FLASH)) {
    outError = "Update begin failed: " + updateError();
    logClient(callbacks, "OTA", "Update.begin failed: %s", outError.c_str());
    return false;
  }

  Stream *stream = http.getStreamPtr();
  const size_t written = Update.writeStream(*stream);
  if (written != static_cast<size_t>(contentLength)) {
    outError = "Write failed: " + String(written) + "/" + String(contentLength);
    const String error = updateError();
    if (error.length()) {
      outError += " " + error;
    }
    logClient(callbacks, "OTA",
              "Update.writeStream failed wrote=%u expected=%d err=%s",
              static_cast<unsigned>(written), contentLength,
              updateError().c_str());
    Update.abort();
    return false;
  }

  if (!Update.end()) {
    outError = "Update end failed: " + updateError();
    logClient(callbacks, "OTA", "Update.end failed: %s", outError.c_str());
    return false;
  }

  if (!Update.isFinished()) {
    outError = "Update incomplete";
    logClient(callbacks, "OTA", "update incomplete");
    return false;
  }

  logClient(callbacks, "OTA", "firmware update installed");
  return true;
}
} // namespace

/**
 * @brief Store the callback bundle and wire up WebSocket event handlers.
 * @param callbacks Callback hooks for session events and tool actions.
 */
void LiveSessionService::init(const LiveSessionCallbacks &callbacks) {
  _callbacks = callbacks;
  if (!_preferredEndpointSetExternally && !_callbacks.onEndpointIndexChanged) {
    _prefsReady = _prefs.begin(kPrefsNamespace, false);
    if (_prefsReady) {
      const int savedIndex = _prefs.getInt(kLastServerIndexKey, 0);
      if (savedIndex >= 0 && savedIndex < SERVER_ENDPOINT_COUNT) {
        _nextServerIndex = savedIndex;
        logClient("WS", "loaded preferred server index %d", _nextServerIndex);
      }
    } else {
      logClient("WS", "preferences init failed; server preference disabled");
    }
  }

  if (hasDeviceAuthToken()) {
    _ws.addHeader("X-Device-Token", DEVICE_AUTH_TOKEN);
  }
  _ws.onMessage([this](WebsocketsMessage msg) { handleMessage(msg); });
  _ws.onEvent(
      [this](WebsocketsEvent event, String data) { handleEvent(event, data); });
}

/**
 * @brief Open the WebSocket to the next server endpoint in rotation.
 */
void LiveSessionService::connect() {
  _activeServerIndex = _nextServerIndex;
  const ServerEndpoint &endpoint = SERVER_ENDPOINTS[_nextServerIndex];
  _nextServerIndex = (_nextServerIndex + 1) % SERVER_ENDPOINT_COUNT;

  const String path = String(SERVER_PATH) + "?device_id=" + DEVICE_ID;
  const String chatQuery = _chatId.isEmpty() ? "" : "&chat_id=" + _chatId;
  const String voiceQuery = _voice.isEmpty() ? "" : "&voice=" + _voice;
  // Tell the server what pixel size to dither generated images to. Different
  // devices have different display sizes; see IMAGE_TARGET_* in Config.h.
  const String imageSizeQuery =
      "&image_w=" + String(IMAGE_TARGET_WIDTH) +
      "&image_h=" + String(IMAGE_TARGET_HEIGHT);
  const String fullPath = path + chatQuery + voiceQuery + imageSizeQuery;
  const String host = endpointHostForConnection(endpoint);
  const char *scheme = wsSchemeForEndpoint(endpoint);

  if (_callbacks.onStatus) {
    _callbacks.onStatus("Connecting...");
  }

  logClient("WS", "connecting %s://%s:%d%s", scheme, host.c_str(),
              endpoint.port, fullPath.c_str());

  if (endpoint.ca_cert) {
    _ws.setCACert(endpoint.ca_cert);
  } else {
    _ws.setInsecure();
  }

  if (endpoint.port == 443) {
    _connected =
        _ws.connectSecure(host.c_str(), endpoint.port, fullPath.c_str());
  } else {
    _connected = _ws.connect(host.c_str(), endpoint.port, fullPath.c_str());
  }

  if (!_connected) {
    logClient("WS", "connect failed %s://%s:%d; will retry", scheme,
                host.c_str(), endpoint.port);
  }
}

/**
 * @brief Close the active WebSocket connection if one is open.
 */
void LiveSessionService::disconnect() {
  if (_ws.available()) {
    _ws.close();
  }
  _connected = false;
}

/**
 * @brief Bias the next connect attempt toward a specific server endpoint.
 * @param endpointIndex Index into SERVER_ENDPOINTS.
 */
void LiveSessionService::setPreferredEndpointIndex(int endpointIndex) {
  if (endpointIndex < 0 || endpointIndex >= SERVER_ENDPOINT_COUNT) {
    return;
  }
  _preferredEndpointSetExternally = true;
  _nextServerIndex = endpointIndex;
}

/**
 * @brief Pump the WebSocket client to drive callbacks.
 */
void LiveSessionService::poll() {
  if (_connected) {
    _ws.poll();
    flushClientLogs();
  }
}

/**
 * @brief Queue one log line for shipping to the server.
 *
 * Lines land in the server's device_logs table so post-mortems (battery
 * readings, breadcrumbs, reset reasons) survive an abrupt device power loss
 * and can be read back by the model via the read_device_logs tool.
 */
void LiveSessionService::noteLog(char side, const char *topic,
                                 const char *message) {
  if (_shippingLogs) {
    return; // Ignore lines emitted while a batch is being sent.
  }
  // Per-frame display diagnostics fire many times a second; shipping them
  // would drown the useful lines and waste bandwidth.
  if (strcmp(topic, "Render") == 0 || strcmp(topic, "Flush") == 0) {
    return;
  }
  char prefix[20];
  snprintf(prefix, sizeof(prefix), "%lus %c ",
           static_cast<unsigned long>(millis() / 1000), side);
  String line;
  line.reserve(strlen(prefix) + strlen(topic) + 2 + strlen(message));
  line += prefix;
  line += topic;
  line += ": ";
  line += message;

  _logShipBuffer[_logShipHead] = line;
  _logShipHead = (_logShipHead + 1) % kLogShipCapacity;
  if (_logShipCount < kLogShipCapacity) {
    _logShipCount++;
  } else {
    _logShipDropped++;
  }
}

/**
 * @brief Send a batch of queued log lines to the server (rate-limited).
 */
void LiveSessionService::flushClientLogs() {
  if (_logShipCount == 0) {
    return;
  }
  const unsigned long now = millis();
  if (now - _lastLogShipMs < kLogShipIntervalMs) {
    return;
  }
  _lastLogShipMs = now;
  _shippingLogs = true;

  JsonDocument doc;
  doc["type"] = "client_log";
  if (_logShipDropped > 0) {
    doc["dropped"] = _logShipDropped;
  }
  JsonArray lines = doc["lines"].to<JsonArray>();
  const int batch = min(_logShipCount, kLogShipBatchMax);
  int idx = (_logShipHead - _logShipCount + 2 * kLogShipCapacity) %
            kLogShipCapacity;
  for (int i = 0; i < batch; i++) {
    lines.add(_logShipBuffer[idx]);
    idx = (idx + 1) % kLogShipCapacity;
  }
  String encoded;
  serializeJson(doc, encoded);
  if (_ws.send(encoded.c_str())) {
    _logShipCount -= batch;
    _logShipDropped = 0;
  }
  _shippingLogs = false;
}

/**
 * @brief Reconnect when disconnected and reconnects are currently allowed.
 * @param enabled Whether reconnect behavior is currently enabled.
 */
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

/**
 * @brief Human-readable label for the currently active endpoint.
 * @return "dev" for the local worker, "prod" otherwise, or "no server".
 */
String LiveSessionService::activeEndpointLabel() const {
  if (_activeServerIndex < 0 || _activeServerIndex >= SERVER_ENDPOINT_COUNT) {
    return "no server";
  }
  // Index 0 is the local dev worker; anything else is treated as prod.
  return _activeServerIndex == 0 ? "dev" : "prod";
}

/**
 * @brief Verify that at least one configured endpoint responds to /ping.
 * @return True when a configured endpoint responds with "pong".
 */
bool LiveSessionService::pingServer() {
  return getFromConfiguredEndpoints(
      "pinging server at",
      [this](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) + "/ping?device_id=" + DEVICE_ID;
      },
      [this](const HttpGetResponse &response) {
        String body = response.body;
        body.trim();
        if (response.statusCode == 200 && body == "pong") {
          logClient("HTTP", "server ping OK endpoint=%d",
                    response.endpointIndex);
          return HttpGetDecision::Success;
        }

        logClient("HTTP", "server ping failed status=%d body=%s",
                  response.statusCode, body.c_str());
        return HttpGetDecision::Continue;
      });
}

/**
 * @brief Send a start-of-turn JSON message to the server.
 * @return True when the message was sent.
 */
bool LiveSessionService::sendStart() { return _ws.send("{\"type\":\"start\"}"); }

/**
 * @brief Send an end-of-turn JSON message to the server.
 * @return True when the message was sent.
 */
bool LiveSessionService::sendStop() { return _ws.send("{\"type\":\"stop\"}"); }

/**
 * @brief Send a chunk of raw microphone PCM as a binary WebSocket frame.
 * @param data PCM samples.
 * @param len Number of bytes to send.
 * @return True when the frame was queued to the socket.
 */
bool LiveSessionService::sendAudio(const int16_t *data, size_t len) {
  return _ws.sendBinary(reinterpret_cast<const char *>(data), len);
}

/**
 * @brief Fetch the last assistant message for the current chat from the server.
 * @param outMessage Receives the last assistant message text.
 * @return True when a message was retrieved.
 */
bool LiveSessionService::fetchLastAssistantMessage(String &outMessage) {
  outMessage = "";
  if (_chatId.isEmpty()) {
    return false;
  }

  return getFromConfiguredEndpoints(
      "restoring session from",
      [this](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) + "/session/" + _chatId +
               "?device_id=" + DEVICE_ID;
      },
      [this, &outMessage](const HttpGetResponse &response) {
        if (response.statusCode == 404) {
          logClient("HTTP", "session not found at endpoint %d",
                      response.endpointIndex);
          return HttpGetDecision::Continue;
        }

        if (response.statusCode != 200 || response.body.isEmpty()) {
          logClient("HTTP", "session restore failed status=%d",
                      response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body)) {
          logClient("HTTP", "session restore returned invalid JSON");
          return HttpGetDecision::Continue;
        }

        const char *lastMessage = doc["last_message"];
        if (!lastMessage || !lastMessage[0]) {
          return HttpGetDecision::Stop;
        }

        outMessage = lastMessage;
        return HttpGetDecision::Success;
      });
}

/**
 * @brief Fetch recent conversation summaries for this device.
 * @param outEntries Output array for summary entries.
 * @param maxEntries Capacity of @p outEntries.
 * @param outCount Receives the number of entries written.
 * @return True when the server returned a valid history payload.
 */
bool LiveSessionService::fetchConversationHistory(ConversationSummary outEntries[],
                                                  int maxEntries,
                                                  int &outCount) {
  outCount = 0;
  if (maxEntries <= 0) {
    return false;
  }

  return getFromConfiguredEndpoints(
      "fetching history from",
      [this](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) + "/history/" + DEVICE_ID +
               "?device_id=" + DEVICE_ID;
      },
      [this, outEntries, maxEntries, &outCount](
          const HttpGetResponse &response) {
        if (response.statusCode != 200 || response.body.isEmpty()) {
          logClient("HTTP", "history fetch failed status=%d",
                      response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body) || !doc.is<JsonArray>()) {
          logClient("HTTP", "history response invalid");
          return HttpGetDecision::Continue;
        }

        JsonArray rows = doc.as<JsonArray>();
        for (JsonVariant row : rows) {
          if (outCount >= maxEntries) {
            break;
          }

          const char *chatId = row["chat_id"];
          if (!chatId || !chatId[0]) {
            continue;
          }

          outEntries[outCount].chatId = chatId;
          outEntries[outCount].lastMessage = row["last_message"] | "";
          outEntries[outCount].updatedAt = row["updated_at"] | "";
          outCount++;
        }
        return HttpGetDecision::Success;
      });
}

/**
 * @brief Ask the server whether a newer firmware build is available.
 * @param outInfo Receives firmware update metadata.
 * @return True when the server responded with a parseable payload.
 */
bool LiveSessionService::checkFirmwareUpdate(FirmwareUpdateInfo &outInfo) {
  outInfo = FirmwareUpdateInfo{};

  return getFromConfiguredEndpoints(
      "checking firmware at",
      [this](const ServerEndpoint &endpoint) {
        return endpointBaseUrl(endpoint) + "/firmware/check?version=" +
               String(FIRMWARE_VERSION) + "&device=" + FIRMWARE_DEVICE;
      },
      [this, &outInfo](const HttpGetResponse &response) {
        if (response.statusCode == 404) {
          return HttpGetDecision::Continue;
        }

        if (response.statusCode != 200 || response.body.isEmpty()) {
          logClient("HTTP", "firmware check failed status=%d",
                      response.statusCode);
          return HttpGetDecision::Continue;
        }

        JsonDocument doc;
        if (deserializeJson(doc, response.body)) {
          logClient("HTTP", "firmware check invalid JSON");
          return HttpGetDecision::Continue;
        }

        outInfo.available = doc["available"] | false;
        outInfo.latestVersion = doc["latest_version"] | FIRMWARE_VERSION;
        outInfo.notes = doc["notes"] | "";
        outInfo.downloadUrl = doc["download_url"] | "";
        return HttpGetDecision::Success;
      });
}

/**
 * @brief Download an OTA firmware payload and apply it via the Update library.
 * @param downloadUrl URL of the OTA binary.
 * @param outError Receives a human-readable error message on failure.
 * @return True when the update was installed successfully.
 */
bool LiveSessionService::downloadAndApplyFirmwareUpdate(
    const String &downloadUrl, String &outError) {
  outError = "";

  if (downloadUrl.isEmpty()) {
    outError = "No download URL";
    return false;
  }

  logClient("OTA", "downloading update from %s", downloadUrl.c_str());
  disconnect();
  delay(100);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(60000);

  if (urlUsesHttps(downloadUrl)) {
    WiFiClientSecure client;
    const char *caCert = caCertForUrl(downloadUrl);
    if (caCert) {
      client.setCACert(caCert);
    } else {
      client.setInsecure();
    }

    if (!http.begin(client, downloadUrl)) {
      outError = "Could not start download";
      return false;
    }

    const bool ok = runFirmwareUpdateDownload(http, outError, _callbacks);
    http.end();
    return ok;
  }

  WiFiClient client;
  if (!http.begin(client, downloadUrl)) {
    outError = "Could not start download";
    return false;
  }

  const bool ok = runFirmwareUpdateDownload(http, outError, _callbacks);
  http.end();
  return ok;
}

/**
 * @brief React to WebSocket lifecycle events.
 * @param event Lifecycle event from the WebSocket client.
 * @param data Event-specific payload (e.g. close reason).
 */
void LiveSessionService::handleEvent(WebsocketsEvent event, String data) {
  switch (event) {
  case WebsocketsEvent::ConnectionOpened:
    logClient("WS", "opened");
    _connected = true;
    rememberSuccessfulEndpoint(_activeServerIndex);
    if (_callbacks.onStatus) {
      _callbacks.onStatus("Waiting for AI...");
    }
    break;

  case WebsocketsEvent::ConnectionClosed:
    logClient("WS", "closed: %s", data.c_str());
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

/**
 * @brief Dispatch an inbound WebSocket message to the appropriate callback.
 * @param msg Inbound message (binary audio or JSON control frame).
 */
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

  if (strcmp(type, "server_ready") == 0) {
    logServer("Device", "channel ready");
    if (_callbacks.onServerReady) {
      _callbacks.onServerReady();
    }
    return;
  }

  if (strcmp(type, "session") == 0) {
    const char *id = doc["chatId"];
    if (id && _callbacks.onChatId) {
      _callbacks.onChatId(id);
    }
    if ((doc["reset"] | false) && _callbacks.onConversationReset) {
      _callbacks.onConversationReset();
    }
    return;
  }

  if (strcmp(type, "ready") == 0) {
    logServer("Gemini", "session ready");
    if (_callbacks.onReady) {
      _callbacks.onReady();
    }
    return;
  }

  if (strcmp(type, "turn_complete") == 0) {
    logServer("Gemini", "turn complete");
    if (_callbacks.onTurnComplete) {
      _callbacks.onTurnComplete();
    }
    return;
  }

  if (strcmp(type, "drop_audio") == 0) {
    logServer("Gemini", "drop audio interrupted");
    if (_callbacks.onDropAudio) {
      _callbacks.onDropAudio();
    }
    return;
  }

  if (strcmp(type, "settings") == 0) {
    if (_callbacks.onPowerTimeouts) {
      _callbacks.onPowerTimeouts(doc["power"]["dim_ms"] | IDLE_DIM_MS,
                                 doc["power"]["screen_off_ms"] |
                                     IDLE_SCREEN_OFF_MS,
                                 doc["power"]["light_sleep_ms"] |
                                     IDLE_LIGHT_SLEEP_MS,
                                 doc["power"]["power_off_ms"] |
                                     IDLE_POWER_OFF_MS);
    }
    return;
  }

  if (strcmp(type, "tool_call") == 0) {
    handleToolCall(doc);
    return;
  }

  if (strcmp(type, "transcript") == 0) {
    const char *source = doc["source"];
    const char *text = doc["text"];
    if (!source || strcmp(source, "model") != 0) {
      logServer("Transcript", "%s: %s", source ? source : "?",
                  text ? text : "");
    }
    if (_callbacks.onTranscript && source && text) {
      _callbacks.onTranscript(String(source), String(text));
    }
    return;
  }

  if (strcmp(type, "error") == 0) {
    const char *category = doc["category"];
    const char *message = doc["message"];
    logServer("Error", "[%s] %s", category ? category : "server",
                message ? message : "unknown");
    if (_callbacks.onError) {
      _callbacks.onError(category ? category : "server",
                         message ? message : "Server error");
    }
    return;
  }

  if (strcmp(type, "ignore_audio") == 0) {
    const char *reason = doc["reason"];
    const int bytes = doc["bytes"] | -1;
    const int avgAbs = doc["avg_abs"] | -1;
    const int chunks = doc["chunks"] | -1;
    logServer("Gemini", "ignored audio: %s bytes=%d avg_abs=%d chunks=%d",
                reason ? reason : "ignored", bytes, avgAbs, chunks);
    if (_callbacks.onIgnoredAudio) {
      _callbacks.onIgnoredAudio(reason ? reason : "ignored");
    }
    return;
  }

  if (strcmp(type, "show_image_pending") == 0) {
    if (_callbacks.onShowImagePending) {
      _callbacks.onShowImagePending();
    }
    return;
  }

  if (strcmp(type, "show_image_failed") == 0) {
    logServer("Image", "generation failed");
    if (_callbacks.onShowImageFailed) {
      _callbacks.onShowImageFailed();
    }
    return;
  }

  if (strcmp(type, "show_image") == 0) {
    const char *data = doc["data"];
    const int width = doc["width"] | 0;
    const int height = doc["height"] | 0;
    if (!data || width <= 0 || height <= 0) {
      logServer("Image", "show_image missing data or dimensions");
      return;
    }
    const size_t encodedLen = strlen(data);
    const size_t expectedBytes = (size_t)((width * height + 7) / 8);
    uint8_t *packed = (uint8_t *)malloc(expectedBytes);
    if (!packed) {
      logServer("Image", "failed to allocate decode buffer");
      if (_callbacks.onShowImageFailed) _callbacks.onShowImageFailed();
      return;
    }
    const int decoded = decodeBase64(data, encodedLen, packed, expectedBytes);
    if (decoded < 0 || (size_t)decoded < expectedBytes) {
      logServer("Image", "decode failed got=%d expected=%u", decoded,
                  (unsigned)expectedBytes);
      free(packed);
      if (_callbacks.onShowImageFailed) _callbacks.onShowImageFailed();
      return;
    }
    logServer("Image", "show_image %dx%d bytes=%u", width, height,
                (unsigned)expectedBytes);
    if (_callbacks.onShowImage) {
      _callbacks.onShowImage(packed, expectedBytes, width, height);
    }
    free(packed);
    return;
  }

  if (strcmp(type, "animation_frame") == 0) {
    const char *data = doc["data"];
    const int width = doc["width"] | 0;
    const int height = doc["height"] | 0;
    const int index = doc["index"] | 0;
    const int intervalMs = doc["interval_ms"] | 500;
    if (!data || width <= 0 || height <= 0 || index <= 0) {
      logServer("Image", "animation_frame missing data or dimensions");
      return;
    }
    const size_t encodedLen = strlen(data);
    const size_t expectedBytes = (size_t)((width * height + 7) / 8);
    uint8_t *packed = (uint8_t *)malloc(expectedBytes);
    if (!packed) {
      logServer("Image", "failed to allocate animation frame buffer");
      return;
    }
    const int decoded = decodeBase64(data, encodedLen, packed, expectedBytes);
    if (decoded < 0 || (size_t)decoded < expectedBytes) {
      logServer("Image", "animation frame decode failed got=%d expected=%u",
                  decoded, (unsigned)expectedBytes);
      free(packed);
      return;
    }
    logServer("Image", "animation_frame %d %dx%d bytes=%u interval=%dms",
                index, width, height, (unsigned)expectedBytes, intervalMs);
    if (_callbacks.onAnimationFrame) {
      _callbacks.onAnimationFrame(packed, expectedBytes, width, height, index,
                                  intervalMs);
    }
    free(packed);
    return;
  }

  if (strcmp(type, "voice_changed") == 0) {
    const char *voice = doc["voice"];
    if (voice && *voice) {
      _voice = voice;
      logServer("Voice", "changed to %s", voice);
      if (_callbacks.onVoiceChanged) {
        _callbacks.onVoiceChanged(String(voice));
      }
    }
  }
}

/**
 * @brief Execute a device-side tool call and send the response back.
 * @param doc Parsed tool-call JSON payload from the server.
 */
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
  } else if (strcmp(name, "set_speaker") == 0) {
    const char *mode = doc["args"]["mode"];
    if (!Board::capabilities().externalSpeakerSwitch) {
      result = "Speaker switching unavailable on this device";
    } else if (!mode) {
      result = "Missing mode (use 'internal' or 'external')";
    } else if (!_callbacks.onSetSpeaker) {
      result = "Speaker control unavailable";
    } else if (_callbacks.onSetSpeaker(mode)) {
      result = String("Speaker set to ") + mode;
    } else {
      result = "Invalid mode (use 'internal' or 'external')";
    }
  } else if (strcmp(name, "set_external_speaker_gain") == 0) {
    if (!Board::capabilities().externalSpeakerGain) {
      result = "External speaker gain unavailable on this device";
    } else if (!doc["args"]["gain"].is<int>()) {
      result = "Missing gain (integer 1..64)";
    } else if (!_callbacks.onSetExternalGain) {
      result = "Gain control unavailable";
    } else {
      const int gain = doc["args"]["gain"].as<int>();
      if (_callbacks.onSetExternalGain(gain)) {
        result = String("External speaker gain set to ") + constrain(gain, 1, 64);
      } else {
        result = "Gain out of range (use 1..64)";
      }
    }
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
  } else if (strcmp(name, "set_timer") == 0) {
    if (!_callbacks.onSetTimer) {
      result = "Timer control unavailable";
    } else {
      const int duration = doc["args"]["duration_seconds"].is<int>()
                               ? doc["args"]["duration_seconds"].as<int>()
                               : 0;
      const char *timerName = doc["args"]["name"];
      result = _callbacks.onSetTimer(
          duration, timerName ? String(timerName) : String(""));
    }
  } else if (strcmp(name, "list_timers") == 0) {
    result =
        _callbacks.onListTimers ? _callbacks.onListTimers() : "{\"timers\":[]}";
  } else if (strcmp(name, "cancel_timer") == 0) {
    if (!_callbacks.onCancelTimer) {
      result = "Timer control unavailable";
    } else {
      const TimerRef ref = parseTimerRef(doc);
      const bool all = doc["args"]["all"].is<bool>()
                           ? doc["args"]["all"].as<bool>()
                           : false;
      result = _callbacks.onCancelTimer(ref, all);
    }
  } else if (strcmp(name, "extend_timer") == 0) {
    if (!_callbacks.onExtendTimer) {
      result = "Timer control unavailable";
    } else if (!doc["args"]["delta_seconds"].is<int>()) {
      result = "Missing delta_seconds";
    } else {
      const int delta = doc["args"]["delta_seconds"].as<int>();
      const TimerRef ref = parseTimerRef(doc);
      result = _callbacks.onExtendTimer(delta, ref);
    }
  }

  sendToolResponse(name, id, result);
}

/**
 * @brief Serialize and send a tool response back to the server.
 * @param name Tool name being responded to.
 * @param id Tool call identifier the server provided.
 * @param result Serialized tool result payload.
 */
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
  logServer("Tool", "%s -> %s", name, result.c_str());
}

/**
 * @brief Build the HTTP base URL for a configured server endpoint.
 * @param endpoint Endpoint configuration.
 * @return Base URL like "https://host" or "http://host:port".
 */
String LiveSessionService::endpointBaseUrl(const ServerEndpoint &endpoint) const {
  const char *scheme = httpSchemeForEndpoint(endpoint);
  String url = String(scheme) + "://" + endpointHostForConnection(endpoint);
  if (endpoint.port != 80 && endpoint.port != 443) {
    url += ":" + String(endpoint.port);
  }
  return url;
}

/**
 * @brief Issue a single GET request using the right secure/insecure client.
 * @param endpoint Endpoint associated with @p url for CA pinning.
 * @param url Full URL to request.
 * @param statusCode Receives the HTTP status code on completion.
 * @param body Receives the response body when one is available.
 * @return True when the request completed at the HTTP layer.
 */
bool LiveSessionService::performEndpointGet(const ServerEndpoint &endpoint,
                                            const String &url,
                                            int &statusCode,
                                            String &body) const {
  statusCode = -1;
  body = "";

  HTTPClient http;
  http.setTimeout(kHttpGetTimeoutMs);
  if (urlUsesHttps(url)) {
    WiFiClientSecure client;
    if (endpoint.ca_cert) {
      client.setCACert(endpoint.ca_cert);
    } else {
      client.setInsecure();
    }

    if (!http.begin(client, url)) {
      logClient("HTTP", "begin failed for %s", url.c_str());
      return false;
    }

    addDeviceAuthHeader(http);
    statusCode = http.GET();
    if (statusCode > 0) {
      body = http.getString();
    }
    http.end();
    return true;
  }

  WiFiClient client;
  if (!http.begin(client, url)) {
    logClient("HTTP", "begin failed for %s", url.c_str());
    return false;
  }

  addDeviceAuthHeader(http);
  statusCode = http.GET();
  if (statusCode > 0) {
    body = http.getString();
  }
  http.end();
  return true;
}

/**
 * @brief Iterate configured endpoints, GET each URL, and let the caller decide.
 * @param logAction Phrase logged before each URL (e.g. "fetching from").
 * @param buildUrl Builds the URL to request for the current endpoint.
 * @param handleResponse Inspects each response and reports success/continue/stop.
 * @return True when @p handleResponse accepted a response.
 */
bool LiveSessionService::getFromConfiguredEndpoints(
    const char *logAction, const EndpointUrlBuilder &buildUrl,
    const HttpGetHandler &handleResponse) {
  for (int offset = 0; offset < SERVER_ENDPOINT_COUNT; offset++) {
    const int index = (_nextServerIndex + offset) % SERVER_ENDPOINT_COUNT;
    const ServerEndpoint &endpoint = SERVER_ENDPOINTS[index];
    const String url = buildUrl(endpoint);

    logClient("HTTP", "%s %s", logAction, url.c_str());

    HttpGetResponse response;
    response.endpointIndex = index;
    if (!performEndpointGet(endpoint, url, response.statusCode,
                            response.body)) {
      continue;
    }

    const HttpGetDecision decision = handleResponse(response);
    if (decision == HttpGetDecision::Success) {
      rememberSuccessfulEndpoint(index);
      return true;
    }
    if (decision == HttpGetDecision::Stop) {
      return false;
    }
  }

  return false;
}

/**
 * @brief Remember a successful endpoint and notify listeners.
 * @param endpointIndex Index into SERVER_ENDPOINTS.
 */
void LiveSessionService::rememberSuccessfulEndpoint(int endpointIndex) {
  if (endpointIndex < 0 || endpointIndex >= SERVER_ENDPOINT_COUNT) {
    return;
  }

  _nextServerIndex = endpointIndex;
  if (_callbacks.onEndpointIndexChanged) {
    _callbacks.onEndpointIndexChanged(endpointIndex);
  } else if (_prefsReady &&
             _prefs.getInt(kLastServerIndexKey, -1) != endpointIndex) {
    _prefs.putInt(kLastServerIndexKey, endpointIndex);
    logClient("WS", "saved preferred server index %d", endpointIndex);
  }
}

void LiveSessionService::logClient(const char *topic, const char *fmt,
                                   ...) const {
  va_list args;
  va_start(args, fmt);
  writeLog(_callbacks, 'C', topic, fmt, args);
  va_end(args);
}

void LiveSessionService::logServer(const char *topic, const char *fmt,
                                   ...) const {
  va_list args;
  va_start(args, fmt);
  writeLog(_callbacks, 'S', topic, fmt, args);
  va_end(args);
}
