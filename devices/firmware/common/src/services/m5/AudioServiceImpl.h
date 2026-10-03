#include "AudioService.h"

#include "Config.h"
#include "hal/Board.h"
#include <M5Unified.h>
#include <math.h>

namespace {
constexpr int kChunkSamples = MIC_SAMPLE_RATE * MIC_CHUNK_MS / 1000;

// Small-speaker EQ for the 24 kHz internal cavity speaker.
// One-pole HPF ~280 Hz strips bass the driver can't reproduce.
// Peaking biquad at 2500 Hz, Q ≈ 0.8, +4 dB lifts the consonant band.
// Coefficients derived from RBJ cookbook at fs = 24 kHz.
// Bypassed when routing to the SPK2 HAT — it has the bottom end to play.
constexpr float kHpAlpha = 0.927f;
constexpr float kEqB0 = 1.1357f;
constexpr float kEqB1 = -1.2185f;
constexpr float kEqB2 = 0.4001f;
constexpr float kEqA1 = -1.2185f;
constexpr float kEqA2 = 0.5359f;

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
  Board::setAudioAmpEnabled(false);
  M5.Speaker.stop();
  M5.Speaker.end();
  delay(20);

  resetPlayback();
  const bool ready = M5.Mic.begin();
  _recording = ready;
  delay(20);
  return ready;
}

void AudioService::stopRecording() {
  M5.Mic.end();
  _recording = false;
  delay(20);
  beginSpeaker();
  setVolume(_volume);
}

bool AudioService::captureChunk() {
  if (!M5.Mic.record(_captureChunk, kChunkSamples, MIC_SAMPLE_RATE)) {
    return false;
  }

  // M5Unified queues recordings asynchronously. Do not transmit or inspect
  // the shared buffer until the DMA task has finished filling it.
  const unsigned long startedAt = millis();
  while (M5.Mic.isRecording()) {
    if (millis() - startedAt > MIC_CHUNK_MS + 1000) {
      M5.Mic.end();
      return false;
    }
    delay(1);
  }

  int64_t totalAbs = 0;
  int peak = 0;
  for (int i = 0; i < kChunkSamples; i++) {
    const int magnitude = abs(static_cast<int>(_captureChunk[i]));
    totalAbs += magnitude;
    peak = max(peak, magnitude);
  }

  _lastCaptureAverageAbs = static_cast<int>(totalAbs / kChunkSamples);
  _lastCapturePeak = peak;
  _lastCaptureChannel = 0;
  _lastCaptureLeftAverageAbs = _lastCaptureAverageAbs;
  _lastCaptureRightAverageAbs = 0;
  return true;
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

bool AudioService::playTone(int frequencyHz, int durationMs) {
  if (_recording || frequencyHz <= 0 || durationMs <= 0) {
    return false;
  }
  Board::setAudioAmpEnabled(true);
  M5.Speaker.tone(frequencyHz, static_cast<uint32_t>(durationMs));
  return true;
}

void AudioService::resetPlayback() { stopPlayback(); }

bool AudioService::queuePlayback(const uint8_t *data, size_t len) {
  if (len == 0) {
    return true;
  }

  if (!_chunkInFlight && !M5.Speaker.isPlaying()) {
    compactPlaybackBuffer();
  }

  if (!data || len % sizeof(int16_t) != 0 ||
      len > static_cast<size_t>(_playCapacity - _playWritePos)) {
    Serial.printf("[Audio] Playback overflow, dropping %u bytes\n",
                  static_cast<unsigned>(len));
    return false;
  }

  memcpy(_playBuffer + _playWritePos, data, len);
  if (!_useExternalSpeaker) {
    applyInternalSpeakerEq(
        reinterpret_cast<int16_t *>(_playBuffer + _playWritePos),
        static_cast<int>(len / sizeof(int16_t)));
  }
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

  const bool started = playAvailableChunk();
  if (playbackIdle()) Board::setAudioAmpEnabled(false);
  return started;
}

bool AudioService::speakerBusy() const {
  return _chunkInFlight || M5.Speaker.isPlaying();
}

bool AudioService::playbackIdle() const {
  return !_chunkInFlight && !M5.Speaker.isPlaying() &&
         bufferedPlaybackBytes() == 0;
}

void AudioService::stopPlayback() {
  Board::setAudioAmpEnabled(false);
  M5.Speaker.stop();
  // stop() queues a request; the audio task may still read our PCM buffer.
  // Keep its bytes and offsets intact until that task releases the channel.
  const unsigned long startedAt = millis();
  while (M5.Speaker.isPlaying()) {
    if (millis() - startedAt >= 100) {
      M5.Speaker.end(); // Joins the speaker task before releasing its buffers.
      if (!_recording) beginSpeaker();
      break;
    }
    delay(1);
  }
  _chunkInFlight = false;
  _playbackStarted = false;
  _playReadPos = 0;
  _playWritePos = 0;
  resetEqState();
}

void AudioService::beginSpeaker() {
  M5.Speaker.end();
  auto cfg = M5.Speaker.config();
  Board::configureSpeaker(cfg, _useExternalSpeaker, _externalSpeakerGain);
  M5.Speaker.config(cfg);
  M5.Speaker.begin();
  Board::setAudioAmpEnabled(false);
}

void AudioService::setUseExternalSpeaker(bool enabled) {
  enabled = enabled && Board::capabilities().externalSpeakerSwitch;
  if (_useExternalSpeaker == enabled) {
    return;
  }
  _useExternalSpeaker = enabled;
  Serial.printf("[Audio] External speaker %s\n", enabled ? "enabled" : "disabled");
  if (_playBuffer != nullptr && !_recording) {
    stopPlayback();
    beginSpeaker();
    setVolume(_volume);
  }
}

void AudioService::setExternalSpeakerGain(int gain) {
  const int clamped = constrain(gain, 1, 64);
  if (_externalSpeakerGain == clamped) {
    return;
  }
  _externalSpeakerGain = clamped;
  Serial.printf("[Audio] External speaker gain set to %d\n", _externalSpeakerGain);
  if (_useExternalSpeaker && _playBuffer != nullptr && !_recording) {
    stopPlayback();
    beginSpeaker();
    setVolume(_volume);
  }
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
  Board::setAudioAmpEnabled(true);
  if (!M5.Speaker.playRaw(start, samples, PLAY_SAMPLE_RATE, false, 1, 0)) {
    Board::setAudioAmpEnabled(false);
    return false;
  }
  _playReadPos = _playWritePos;
  _chunkInFlight = true;
  return true;
}

void AudioService::resetEqState() {
  _hpPrevX = 0.0f;
  _hpPrevY = 0.0f;
  _eqX1 = 0.0f;
  _eqX2 = 0.0f;
  _eqY1 = 0.0f;
  _eqY2 = 0.0f;
}

void AudioService::applyInternalSpeakerEq(int16_t *samples, int count) {
  for (int i = 0; i < count; ++i) {
    const float x = static_cast<float>(samples[i]);

    const float hp = kHpAlpha * (_hpPrevY + x - _hpPrevX);
    _hpPrevX = x;
    _hpPrevY = hp;

    float y = kEqB0 * hp + kEqB1 * _eqX1 + kEqB2 * _eqX2 -
              kEqA1 * _eqY1 - kEqA2 * _eqY2;
    _eqX2 = _eqX1;
    _eqX1 = hp;
    _eqY2 = _eqY1;
    _eqY1 = y;

    if (y > 32767.0f) y = 32767.0f;
    else if (y < -32768.0f) y = -32768.0f;
    samples[i] = static_cast<int16_t>(y);
  }
}

bool AudioService::playToneSequence(const String &sequence) {
  if (_recording || sequence.isEmpty()) {
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

    Board::setAudioAmpEnabled(true);
    M5.Speaker.tone(frequency, duration);
    delay(duration);
    played = true;
    start = end + 1;
  }

  stopPlayback();
  return played;
}
