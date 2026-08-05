#pragma once

#include <Arduino.h>

class CameraService {
public:
  bool begin();
  bool startStream();
  bool available() const { return _available; }
  const char *error() const { return _error.c_str(); }
  bool captureJpeg(uint8_t **jpeg, size_t *jpegLength);

private:
  bool _initialized = false;
  bool _available = false;
  String _error;
  int _videoFd = -1;
  void *_frameBuffer = nullptr;
  size_t _frameBufferLength = 0;
  uint32_t _width = 0;
  uint32_t _height = 0;
  uint32_t _pixelFormat = 0;
};
