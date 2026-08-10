#include "AudioService.h"

#include "Config.h"
#include <M5Unified.h>
#include <cstring>
#include <esp_http_server.h>

namespace {
constexpr size_t kWavHeaderBytes = 44;
constexpr size_t kCaptureSamples =
    Config::kAudioSampleRate * Config::kMaxDeviceRecordingSeconds;
constexpr size_t kCaptureBytes = kCaptureSamples * sizeof(int16_t);
constexpr size_t kPlaybackBytes =
    Config::kAudioSampleRate * Config::kMaxWebsiteRecordingSeconds *
        sizeof(int16_t) +
    kWavHeaderBytes;

struct __attribute__((packed)) WavHeader {
  char riff[4];
  uint32_t fileSizeMinus8;
  char wave[4];
  char fmt[4];
  uint32_t fmtSize;
  uint16_t format;
  uint16_t channels;
  uint32_t sampleRate;
  uint32_t bytesPerSecond;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char data[4];
  uint32_t dataBytes;
};

static_assert(sizeof(WavHeader) == kWavHeaderBytes,
              "The PCM WAV header must be 44 bytes");

WavHeader makeWavHeader(size_t samples) {
  WavHeader header = {};
  memcpy(header.riff, "RIFF", 4);
  header.fileSizeMinus8 =
      static_cast<uint32_t>(kWavHeaderBytes + samples * sizeof(int16_t) - 8);
  memcpy(header.wave, "WAVE", 4);
  memcpy(header.fmt, "fmt ", 4);
  header.fmtSize = 16;
  header.format = 1;
  header.channels = 1;
  header.sampleRate = Config::kAudioSampleRate;
  header.bytesPerSecond = Config::kAudioSampleRate * sizeof(int16_t);
  header.blockAlign = sizeof(int16_t);
  header.bitsPerSample = 16;
  memcpy(header.data, "data", 4);
  header.dataBytes = static_cast<uint32_t>(samples * sizeof(int16_t));
  return header;
}

void setCorsHeaders(httpd_req_t *request) {
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Methods",
                     "GET, POST, OPTIONS");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Headers", "Content-Type");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
}

esp_err_t sendError(httpd_req_t *request, const char *status,
                    const char *message) {
  setCorsHeaders(request);
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "application/json");
  String body = "{\"error\":\"" + String(message) + "\"}";
  return httpd_resp_send(request, body.c_str(), body.length());
}

bool validPlaybackWav(const uint8_t *data, size_t length) {
  if (!data || length < sizeof(WavHeader)) {
    return false;
  }
  const auto *header = reinterpret_cast<const WavHeader *>(data);
  return memcmp(header->riff, "RIFF", 4) == 0 &&
         memcmp(header->wave, "WAVE", 4) == 0 &&
         memcmp(header->fmt, "fmt ", 4) == 0 && header->fmtSize == 16 &&
         header->format == 1 && header->channels == 1 &&
         header->sampleRate == Config::kAudioSampleRate &&
         header->bytesPerSecond == Config::kAudioSampleRate * sizeof(int16_t) &&
         header->blockAlign == sizeof(int16_t) && header->bitsPerSample == 16 &&
         memcmp(header->data, "data", 4) == 0 &&
         header->fileSizeMinus8 == length - 8 &&
         header->dataBytes == length - sizeof(WavHeader);
}
} // namespace

AudioService::~AudioService() {
  stopServer();
  M5.Mic.end();
  M5.Speaker.stop();
  if (_captureBuffer) {
    free(_captureBuffer);
  }
  if (_playbackBuffer) {
    free(_playbackBuffer);
  }
  if (_audioMutex) {
    vSemaphoreDelete(_audioMutex);
  }
}

bool AudioService::begin() {
  _audioMutex = xSemaphoreCreateMutex();
  _captureBuffer = static_cast<int16_t *>(ps_malloc(kCaptureBytes));
  _playbackBuffer = static_cast<uint8_t *>(ps_malloc(kPlaybackBytes));
  if (!_audioMutex || !_captureBuffer || !_playbackBuffer) {
    _error = "PSRAM audio buffer allocation failed";
    Serial.printf("[Audio] %s\n", _error.c_str());
    return false;
  }

  if (!M5.Mic.isEnabled() || !M5.Speaker.isEnabled()) {
    _error = "CoreS3 audio hardware unavailable";
    Serial.printf("[Audio] %s\n", _error.c_str());
    return false;
  }

  M5.Speaker.setVolume(Config::kSpeakerVolume);
  _available = true;
  Serial.printf("[Audio] ready capture=%uK playback=%uK rate=%uHz\n",
                static_cast<unsigned>(kCaptureBytes / 1024),
                static_cast<unsigned>(kPlaybackBytes / 1024),
                Config::kAudioSampleRate);
  return true;
}

bool AudioService::startServer() {
  if (!_available || _server) {
    return _available;
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = Config::kAudioHttpPort;
  config.ctrl_port = Config::kAudioHttpPort + 1000;
  config.stack_size = 8192;
  config.max_open_sockets = 3;
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 10;
  if (httpd_start(&_server, &config) != ESP_OK) {
    _server = nullptr;
    _error = "audio HTTP server failed";
    Serial.printf("[Audio] %s\n", _error.c_str());
    return false;
  }

  httpd_uri_t capture = {};
  capture.uri = "/capture";
  capture.method = HTTP_GET;
  capture.handler = captureHandler;
  capture.user_ctx = this;
  httpd_uri_t playback = {};
  playback.uri = "/play";
  playback.method = HTTP_POST;
  playback.handler = playbackHandler;
  playback.user_ctx = this;
  httpd_uri_t captureOptions = {};
  captureOptions.uri = "/capture";
  captureOptions.method = HTTP_OPTIONS;
  captureOptions.handler = optionsHandler;
  captureOptions.user_ctx = this;
  httpd_uri_t playbackOptions = {};
  playbackOptions.uri = "/play";
  playbackOptions.method = HTTP_OPTIONS;
  playbackOptions.handler = optionsHandler;
  playbackOptions.user_ctx = this;

  if (httpd_register_uri_handler(_server, &capture) != ESP_OK ||
      httpd_register_uri_handler(_server, &playback) != ESP_OK ||
      httpd_register_uri_handler(_server, &captureOptions) != ESP_OK ||
      httpd_register_uri_handler(_server, &playbackOptions) != ESP_OK) {
    httpd_stop(_server);
    _server = nullptr;
    _error = "audio HTTP route registration failed";
    Serial.printf("[Audio] %s\n", _error.c_str());
    return false;
  }

  Serial.printf("[Audio] transfer server ready on port %u\n",
                Config::kAudioHttpPort);
  return true;
}

void AudioService::update() {
  if (_playing && !M5.Speaker.isPlaying()) {
    _playing = false;
  }
}

void AudioService::stopServer() {
  if (_server) {
    httpd_stop(_server);
    _server = nullptr;
  }
  M5.Mic.end();
  M5.Speaker.stop();
  _recording = false;
  _playing = false;
}

void AudioService::setMicrophoneEnabled(bool enabled) {
  _microphoneEnabled = enabled;
  Serial.printf("[Audio] device microphone %s\n", enabled ? "on" : "muted");
}

void AudioService::setSpeakerEnabled(bool enabled) {
  _speakerEnabled = enabled;
  if (!enabled && _audioMutex &&
      xSemaphoreTake(_audioMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    M5.Speaker.stop();
    M5.Speaker.end();
    _playing = false;
    xSemaphoreGive(_audioMutex);
  }
  Serial.printf("[Audio] device speaker %s\n", enabled ? "on" : "muted");
}

esp_err_t AudioService::captureHandler(httpd_req_t *request) {
  return static_cast<AudioService *>(request->user_ctx)->handleCapture(request);
}

esp_err_t AudioService::playbackHandler(httpd_req_t *request) {
  return static_cast<AudioService *>(request->user_ctx)->handlePlayback(request);
}

esp_err_t AudioService::optionsHandler(httpd_req_t *request) {
  setCorsHeaders(request);
  return httpd_resp_send(request, nullptr, 0);
}

esp_err_t AudioService::handleCapture(httpd_req_t *request) {
  if (!_available) {
    return sendError(request, "503 Service Unavailable", "audio unavailable");
  }
  if (!_microphoneEnabled) {
    return sendError(request, "403 Forbidden", "device microphone is muted");
  }
  if (xSemaphoreTake(_audioMutex, 0) != pdTRUE) {
    return sendError(request, "409 Conflict", "audio device is busy");
  }

  int seconds = Config::kDefaultDeviceRecordingSeconds;
  char query[48];
  char value[12];
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "seconds", value, sizeof(value)) == ESP_OK) {
    seconds = atoi(value);
  }
  seconds = constrain(seconds, 1, Config::kMaxDeviceRecordingSeconds);
  const size_t samples = Config::kAudioSampleRate * seconds;

  _playing = false;
  M5.Speaker.stop();
  M5.Speaker.end();
  if (!M5.Mic.begin() ||
      !M5.Mic.record(_captureBuffer, samples, Config::kAudioSampleRate, false)) {
    M5.Mic.end();
    if (_speakerEnabled) {
      M5.Speaker.begin();
      M5.Speaker.setVolume(Config::kSpeakerVolume);
    }
    xSemaphoreGive(_audioMutex);
    return sendError(request, "500 Internal Server Error",
                     "microphone capture failed to start");
  }

  _recording = true;
  const uint32_t started = millis();
  bool cancelled = false;
  while (M5.Mic.isRecording()) {
    if (!_microphoneEnabled ||
        millis() - started > static_cast<uint32_t>((seconds + 2) * 1000)) {
      cancelled = true;
      M5.Mic.end();
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  M5.Mic.end();
  _recording = false;
  if (_speakerEnabled) {
    M5.Speaker.begin();
    M5.Speaker.setVolume(Config::kSpeakerVolume);
  }

  if (cancelled) {
    xSemaphoreGive(_audioMutex);
    return sendError(request, "409 Conflict", "microphone capture cancelled");
  }

  const WavHeader header = makeWavHeader(samples);
  setCorsHeaders(request);
  httpd_resp_set_type(request, "audio/wav");
  httpd_resp_set_hdr(request, "Content-Disposition",
                     "inline; filename=stack-chan.wav");
  esp_err_t result = httpd_resp_send_chunk(
      request, reinterpret_cast<const char *>(&header), sizeof(header));
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(
        request, reinterpret_cast<const char *>(_captureBuffer),
        samples * sizeof(int16_t));
  }
  if (result == ESP_OK) {
    result = httpd_resp_send_chunk(request, nullptr, 0);
  }
  xSemaphoreGive(_audioMutex);
  return result;
}

esp_err_t AudioService::handlePlayback(httpd_req_t *request) {
  if (!_available) {
    return sendError(request, "503 Service Unavailable", "audio unavailable");
  }
  if (!_speakerEnabled) {
    return sendError(request, "403 Forbidden", "device speaker is muted");
  }
  if (request->content_len < static_cast<int>(kWavHeaderBytes) ||
      request->content_len > static_cast<int>(kPlaybackBytes)) {
    return sendError(request, "413 Payload Too Large",
                     "recording must be a WAV no longer than 10 seconds");
  }
  if (xSemaphoreTake(_audioMutex, 0) != pdTRUE) {
    return sendError(request, "409 Conflict", "audio device is busy");
  }

  M5.Speaker.stop();
  _playing = false;
  size_t received = 0;
  int timeoutCount = 0;
  while (received < static_cast<size_t>(request->content_len)) {
    const int result = httpd_req_recv(
        request, reinterpret_cast<char *>(_playbackBuffer + received),
        request->content_len - received);
    if (result == HTTPD_SOCK_ERR_TIMEOUT && timeoutCount++ < 3) {
      continue;
    }
    if (result <= 0) {
      xSemaphoreGive(_audioMutex);
      return sendError(request, "400 Bad Request", "audio upload interrupted");
    }
    received += result;
  }

  if (!validPlaybackWav(_playbackBuffer, received)) {
    xSemaphoreGive(_audioMutex);
    return sendError(request, "415 Unsupported Media Type",
                     "expected 16 kHz mono 16-bit PCM WAV");
  }
  if (!_speakerEnabled) {
    xSemaphoreGive(_audioMutex);
    return sendError(request, "403 Forbidden", "device speaker is muted");
  }

  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(Config::kSpeakerVolume);
  const bool started =
      M5.Speaker.playWav(_playbackBuffer, received, 1, 0, true);
  _playing = started;
  xSemaphoreGive(_audioMutex);
  if (!started) {
    return sendError(request, "500 Internal Server Error",
                     "speaker playback failed to start");
  }

  setCorsHeaders(request);
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, "{\"playing\":true}");
}
