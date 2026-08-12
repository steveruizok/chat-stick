#include "StackChanBoard.h"

#include "Config.h"
#include <math.h>

bool StackChanBoard::begin() {
  M5StackChan.begin();

  auto &lcd = display();
  lcd.setBrightness(Config::kDisplayBrightness);
  lcd.setRotation(1);
  lcd.setTextWrap(false);

  M5.Speaker.setVolume(Config::kSpeakerVolume);
  M5StackChan.Motion.setAutoTorqueReleaseEnabled(true);
  M5StackChan.Motion.setAutoAngleSyncEnabled(true);
  setRgb(0, 0, 0);
  return true;
}

void StackChanBoard::update() { M5StackChan.update(); }

LGFX_Device &StackChanBoard::display() { return M5StackChan.Display(); }

bool StackChanBoard::topWasClicked() const {
  return M5StackChan.TouchSensor.wasClicked();
}

bool StackChanBoard::topWasPressed() const {
  return M5StackChan.TouchSensor.wasPressed();
}

bool StackChanBoard::topWasReleased() const {
  return M5StackChan.TouchSensor.wasReleased();
}

bool StackChanBoard::topIsPressed() const {
  return M5StackChan.TouchSensor.isPressed();
}

bool StackChanBoard::topWasSwipedForward() const {
  return M5StackChan.TouchSensor.wasSwipedForward();
}

bool StackChanBoard::topWasSwipedBackward() const {
  return M5StackChan.TouchSensor.wasSwipedBackward();
}

const std::array<uint8_t, 3> &StackChanBoard::topTouchIntensities() const {
  return M5StackChan.TouchSensor.getIntensities();
}

bool StackChanBoard::screenWasTapped(int16_t &x, int16_t &y) {
  int16_t currentX = 0;
  int16_t currentY = 0;
  const bool down = display().getTouch(&currentX, &currentY);

  if (down && !_screenWasDown) {
    _screenWasDown = true;
    _screenDownX = currentX;
    _screenDownY = currentY;
  }

  if (!down && _screenWasDown) {
    _screenWasDown = false;
    x = _screenDownX;
    y = _screenDownY;
    return true;
  }

  return false;
}

void StackChanBoard::lookAtDegrees(float yawDegrees, float pitchDegrees,
                                   int speed) {
  const int yaw = constrain(static_cast<int>(lroundf(yawDegrees * 10.0f)),
                            Config::kYawMinTenths, Config::kYawMaxTenths);
  const int pitch =
      constrain(static_cast<int>(lroundf(pitchDegrees * 10.0f)),
                Config::kPitchMinTenths, Config::kPitchMaxTenths);
  M5StackChan.Motion.move(yaw, pitch, constrain(speed, 1, 1000));
}

void StackChanBoard::lookNeutral(int speed) {
  M5StackChan.Motion.move(Config::kNeutralYawTenths,
                         Config::kNeutralPitchTenths,
                         constrain(speed, 1, 1000));
}

void StackChanBoard::stopMotion() { M5StackChan.Motion.stop(); }

void StackChanBoard::setMotionTorqueEnabled(bool enabled) {
  M5StackChan.Motion.setTorqueEnabled(enabled);
}

int StackChanBoard::yawTenths() {
  return M5StackChan.Motion.getCurrentYawAngle();
}

int StackChanBoard::pitchTenths() {
  return M5StackChan.Motion.getCurrentPitchAngle();
}

bool StackChanBoard::motionIsMoving() {
  return M5StackChan.Motion.isMoving();
}

void StackChanBoard::setRgb(uint8_t red, uint8_t green, uint8_t blue) {
  M5StackChan.showRgbColor(red, green, blue);
}

float StackChanBoard::batteryVoltage() const {
  return M5StackChan.getBatteryVoltage();
}

float StackChanBoard::batteryCurrent() const {
  return M5StackChan.getBatteryCurrent();
}

int StackChanBoard::batteryPercent() const {
  const float voltage = batteryVoltage();
  if (voltage <= 0.0f) {
    return -1;
  }

  // A deliberately conservative one-cell LiPo estimate. Voltage is useful
  // for bring-up; a future power service can add a filtered discharge curve.
  return constrain(static_cast<int>(lroundf((voltage - 3.30f) / 0.90f * 100)),
                   0, 100);
}
