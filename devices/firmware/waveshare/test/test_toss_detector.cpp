#include "game/TossDetector.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

namespace {
void expectNear(float actual, float expected, float tolerance) {
  assert(fabsf(actual - expected) <= tolerance);
}

void testHeightFormula() {
  expectNear(TossDetector::heightFromAirtimeMs(1000), 1.2258f, 0.001f);
  expectNear(TossDetector::heightFromAirtimeMs(0), 0.0f, 0.001f);
}

void testValidToss() {
  TossDetector detector;
  detector.arm(1000);
  assert(detector.update(1010, 1.30f).event == TossDetector::Event::None);
  assert(detector.update(1050, 0.10f).event == TossDetector::Event::None);
  const auto started = detector.update(1075, 0.08f);
  assert(started.event == TossDetector::Event::FlightStarted);

  assert(detector.update(1900, 0.12f).event == TossDetector::Event::None);
  const auto caught = detector.update(1950, 2.2f);
  assert(caught.event == TossDetector::Event::TossComplete);
  assert(caught.airtimeMs == 900);
  expectNear(caught.heightMeters, 0.993f, 0.002f);
}

void testDropWithoutLaunchImpulseIsIgnored() {
  TossDetector detector;
  detector.arm(0);
  for (uint32_t now = 10; now < 500; now += 10) {
    assert(detector.update(now, 0.05f).event == TossDetector::Event::None);
  }
  assert(detector.isArmed());
}

void testArmTimeout() {
  TossDetector detector;
  detector.arm(500);
  assert(detector.update(10500, 1.0f).event == TossDetector::Event::TimedOut);
}
} // namespace

int main() {
  testHeightFormula();
  testValidToss();
  testDropWithoutLaunchImpulseIsIgnored();
  testArmTimeout();
  puts("TossDetector tests passed");
  return 0;
}
