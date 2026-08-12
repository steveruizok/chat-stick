#pragma once

#include <Arduino.h>
#include <M5StackChan.h>
#include <array>

class StackChanBoard {
public:
  bool begin();
  void update();

  LGFX_Device &display();

  bool topWasClicked() const;
  bool topWasPressed() const;
  bool topWasReleased() const;
  bool topIsPressed() const;
  bool topWasSwipedForward() const;
  bool topWasSwipedBackward() const;
  const std::array<uint8_t, 3> &topTouchIntensities() const;
  bool screenWasTapped(int16_t &x, int16_t &y);

  void lookAtDegrees(float yawDegrees, float pitchDegrees,
                     int speed = 350);
  void lookNeutral(int speed = 350);
  void stopMotion();
  void setMotionTorqueEnabled(bool enabled);
  int yawTenths();
  int pitchTenths();
  bool motionIsMoving();

  void setRgb(uint8_t red, uint8_t green, uint8_t blue);

  float batteryVoltage() const;
  float batteryCurrent() const;
  int batteryPercent() const;

private:
  bool _screenWasDown = false;
  int16_t _screenDownX = 0;
  int16_t _screenDownY = 0;
};
