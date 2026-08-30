#ifdef VOLUME_CAL_MODE

// Volume calibration harness (built via the waveshare-*-volcal envs).
//
// Repeats a fixed-amplitude 1 kHz tone while sweeping the speaker volume
// through the full 0-255 setVolume() API range, showing the current level on
// screen so the audible floor and comfort ceiling can be read off directly.
//
// The tone is synthesized at constant amplitude on purpose: playTone() also
// scales its samples by volume/255 in software, so tones are attenuated twice.
// Assistant speech is only scaled once, by the codec. Queuing raw PCM here and
// letting setVolume() do all the work matches the speech path.
//
// Controls: button A holds/resumes the sweep, button B (power) resets to 0.

#include "hal/Board.h"
#include "services/AudioService.h"
#include "state/StateTypes.h"
#include "ui/TextDisplay.h"

namespace {
/// Volume units (0-255 API scale) advanced per step.
constexpr int kStep = 5;

/// Time spent at each volume level before advancing.
constexpr unsigned long kDwellMs = 1200;

/// Calibration tone frequency.
constexpr int kToneHz = 1000;

/// Calibration tone duration per repeat.
constexpr int kToneMs = 400;

/// Constant tone amplitude; matches playTone() at full volume.
constexpr float kToneAmplitude = 16000.0f;

AudioService audio;
TextDisplay display;

int level = 0;
bool held = false;
unsigned long lastStepAt = 0;
bool buttonAWas = false;
bool buttonBWas = false;
unsigned long buttonEdgeAt = 0;

/// Mirror of AudioService's internal 0-255 -> ES8311 0-100 mapping.
int codecValueFromLevel(int value) {
  return map(constrain(value, 0, 255), 0, 255, 0, 100);
}

void playCalTone() {
  audio.resetPlayback();
  const int totalSamples = PLAY_SAMPLE_RATE * kToneMs / 1000;
  const int rampSamples = PLAY_SAMPLE_RATE * 5 / 1000; // 5 ms anti-click fade
  const float phaseStep = 2.0f * PI * kToneHz / PLAY_SAMPLE_RATE;
  float phase = 0.0f;
  int16_t chunk[256];
  int written = 0;
  while (written < totalSamples) {
    const int samples =
        min(totalSamples - written, static_cast<int>(sizeof(chunk) / 2));
    for (int i = 0; i < samples; i++) {
      const int sampleIndex = written + i;
      float gain = 1.0f;
      if (sampleIndex < rampSamples) {
        gain = sampleIndex / static_cast<float>(rampSamples);
      }
      const int fromEnd = totalSamples - 1 - sampleIndex;
      if (fromEnd < rampSamples) {
        gain = min(gain, fromEnd / static_cast<float>(rampSamples));
      }
      chunk[i] = static_cast<int16_t>(sinf(phase) * kToneAmplitude * gain);
      phase += phaseStep;
      if (phase > 2.0f * PI) {
        phase -= 2.0f * PI;
      }
    }
    if (!audio.queuePlayback(reinterpret_cast<const uint8_t *>(chunk),
                             samples * sizeof(int16_t))) {
      return;
    }
    written += samples;
  }
  audio.markPlaybackStarted();
}

void renderScreen() {
  DisplayState state;
  state.appState = AppState::Ready;
  state.headerLeft = "VOL CAL";
  state.headerRight = held ? "HELD" : "SWEEP";
  state.bodyText = "LEVEL " + String(level) + " / 255\nCODEC " +
                   String(codecValueFromLevel(level)) + " / 100";
  state.footerLeft = "A HOLD";
  state.footerRight = "B RESET";
  display.render(state);
}
} // namespace

void setup() {
  Serial.begin(115200);
  Board::init();
  display.init();
  display.setBrightness(200);
  audio.init();
  audio.setVolume(level);
  renderScreen();
  Serial.printf("[VolCal] level=%d codec=%d\n", level,
                codecValueFromLevel(level));
  lastStepAt = millis();
}

void loop() {
  Board::update();

  const unsigned long now = millis();
  const bool aPressed = Board::buttonAIsPressed();
  if (aPressed && !buttonAWas && now - buttonEdgeAt > 150) {
    buttonEdgeAt = now;
    held = !held;
    renderScreen();
  }
  buttonAWas = aPressed;

  const bool bPressed = Board::buttonBIsPressed();
  if (bPressed && !buttonBWas && now - buttonEdgeAt > 150) {
    buttonEdgeAt = now;
    level = 0;
    held = false;
    audio.setVolume(level);
    renderScreen();
    lastStepAt = now - kDwellMs;
  }
  buttonBWas = bPressed;

  if (now - lastStepAt >= kDwellMs) {
    if (!held) {
      level += kStep;
      if (level > 255) {
        level = 0;
      }
      audio.setVolume(level);
      renderScreen();
      Serial.printf("[VolCal] level=%d codec=%d\n", level,
                    codecValueFromLevel(level));
    }
    playCalTone();
    lastStepAt = now;
  }
  delay(10);
}

#else // VOLUME_CAL_MODE

#include "app/AppController.h"

namespace {
AppController app;
}

void setup() { app.setup(); }

void loop() { app.loop(); }

#endif // VOLUME_CAL_MODE
