#pragma once

#include <stdint.h>

/**
 * Detects a deliberate toss from orientation-independent acceleration.
 *
 * The accelerometer measures close to zero g while the device is in ballistic
 * flight. Peak height is estimated from total airtime, assuming the device is
 * caught at approximately the same height from which it was released.
 */
class TossDetector {
public:
  enum class Event { None, FlightStarted, TossComplete, TimedOut, Invalid };

  struct Update {
    Event event = Event::None;
    uint32_t airtimeMs = 0;
    float heightMeters = 0.0f;
  };

  void arm(uint32_t nowMs);
  void cancel();
  Update update(uint32_t nowMs, float accelerationG);

  bool isArmed() const { return _phase == Phase::Armed; }
  bool isInFlight() const { return _phase == Phase::InFlight; }

  static float heightFromAirtimeMs(uint32_t airtimeMs);

private:
  enum class Phase { Idle, Armed, InFlight };

  static constexpr float kLaunchImpulseG = 1.18f;
  static constexpr float kFreefallEnterG = 0.35f;
  static constexpr float kFreefallExitG = 0.62f;
  static constexpr float kImpactG = 1.8f;
  static constexpr uint32_t kLaunchToFreefallMaxMs = 650;
  static constexpr uint32_t kFreefallConfirmMs = 24;
  static constexpr uint32_t kLandingConfirmMs = 24;
  static constexpr uint32_t kMinimumAirtimeMs = 180;
  static constexpr uint32_t kMaximumAirtimeMs = 3000;
  static constexpr uint32_t kArmTimeoutMs = 10000;

  Phase _phase = Phase::Idle;
  uint32_t _armedAtMs = 0;
  uint32_t _impulseAtMs = 0;
  uint32_t _freefallCandidateAtMs = 0;
  uint32_t _flightStartedAtMs = 0;
  uint32_t _landingCandidateAtMs = 0;
  bool _sawImpulse = false;
  bool _freefallCandidate = false;
  bool _landingCandidate = false;
};
