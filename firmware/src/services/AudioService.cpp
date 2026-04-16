#include "AudioService.h"

#include "../Config.h"
#include <M5Unified.h>

namespace {
constexpr int kChunkSamples = MIC_SAMPLE_RATE * MIC_CHUNK_MS / 1000;
}

AudioService::~AudioService() {
  if (_captureChunk) {
    free(_captureChunk);
    _captureChunk = nullptr;
  }
  if (_playBuffer) {
    free(_playBuffer);
    _playBuffer = nullptr;
  }
}

bool AudioService::init() {
  _captureChunk = static_cast<int16_t *>(ps_malloc(_chunkBytes));
  _playBuffer = static_cast<uint8_t *>(ps_malloc(kMaxPlayBytes));

  if (!_captureChunk || !_playBuffer) {
    Serial.println("[Audio] PSRAM allocation failed");
    return false;
  }

  beginSpeaker();
  setVolume(_volume);
  Serial.printf("[Audio] Capture chunk: %d samples (%u bytes)\n", kChunkSamples,
                static_cast<unsigned>(_chunkBytes));
  Serial.printf("[Audio] Playback buffer: %d bytes\n", kMaxPlayBytes);
  return true;
}

void AudioService::setVolume(int level) {
  _volume = constrain(level, 0, 255);
  M5.Speaker.setVolume(_volume);
  M5.Speaker.setAllChannelVolume(_volume);
}

bool AudioService::startRecording() {
  M5.Speaker.stop();
  M5.Speaker.end();
  delay(20);

  resetPlayback();
  M5.Mic.begin();
  delay(20);
  return true;
}

void AudioService::stopRecording() {
  M5.Mic.end();
  delay(20);
  beginSpeaker();
  setVolume(_volume);
}

bool AudioService::captureChunk() {
  return M5.Mic.record(_captureChunk, kChunkSamples, MIC_SAMPLE_RATE);
}

void AudioService::resetPlayback() {
  _playWritePos = 0;
  _playReadPos = 0;
  _playbackStarted = false;
  _chunkInFlight = false;
  M5.Speaker.stop();
}

bool AudioService::queuePlayback(const uint8_t *data, size_t len) {
  if (len == 0) {
    return true;
  }

  if (!_chunkInFlight && !M5.Speaker.isPlaying()) {
    compactPlaybackBuffer();
  }

  if (_playWritePos + static_cast<int>(len) > kMaxPlayBytes) {
    Serial.printf("[Audio] Playback overflow, dropping %u bytes\n",
                  static_cast<unsigned>(len));
    return false;
  }

  memcpy(_playBuffer + _playWritePos, data, len);
  _playWritePos += static_cast<int>(len);
  return true;
}

int AudioService::bufferedPlaybackBytes() const {
  return _playWritePos - _playReadPos;
}

bool AudioService::advancePlayback() {
  if (_chunkInFlight && M5.Speaker.isPlaying()) {
    return false;
  }

  if (_chunkInFlight && !M5.Speaker.isPlaying()) {
    _chunkInFlight = false;
    compactPlaybackBuffer();
  }

  return playAvailableChunk();
}

bool AudioService::speakerBusy() const {
  return _chunkInFlight || M5.Speaker.isPlaying();
}

bool AudioService::playbackIdle() const {
  return !_chunkInFlight && !M5.Speaker.isPlaying() &&
         bufferedPlaybackBytes() == 0;
}

void AudioService::stopPlayback() {
  M5.Speaker.stop();
  _chunkInFlight = false;
  _playbackStarted = false;
}

void AudioService::beginSpeaker() {
  M5.Speaker.begin();
}

void AudioService::compactPlaybackBuffer() {
  if (_playReadPos == 0) {
    return;
  }

  const int unread = bufferedPlaybackBytes();
  if (unread > 0) {
    memmove(_playBuffer, _playBuffer + _playReadPos, unread);
  }

  _playReadPos = 0;
  _playWritePos = unread;
}

bool AudioService::playAvailableChunk() {
  const int available = bufferedPlaybackBytes();
  if (available <= 0) {
    return false;
  }

  auto *start = reinterpret_cast<int16_t *>(_playBuffer + _playReadPos);
  const int samples = available / static_cast<int>(sizeof(int16_t));
  M5.Speaker.playRaw(start, samples, PLAY_SAMPLE_RATE, false, 1, 0);
  _playReadPos = _playWritePos;
  _chunkInFlight = true;
  return true;
}
