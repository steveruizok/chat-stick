#pragma once

#include "../Config.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

/**
 * @brief Owns microphone capture, speaker playback, and audio buffering.
 */
class AudioService {
public:
  /// Release allocated audio buffers and tasks.
  ~AudioService();

  /// Initialize the audio hardware and internal buffers.
  bool init();

  /**
   * @brief Set speaker output volume.
   * @param level Output volume level.
   */
  void setVolume(int level);

  /// Current speaker volume level.
  int volume() const { return _volume; }

  /// No-op on boards without selectable external speaker routing.
  void setUseExternalSpeaker(bool) {}

  /// Whether external speaker routing is enabled.
  bool useExternalSpeaker() const { return false; }

  /// No-op on boards without adjustable external speaker gain.
  void setExternalSpeakerGain(int) {}

  /// Current external speaker gain, or zero when unsupported.
  int externalSpeakerGain() const { return 0; }

  /// Begin microphone capture for a user turn.
  bool startRecording();

  /// Stop microphone capture.
  void stopRecording();

  /// Capture the next microphone chunk into the internal buffer.
  bool captureChunk();

  /**
   * @brief Play a named UI sound effect.
   * @param name Sound identifier.
   * @return True on successful playback scheduling.
   */
  bool playNamedSound(const String &name);

  /**
   * @brief Play a tone sequence melody.
   * @param melody Melody description string.
   * @return True on successful playback scheduling.
   */
  bool playMelody(const String &melody);

  /**
   * @brief Play a melody at the reserved alert volume (VOLUME_RAW_ALERT),
   *        louder than the calibrated speech ceiling, then restore the
   *        current speech volume.
   * @param melody Melody description string.
   * @return True on successful playback scheduling.
   */
  bool playAlarmMelody(const String &melody);

  /**
   * @brief Play a single synthesized tone.
   * @param frequencyHz Frequency in Hz.
   * @param durationMs Duration in milliseconds.
   * @return True on successful playback scheduling.
   */
  bool playTone(int frequencyHz, int durationMs);

  /// Pointer to the most recently captured mono PCM chunk.
  const int16_t *captureData() const { return _captureChunk; }

  /// Byte length of the mono capture buffer.
  size_t captureBytes() const { return _chunkBytes; }

  /// Mean absolute amplitude of the last capture.
  int lastCaptureAverageAbs() const { return _lastCaptureAverageAbs; }

  /// Peak amplitude of the last capture.
  int lastCapturePeak() const { return _lastCapturePeak; }

  /// Dominant channel reported by the last stereo capture analysis.
  int lastCaptureChannel() const { return _lastCaptureChannel; }

  /// Mean absolute amplitude of the left channel.
  int lastCaptureLeftAverageAbs() const { return _lastCaptureLeftAverageAbs; }

  /// Mean absolute amplitude of the right channel.
  int lastCaptureRightAverageAbs() const { return _lastCaptureRightAverageAbs; }

  /// Clear queued playback state without stopping the audio subsystem.
  void resetPlayback();

  /**
   * @brief Queue decoded PCM audio for playback.
   * @param data PCM bytes to enqueue.
   * @param len Number of bytes in the buffer.
   * @return True when the bytes were accepted into the ring buffer.
   */
  bool queuePlayback(const uint8_t *data, size_t len);

  /// Number of bytes currently buffered for playback.
  int bufferedPlaybackBytes() const;

  /// Whether playback of the current turn has started.
  bool playbackStarted() const;

  /// Mark the current playback turn as started.
  void markPlaybackStarted();

  /// Advance playback by handing the next chunk to the speaker.
  bool advancePlayback();

  /// Whether the speaker path is currently busy.
  bool speakerBusy() const;

  /// Whether no playback data or in-flight audio remains.
  bool playbackIdle() const;

  /// Stop playback and clear queued audio.
  void stopPlayback();

private:
  /// Maximum playback buffer size derived from configured max duration.
  static constexpr int kMaxPlayBytes = PLAY_SAMPLE_RATE * 2 * MAX_PLAYBACK_SEC;

  /// Smaller fallback playback buffer size when allocation is constrained.
  static constexpr int kFallbackPlayBytes = PLAY_SAMPLE_RATE * 2 * 4;

  /// Mono capture buffer sent to the server.
  int16_t *_captureChunk = nullptr;

  /// Stereo capture buffer used for channel analysis.
  int16_t *_captureStereoChunk = nullptr;

  /// Byte size of the mono capture buffer.
  size_t _chunkBytes = MIC_SAMPLE_RATE * MIC_CHUNK_MS / 1000 * sizeof(int16_t);

  /// Byte size of the stereo capture buffer.
  size_t _stereoChunkBytes =
      MIC_SAMPLE_RATE * MIC_CHUNK_MS / 1000 * 2 * sizeof(int16_t);

  /// Ring buffer for queued playback bytes.
  uint8_t *_playBuffer = nullptr;

  /// Scratch stereo buffer for speaker writes.
  int16_t *_stereoPlaybackChunk = nullptr;

  /// Mutex protecting shared audio hardware access.
  SemaphoreHandle_t _audioMutex = nullptr;

  /// Mutex protecting playback ring-buffer state.
  SemaphoreHandle_t _playbackMutex = nullptr;

  /// Wake signal for the playback task.
  SemaphoreHandle_t _playbackWake = nullptr;

  /// Background task that feeds audio to the speaker.
  TaskHandle_t _playbackTask = nullptr;

  /// Total allocated playback buffer capacity.
  int _playCapacity = kMaxPlayBytes;

  /// Ring-buffer write cursor.
  int _playWritePos = 0;

  /// Ring-buffer read cursor.
  int _playReadPos = 0;

  /// Number of queued bytes currently buffered.
  int _playBufferedBytes = 0;

  /// Whether playback has started for the current turn.
  bool _playbackStarted = false;

  /// Whether a speaker chunk is currently in flight.
  bool _playChunkInFlight = false;

  /// Whether the background playback task should keep running.
  volatile bool _playbackTaskRunning = false;

  /// Current speaker volume.
  int _volume = DEFAULT_VOLUME;

  /// Mean absolute amplitude from the last capture.
  int _lastCaptureAverageAbs = 0;

  /// Peak amplitude from the last capture.
  int _lastCapturePeak = 0;

  /// Dominant channel inferred from the last capture.
  int _lastCaptureChannel = 0;

  /// Mean absolute amplitude of the left capture channel.
  int _lastCaptureLeftAverageAbs = 0;

  /// Mean absolute amplitude of the right capture channel.
  int _lastCaptureRightAverageAbs = 0;

  /**
   * @brief Configure the audio codec for a target sample rate.
   * @param sampleRate Desired sample rate.
   * @return True on success.
   */
  bool configureAudio(int sampleRate);

  /**
   * @brief Acquire the audio-hardware mutex.
   * @param timeout FreeRTOS wait timeout.
   * @return True when the lock was acquired.
   */
  bool takeAudioLock(TickType_t timeout = portMAX_DELAY);

  /// Release the audio-hardware mutex.
  void releaseAudioLock();

  /**
   * @brief Acquire the playback-state mutex.
   * @param timeout FreeRTOS wait timeout.
   * @return True when the lock was acquired.
   */
  bool takePlaybackLock(TickType_t timeout = portMAX_DELAY) const;

  /// Release the playback-state mutex.
  void releasePlaybackLock() const;

  /// Start the background playback task.
  bool startPlaybackTask();

  /// Stop the background playback task.
  void stopPlaybackTask();

  /// Wake the playback task after new audio arrives.
  void wakePlaybackTask();

  /// Main loop for the playback background task.
  void playbackTaskLoop();

  /**
   * @brief FreeRTOS trampoline that forwards into playbackTaskLoop().
   * @param ctx AudioService instance pointer.
   */
  static void playbackTaskTrampoline(void *ctx);

  /// Send the next queued playback chunk to the speaker.
  bool playAvailableChunk();

  /**
   * @brief Parse and play a tone-sequence string.
   * @param sequence Tone sequence description.
   * @param amplitude Peak sample amplitude for the synthesized notes.
   * @return True on successful scheduling.
   */
  bool playToneSequence(const String &sequence, float amplitude);

  /// Tone amplitude scaled by the current volume level.
  float volumeScaledAmplitude() const { return 16000.0f * (_volume / 255.0f); }
};
