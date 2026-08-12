#pragma once

#include <Arduino.h>
#include <Wire.h>

/** Minimal accelerometer-only QMI8658 driver for high-rate toss detection. */
class Qmi8658 {
public:
  bool begin(TwoWire &wire);
  bool readAcceleration(float &xG, float &yG, float &zG);
  uint8_t address() const { return _address; }

private:
  bool probe(uint8_t address);
  bool writeRegister(uint8_t reg, uint8_t value);
  bool readRegisters(uint8_t reg, uint8_t *data, size_t length);

  TwoWire *_wire = nullptr;
  uint8_t _address = 0;
};
