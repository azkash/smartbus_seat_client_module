import sys
import time
import subprocess
import os

try:
    import serial
    import serial.tools.list_ports as list_ports
except ImportError:
    print("pyserial tidak ditemukan! Pastikan telah terinstall.")
    sys.exit(1)

try:
    import winsound
    def beep_cue():
        winsound.Beep(1200, 100)
    def beep_success():
        winsound.Beep(1500, 150)
        time.sleep(0.05)
        winsound.Beep(2200, 250)
    def beep_error():
        winsound.Beep(600, 400)
except Exception:
    def beep_cue(): pass
    def beep_success(): pass
    def beep_error(): pass

BAUD = 115200
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(BASE_DIR, ".pio", "build", "esp32-wroom-32u")
BOOTLOADER = os.path.join(BUILD_DIR, "bootloader.bin")
PARTITIONS = os.path.join(BUILD_DIR, "partitions.bin")
BOOT_APP0 = r"C:\.pio-napt\packages\framework-arduinoespressif32\tools\partitions\boot_app0.bin"
FIRMWARE = os.path.join(BUILD_DIR, "firmware.bin")
PYTHON_EXE = r"C:\.pio-napt\penv\Scripts\python.exe"
ESPTOOL_PY = r"C:\.pio-napt\packages\tool-esptoolpy\esptool.py"

def find_target_port(preferred_port=None):
    ports = list(list_ports.comports())
    if not ports:
        return None, None
    
    # 1. Cek preferred_port jika tersedia
    if preferred_port:
        for p in ports:
            if p.device.upper() == preferred_port.upper():
                return p.device, p.description

    # 2. Cari perangkat USB-TTL (CH340, CP210, FTDI, USB-to-UART)
    for p in ports:
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if any(k in desc or k in hwid for k in ["ch340", "ch341", "cp210", "ftdi", "usb-serial", "uart", "esp"]):
            return p.device, p.description

    return None, None

def run_flashing(port):
    cmd = [
        PYTHON_EXE,
        ESPTOOL_PY,
        "--chip", "esp32",
        "--port", port,
        "--baud", "115200",
        "--before", "no-reset",
        "--after", "hard-reset",
        "write-flash",
        "-z",
        "--flash-mode", "dio",
        "--flash-freq", "40m",
        "--flash-size", "4MB",
        "0x1000", BOOTLOADER,
        "0x8000", PARTITIONS,
        "0xe000", BOOT_APP0,
        "0x10000", FIRMWARE
    ]
    res = subprocess.run(cmd)
    return res.returncode == 0

def main():
    target_arg = sys.argv[1] if len(sys.argv) > 1 else None
    flashed_count = 0

    print("=" * 65, flush=True)
    print("       SMARTBUS CONTINUOUS AUTO-FLASHER (BATCH PROVISIONING)     ", flush=True)
    print("=" * 65, flush=True)
    print("Mode: Siap mem-flash setiap modul ESP32 baru secara otomatis.", flush=True)
    print("Tekan Ctrl+C di terminal ini jika ingin menghentikan.", flush=True)
    print("=" * 65, flush=True)

    while True:
        # 1. Tunggu port terdeteksi
        port, desc = find_target_port(target_arg)
        if not port:
            print("\r[STANDBY] Menunggu kabel USB-TTL / ESP32 dicolokkan ke PC...", end="", flush=True)
            time.sleep(1)
            continue

        print(f"\n\n[PORT AKTIF] Terhubung ke: {port} ({desc})", flush=True)
        print("Memonitor status ESP32...")
        print("-> TAHAN tombol BOOT (GPIO0), lalu TEKAN & LEPAS tombol RESET (EN) pada ESP32 baru.")

        # 2. Monitor serial stream sampai terdeteksi DOWNLOAD_BOOT
        in_boot_mode = False
        last_remind_time = 0

        while not in_boot_mode:
            # Cek apakah port masih ada
            cur_port, _ = find_target_port(port)
            if not cur_port:
                print(f"\n[INFO] Port {port} terputus (dicabut).", flush=True)
                break

            try:
                ser = serial.Serial(port, BAUD, timeout=0.2)
                loop_start = time.time()
                while time.time() - loop_start < 5:
                    line = ser.readline()
                    if line:
                        text = line.decode('utf-8', errors='ignore').strip()
                        now = time.time()

                        if "boot:0x13" in text or "SPI_FAST_FLASH_BOOT" in text:
                            if now - last_remind_time > 3.0:
                                print(f"[INFO] Terdeteksi modul belum flash / reboot loop ({text[:40]}).", flush=True)
                                print("       >>> Silakan TAHAN BOOT, lalu TEKAN & LEPAS RESET! <<<", flush=True)
                                last_remind_time = now

                        elif any(k in text for k in ["boot:0x03", "boot:0x01", "waiting for download", "DOWNLOAD_BOOT"]):
                            print(f"\n[CUE] Terdeteksi BOOTLOADER MODE! Log: {text}", flush=True)
                            beep_cue()
                            in_boot_mode = True
                            break
                ser.close()
                time.sleep(0.2)
            except Exception as e:
                # Port mungkin sedang sibuk atau dicabut
                time.sleep(0.5)

        if not in_boot_mode:
            continue

        # 3. Eksekusi Flashing
        print("\n" + "=" * 65, flush=True)
        print(f"[FLASHING] Mengirim firmware ke modul pada {port}...", flush=True)
        print("=" * 65, flush=True)

        success = run_flashing(port)

        if success:
            flashed_count += 1
            beep_success()
            print("\n" + "#" * 65, flush=True)
            print(f" [SUKSES #{flashed_count}] FIRMWARE BERHASIL DI-FLASH!", flush=True)
            print(f" Total Modul Selesai Di-Flash: {flashed_count}", flush=True)
            print("#" * 65, flush=True)
            print("1. Tekan tombol RESET pada modul agar mulai berjalan.", flush=True)
            print("2. Cabut modul yang sudah selesai, lalu tancapkan modul berikutnya!\n", flush=True)
            
            # Beri jeda agar tidak flash modul yang sama secara beruntun
            print("Menunggu modul berikutnya...", flush=True)
            time.sleep(3)
        else:
            beep_error()
            print("\n" + "!" * 65, flush=True)
            print(" [GAGAL] Flashing tidak berhasil. Coba ulangi tombol BOOT + RESET.", flush=True)
            print("!" * 65 + "\n", flush=True)
            time.sleep(2)

if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[STOP] Auto-flasher dihentikan oleh user.")
