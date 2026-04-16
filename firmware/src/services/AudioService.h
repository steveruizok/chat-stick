#pragma once

#include "../Config.h"
#include <Arduino.h>

class AudioService {
public:
  ~AudioService();

  bool init();

  void setVolume(int level);
  int volume() const { return _volume; }

  bool startRecording();
  void stopRecording();
  bool captureChunk();

  const int16_t *captureData() const { return _captureChunk; }
  size_t captureBytes() const { return _chunkBytes; }

  void resetPlayback();
  bool queuePlayback(const uint8_t *data, size_t len);
  int bufferedPlaybackBytes() const;
  bool playbackStarted() const { return _playbackStarted; }
  void markPlaybackStarted() { _playbackStarted = true; }
  bool advancePlayback();
  bool speakerBusy() const;
  bool playbackIdle() const;
  void stopPlayback();

private:
  static constexpr int kMaxPlayBytes =
      PLAY_SAMPLE_RATE * 2 * MAX_PLAYBACK_SEC;

  int16_t *_captureChunk = nullptr;
  size_t _chunkBytes = MIC_SAMPLE_RATE * MIC_CHUNK_MS / 1000 * sizeof(int16_t);

  uint8_t *_playBuffer = nullptr;
  int _playWritePos = 0;
  int _playReadPos = 0;
  bool _playbackStarted = false;
  bool _chunkInFlight = false;
  int _volume = DEFAULT_VOLUME;

  void beginSpeaker();
  void compactPlaybackBuffer();
  bool playAvailableChunk();
};
