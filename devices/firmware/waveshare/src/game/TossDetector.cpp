#include "TossDetector.h"

namespace {
constexpr float kGravityMetersPerSecondSquared = 9.80665f;
}

void TossDetector::arm(uint32_t nowMs) {
  _phase = Phase::Armed;
  _armedAtMs = nowMs;
  _impulseAtMs = 0;
  _freefallCandidateAtMs = 0;
  _flightStartedAtMs = 0;
  _landingCandidateAtMs = 0;
  _sawImpulse = false;
  _freefallCandidate = false;
  _landingCandidate = false;
}

void TossDetector::cancel() {
  _phase = Phase::Idle;
  _sawImpulse = false;
  _freefallCandidate = false;
  _landingCandidate = false;
}

TossDetector::Update TossDetector::update(uint32_t nowMs,
                                          float accelerationG) {
  if (_phase == Phase::Idle) {
    return {};
  }

  if (_phase == Phase::Armed) {
    if (nowMs - _armedAtMs >= kArmTimeoutMs) {
      cancel();
      return {.event = Event::TimedOut};
    }

    if (accelerationG >= kLaunchImpulseG) {
      _sawImpulse = true;
      _impulseAtMs = nowMs;
    }

    const bool recentImpulse =
        _sawImpulse && nowMs - _impulseAtMs <= kLaunchToFreefallMaxMs;
    if (recentImpulse && accelerationG <= kFreefallEnterG) {
      if (!_freefallCandidate) {
        _freefallCandidate = true;
        _freefallCandidateAtMs = nowMs;
      }
      if (nowMs - _freefallCandidateAtMs >= kFreefallConfirmMs) {
        // Preserve the first low-g sample as the release time instead of the
        // later confirmation time.
        _flightStartedAtMs = _freefallCandidateAtMs;
        _phase = Phase::InFlight;
        _landingCandidate = false;
        return {.event = Event::FlightStarted};
      }
    } else {
      _freefallCandidate = false;
    }
    return {};
  }

  const uint32_t elapsedMs = nowMs - _flightStartedAtMs;
  if (elapsedMs > kMaximumAirtimeMs) {
    cancel();
    return {.event = Event::Invalid};
  }

  if (accelerationG >= kFreefallExitG) {
    if (!_landingCandidate) {
      _landingCandidate = true;
      _landingCandidateAtMs = nowMs;
    }

    const bool confirmed =
        accelerationG >= kImpactG ||
        nowMs - _landingCandidateAtMs >= kLandingConfirmMs;
    if (confirmed) {
      const uint32_t airtimeMs = _landingCandidateAtMs - _flightStartedAtMs;
      cancel();
      if (airtimeMs < kMinimumAirtimeMs) {
        return {.event = Event::Invalid, .airtimeMs = airtimeMs};
      }
      return {.event = Event::TossComplete,
              .airtimeMs = airtimeMs,
              .heightMeters = heightFromAirtimeMs(airtimeMs)};
    }
  } else {
    _landingCandidate = false;
  }

  return {};
}

float TossDetector::heightFromAirtimeMs(uint32_t airtimeMs) {
  const float seconds = static_cast<float>(airtimeMs) / 1000.0f;
  return kGravityMetersPerSecondSquared * seconds * seconds / 8.0f;
}
