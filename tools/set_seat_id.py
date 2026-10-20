#!/usr/bin/env python3
"""
set_seat_id.py - assign a seat id to a PassengerSeat ESP32 over serial.

The firmware persists the id in NVS and reboots. Re-flashing the firmware does
NOT erase it; only a full chip erase (`pio run -t erase`) does. So you flash the
same firmware to every seat, then label each one once with this tool.

Examples:
  python set_seat_id.py --id B15                 # auto-detect the port
  python set_seat_id.py --port COM7 --id B15
  python set_seat_id.py --port COM7 --read       # just show the current id
  python set_seat_id.py --list                   # list serial ports

Requires:  pip install pyserial
"""

import argparse
import re
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial not installed. Run:  pip install pyserial")

DEFAULT_BAUD = 115200
ID_RE = re.compile(r"\[Seat\] ID = (\S+)")


def list_serial_ports():
    return list(list_ports.comports())


def pick_port():
    ports = list_serial_ports()
    if not ports:
        sys.exit("No serial ports found. Plug in the board, or pass --port.")
    if len(ports) == 1:
        print(f"Using {ports[0].device} ({ports[0].description})")
        return ports[0].device
    print("Multiple serial ports found - choose one with --port:")
    for p in ports:
        print(f"  {p.device:12} {p.description}")
    sys.exit(1)


def read_for(ser, seconds, echo=True):
    """Read serial for `seconds`, optionally echoing; return captured text."""
    end = time.time() + seconds
    buf = ""
    while time.time() < end:
        line = ser.readline().decode("utf-8", "replace")
        if line:
            buf += line
            if echo:
                sys.stdout.write(line)
                sys.stdout.flush()
    return buf


def latest_id(text):
    """Return the last '[Seat] ID = X' value seen in `text`, or None."""
    found = ID_RE.findall(text)
    return found[-1] if found else None


def main():
    ap = argparse.ArgumentParser(
        description="Assign a seat id to a PassengerSeat ESP32 over serial."
    )
    ap.add_argument("--port", help="serial port (e.g. COM7 or /dev/ttyUSB0); auto-detect if omitted")
    ap.add_argument("--id", help="seat id to assign (e.g. B15), 1-31 chars")
    ap.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    ap.add_argument("--read", action="store_true", help="read the current seat id and exit")
    args = ap.parse_args()

    if args.list:
        ports = list_serial_ports()
        if not ports:
            print("No serial ports found.")
        for p in ports:
            print(f"{p.device:12} {p.description}")
        return

    if not args.read:
        if not args.id:
            sys.exit("Provide --id <name> to set, or --read to query.")
        if not (1 <= len(args.id) < 32):
            sys.exit("Seat id must be 1-31 characters.")

    port = args.port or pick_port()

    try:
        ser = serial.Serial(port, args.baud, timeout=0.3)
    except serial.SerialException as e:
        sys.exit(f"Could not open {port}: {e}")

    with ser:
        # Opening the port resets the board on most ESP32 USB-serial chips.
        time.sleep(1.0)
        ser.reset_input_buffer()

        if args.read:
            ser.write(b"id?\n")
            ser.flush()
            out = read_for(ser, 2, echo=False)
            cur = latest_id(out)
            print(f"Current seat id: {cur if cur else '(no response - press EN/reset and retry)'}")
            return

        print("--- board output ---")
        ser.write(f"setid={args.id}\n".encode())
        ser.flush()
        print(f">>> sent: setid={args.id}")

        # Firmware saves + reboots; capture the reboot banner to confirm.
        out = read_for(ser, 6)
        got = latest_id(out)
        print("\n--------------------")
        if got == args.id:
            print(f"OK - seat id set to '{args.id}' (persisted in NVS).")
        else:
            print(f"WARNING: could not confirm. Saw id={got!r}, expected {args.id!r}.")
            print("Re-run with --read to verify.")
            sys.exit(2)


if __name__ == "__main__":
    main()
