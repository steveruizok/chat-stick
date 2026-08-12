#include "Qmi8658.h"

namespace {
constexpr uint8_t kAddressPrimary = 0x6B;
constexpr uint8_t kAddressFallback = 0x6A;
constexpr uint8_t kRegisterWhoAmI = 0x00;
constexpr uint8_t kWhoAmI = 0x05;
constexpr uint8_t kRegisterCtrl1 = 0x02;
constexpr uint8_t kRegisterCtrl2 = 0x03;
constexpr uint8_t kRegisterCtrl5 = 0x06;
constexpr uint8_t kRegisterCtrl7 = 0x08;
constexpr uint8_t kRegisterAccelXLow = 0x35;
constexpr uint8_t kRegisterReset = 0x60;
constexpr uint8_t kResetCommand = 0xB0;

// CTRL2: accelerometer +/-16 g (bits 5:4 = 3), 1000 Hz ODR (bits 3:0 = 3).
constexpr uint8_t kAccel16G1000Hz = 0x33;
constexpr float kAccelScaleGPerLsb = 16.0f / 32768.0f;
} // namespace

bool Qmi8658::begin(TwoWire &wire) {
  _wire = &wire;
  if (probe(kAddressPrimary)) {
    _address = kAddressPrimary;
  } else if (probe(kAddressFallback)) {
    _address = kAddressFallback;
  } else {
    return false;
  }

  if (!writeRegister(kRegisterReset, kResetCommand)) {
    return false;
  }
  delay(25);

  // Address auto-increment, little-endian output.
  if (!writeRegister(kRegisterCtrl1, 0x40) ||
      !writeRegister(kRegisterCtrl7, 0x00) ||
      !writeRegister(kRegisterCtrl2, kAccel16G1000Hz) ||
      !writeRegister(kRegisterCtrl5, 0x00) ||
      !writeRegister(kRegisterCtrl7, 0x01)) {
    return false;
  }
  delay(10);
  return true;
}

bool Qmi8658::readAcceleration(float &xG, float &yG, float &zG) {
  uint8_t bytes[6] = {};
  if (!readRegisters(kRegisterAccelXLow, bytes, sizeof(bytes))) {
    return false;
  }

  const int16_t x = static_cast<int16_t>(
      static_cast<uint16_t>(bytes[0]) |
      static_cast<uint16_t>(bytes[1]) << 8);
  const int16_t y = static_cast<int16_t>(
      static_cast<uint16_t>(bytes[2]) |
      static_cast<uint16_t>(bytes[3]) << 8);
  const int16_t z = static_cast<int16_t>(
      static_cast<uint16_t>(bytes[4]) |
      static_cast<uint16_t>(bytes[5]) << 8);

  xG = static_cast<float>(x) * kAccelScaleGPerLsb;
  yG = static_cast<float>(y) * kAccelScaleGPerLsb;
  zG = static_cast<float>(z) * kAccelScaleGPerLsb;
  return true;
}

bool Qmi8658::probe(uint8_t address) {
  _address = address;
  uint8_t id = 0;
  return readRegisters(kRegisterWhoAmI, &id, 1) && id == kWhoAmI;
}

bool Qmi8658::writeRegister(uint8_t reg, uint8_t value) {
  _wire->beginTransmission(_address);
  _wire->write(reg);
  _wire->write(value);
  return _wire->endTransmission() == 0;
}

bool Qmi8658::readRegisters(uint8_t reg, uint8_t *data, size_t length) {
  _wire->beginTransmission(_address);
  _wire->write(reg);
  if (_wire->endTransmission(false) != 0) {
    return false;
  }

  const size_t received =
      _wire->requestFrom(static_cast<int>(_address), static_cast<int>(length));
  if (received != length) {
    while (_wire->available()) {
      _wire->read();
    }
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    data[i] = static_cast<uint8_t>(_wire->read());
  }
  return true;
}
