#pragma once

#include <Arduino.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

typedef void *httpd_handle_t;
typedef struct httpd_req httpd_req_t;

class AudioService {
public:
  ~AudioService();

  bool begin();
  bool startServer();
  void update();
  void stopServer();

  bool available() const { return _available; }
  const char *error() const { return _error.c_str(); }
  bool microphoneEnabled() const { return _microphoneEnabled; }
  bool speakerEnabled() const { return _speakerEnabled; }
  bool recording() const { return _recording; }
  bool playing() const { return _playing; }

  void setMicrophoneEnabled(bool enabled);
  void setSpeakerEnabled(bool enabled);

private:
  httpd_handle_t _server = nullptr;
  SemaphoreHandle_t _audioMutex = nullptr;
  int16_t *_captureBuffer = nullptr;
  uint8_t *_playbackBuffer = nullptr;
  bool _available = false;
  volatile bool _microphoneEnabled = true;
  volatile bool _speakerEnabled = true;
  volatile bool _recording = false;
  volatile bool _playing = false;
  String _error;

  static esp_err_t captureHandler(httpd_req_t *request);
  static esp_err_t playbackHandler(httpd_req_t *request);
  static esp_err_t optionsHandler(httpd_req_t *request);

  esp_err_t handleCapture(httpd_req_t *request);
  esp_err_t handlePlayback(httpd_req_t *request);
};
