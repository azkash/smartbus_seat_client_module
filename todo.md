# TODO — Code_PassengerSeat (seat node)

## Reliability (do first — these fix concrete master↔slave issues)
- [x] **[seat]** Heartbeat — `HEARTBEAT_MS` (5s) re-sends state (sensorState=0) in `loop()` to keep the master's `lastSeen` fresh so a still/occupied seat isn't marked offline.
- [x] **[seat]** Button debounce — ISR ignores edges within `BUTTON_DEBOUNCE_MS` (50ms), so one press = one seat event = one announcement.

## Quality
- [ ] **[seat]** Occupancy hysteresis + sample averaging — occupancy flips on a single raw `scale.read()` vs one threshold (`< 2800000`); a weight near the line flip-flops → telemetry spam + LED flicker. Average a few samples and use two thresholds (occupied / vacated).

## Pairs with master "call acknowledge / clear"
- [ ] **[seat]** LED on the calling seat — slave currently ignores incoming WS text. Have the master send `light_<id>` / `clear_<id>` so the calling seat's own LED stays lit until crew acknowledges, closing the loop with the master's `callPending` / `clear_<id>` feature.

## Notes
- [x] Seat id is now stored in NVS (`Preferences`, namespace `seat`). If unset, it falls back to a MAC-derived unique id (`SEAT-AABBCC`). Assign a friendly id per node over serial: `setid=B15` (saves + reboots). Flash one firmware to all seats, then label each in the field.
