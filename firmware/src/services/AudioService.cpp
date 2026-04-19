#include "AudioService.h"

#include "../Config.h"
#include <M5Unified.h>
#include <math.h>

namespace {
constexpr int kChunkSamples = MIC_SAMPLE_RATE * MIC_CHUNK_MS / 1000;

int noteIndex(char note) {
  switch (toupper(note)) {
  case 'C':
    return 0;
  case 'D':
    return 2;
  case 'E':
    return 4;
  case 'F':
    return 5;
  case 'G':
    return 7;
  case 'A':
    return 9;
  case 'B':
    return 11;
  default:
    return -1;
  }
}

float noteFrequency(const String &noteToken) {
  if (noteToken.isEmpty()) {
    return 0.0f;
  }

  const char noteName = noteToken[0];
  if (toupper(noteName) == 'R') {
    return 0.0f;
  }

  int semitone = noteIndex(noteName);
  if (semitone < 0) {
    return -1.0f;
  }

  int index = 1;
  if (index < static_cast<int>(noteToken.length())) {
    if (noteToken[index] == '#') {
      semitone += 1;
      index++;
    } else if (noteToken[index] == 'b' || noteToken[index] == 'B') {
      semitone -= 1;
      index++;
    }
  }

  const int octave =
      index < static_cast<int>(noteToken.length()) ? noteToken.substring(index).toInt() : 4;
  const int midi = (octave + 1) * 12 + semitone;
  return 440.0f * powf(2.0f, (midi - 69) / 12.0f);
}
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
  _playCapacity = kMaxPlayBytes;

  if (!_captureChunk) {
    _captureChunk = static_cast<int16_t *>(malloc(_chunkBytes));
    if (_captureChunk) {
      Serial.println("[Audio] Using heap capture buffer fallback");
    }
  }

  if (!_playBuffer) {
    _playBuffer = static_cast<uint8_t *>(malloc(kFallbackPlayBytes));
    if (_playBuffer) {
      _playCapacity = kFallbackPlayBytes;
      Serial.printf("[Audio] Using heap playback buffer fallback (%d bytes)\n",
                    _playCapacity);
    }
  }

  if (!_captureChunk || !_playBuffer) {
    Serial.println("[Audio] Audio buffer allocation failed");
    return false;
  }

  beginSpeaker();
  setVolume(_volume);
  Serial.printf("[Audio] Capture chunk: %d samples (%u bytes)\n", kChunkSamples,
                static_cast<unsigned>(_chunkBytes));
  Serial.printf("[Audio] Playback buffer: %d bytes\n", _playCapacity);
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

bool AudioService::playNamedSound(const String &name) {
  const String normalized = name;
  if (normalized.equalsIgnoreCase("beep")) {
    return playToneSequence("C6:120");
  }
  if (normalized.equalsIgnoreCase("success")) {
    return playToneSequence("C6:90 E6:90 G6:140");
  }
  if (normalized.equalsIgnoreCase("error")) {
    return playToneSequence("G4:120 R:40 C4:240");
  }
  if (normalized.equalsIgnoreCase("alert")) {
    return playToneSequence("A5:120 R:40 A5:120 R:40 A5:200");
  }
  if (normalized.equalsIgnoreCase("fanfare")) {
    return playToneSequence("C5:100 E5:100 G5:100 C6:260");
  }

  return false;
}

bool AudioService::playMelody(const String &melody) {
  return playToneSequence(melody);
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

  if (_playWritePos + static_cast<int>(len) > _playCapacity) {
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
  _playReadPos = 0;
  _playWritePos = 0;
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

bool AudioService::playToneSequence(const String &sequence) {
  if (sequence.isEmpty()) {
    return false;
  }

  stopPlayback();
  beginSpeaker();
  setVolume(_volume);

  int start = 0;
  bool played = false;
  while (start < static_cast<int>(sequence.length())) {
    while (start < static_cast<int>(sequence.length()) &&
           (sequence[start] == ' ' || sequence[start] == ',')) {
      start++;
    }
    if (start >= static_cast<int>(sequence.length())) {
      break;
    }

    int end = start;
    while (end < static_cast<int>(sequence.length()) && sequence[end] != ' ' &&
           sequence[end] != ',') {
      end++;
    }

    const String token = sequence.substring(start, end);
    const int divider = token.indexOf(':');
    const String noteToken = divider >= 0 ? token.substring(0, divider) : token;
    const int durationMs =
        divider >= 0 ? max(10, static_cast<int>(token.substring(divider + 1).toInt())) : 200;
    const uint32_t duration = static_cast<uint32_t>(durationMs);
    const float frequency = noteFrequency(noteToken);

    if (frequency < 0.0f) {
      start = end + 1;
      continue;
    }

    if (frequency == 0.0f) {
      delay(duration);
      played = true;
      start = end + 1;
      continue;
    }

    M5.Speaker.tone(frequency, duration);
    delay(duration);
    played = true;
    start = end + 1;
  }

  return played;
}
