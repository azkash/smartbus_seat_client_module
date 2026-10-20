#include <Arduino.h>
#include <unity.h>

#include "seat_pressure_logic.h"

void test_free_seat_stays_free_below_occupied_threshold() {
  TEST_ASSERT_FALSE(updateSeatOccupied(false, 149));
}

void test_free_seat_becomes_occupied_at_upper_threshold() {
  TEST_ASSERT_TRUE(updateSeatOccupied(false, 150));
}

void test_occupied_seat_stays_occupied_in_hysteresis_band() {
  TEST_ASSERT_TRUE(updateSeatOccupied(true, 101));
}

void test_occupied_seat_becomes_free_at_lower_threshold() {
  TEST_ASSERT_FALSE(updateSeatOccupied(true, 100));
}

void setup() {
  delay(2000);
  UNITY_BEGIN();
  RUN_TEST(test_free_seat_stays_free_below_occupied_threshold);
  RUN_TEST(test_free_seat_becomes_occupied_at_upper_threshold);
  RUN_TEST(test_occupied_seat_stays_occupied_in_hysteresis_band);
  RUN_TEST(test_occupied_seat_becomes_free_at_lower_threshold);
  UNITY_END();
}

void loop() {}
