#pragma once

#include <stdint.h>

// 12-bit ADC thresholds for an FSR wired from 3.3 V to GPIO34 with a 10 kOhm
// resistor from GPIO34 to GND. Pressure raises the ADC reading.
constexpr uint16_t PRESSURE_OCCUPIED_ON = 150;
constexpr uint16_t PRESSURE_OCCUPIED_OFF = 100;

inline bool updateSeatOccupied(bool currentlyOccupied, uint16_t reading) {
  if (currentlyOccupied) {
    return reading > PRESSURE_OCCUPIED_OFF;
  }
  return reading >= PRESSURE_OCCUPIED_ON;
}
