# Seat load-scale test (1 device)

This firmware variant replaces the VL53L0X distance sensor with an analog
resistive pressure sensor on GPIO34 (ADC1_CH6).

## Wiring

Use a voltage divider:

```text
3.3V --- resistive pressure sensor ---+--- GPIO34
                                      |
                                    10 kOhm
                                      |
                                     GND
```

Pressure must make the GPIO34 ADC value rise. Never feed more than 3.3 V into
the GPIO. GPIO34 has no internal pull-up or pull-down, so the divider resistor
must be installed and the input must not be left floating.

This wiring is for an analog resistive pressure sensor/FSR. A four-wire load
cell with an HX711 is not compatible with this one-pin circuit.

## Initial thresholds

- Occupied: ADC reaches 150 or more.
- Free: ADC falls to 100 or less.
- Between 101 and 149, the previous state is retained to prevent flicker.

Open the serial monitor at 115200 baud and observe `Pressure ADC`. Record the
stable empty and occupied values, then adjust the two constants in
`include/seat_pressure_logic.h` if necessary.

## Build and upload

```text
pio run -e esp32-wroom-32u
pio run -e esp32-wroom-32u -t upload --upload-port COM4
pio device monitor -b 115200
```
