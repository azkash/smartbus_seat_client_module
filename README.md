# SmartBus Passenger Seat ESP32-WROOM-32U v1.7

Firmware ini adalah adaptasi `Code_PassengerSeat_LoadScale_21A_Gateway_Debounce_v1.6`
untuk modul ESP32-WROOM-32U (classic ESP32 dengan konektor antena U.FL).

Semua fitur v1.6 tetap dipertahankan:

- Dynamic Seat ID dari aplikasi head unit dan penyimpanan NVS.
- Pemilihan master/gateway serta fallback otomatis.
- WebSocket heartbeat dan diagnostik koneksi.
- Debounce tombol 100 ms dan lockout panggilan 10 detik.
- Pembacaan sensor tekanan dengan averaging dan hysteresis.

## Pinout

| Fungsi | GPIO | Konfigurasi firmware | Catatan wiring |
|---|---:|---|---|
| Sensor tekanan/FSR | 34 | ADC1_CH6, input | Wajib memakai pembagi tegangan eksternal; GPIO34 tidak memiliki pull-up/pull-down internal. |
| Tombol panggil | 27 | `INPUT_PULLUP` | Hubungkan tombol antara GPIO27 dan GND. Aktif saat LOW. |
| Sensor seatbelt | 18 | `INPUT_PULLUP` | Hubungkan kontak buckle antara GPIO18 dan GND. LOW = seatbelt terpasang. |
| Data WS2812B | 25 | output FastLED, GRB | Pasang resistor seri 330-470 ohm dan satukan GND LED dengan GND ESP32. Level shifter 74AHCT125 direkomendasikan. |

Sensor seatbelt dibaca dengan debounce 200 ms dan dilaporkan ke master pada
field `"seatbelt":true/false` (true = buckle terpasang). Bila modul switch yang
dipakai normally-closed (terbuka saat terpasang), ubah
`SEATBELT_FASTENED_LEVEL` di `src/main.cpp` dari `LOW` menjadi `HIGH`.
Master yang memutuskan kapan peringatan "kursi nomor ... tidak memakai
seatbelt" diputar: hanya saat kursi terisi dan seatbelt belum terpasang.

Contoh pembagi tegangan FSR agar nilai ADC naik saat kursi ditekan:

```text
3V3 --- FSR ---+--- GPIO34
               |
              10k ohm
               |
              GND
```

Nilai resistor 10k adalah titik awal. Ambang firmware masih mengikuti v1.6:

```text
occupied ON  : ADC >= 150
occupied OFF : ADC <= 100
```

Lihat nilai `Pressure ADC` melalui serial saat kursi kosong dan diduduki. Sesuaikan
`PRESSURE_OCCUPIED_ON` dan `PRESSURE_OCCUPIED_OFF` di
`include/seat_pressure_logic.h` bila hasil rangkaian berbeda.

WS2812B harus diberi daya dari suplai 5 V yang memadai, bukan dari pin 3V3 ESP32.
Pasang kapasitor sekitar 1000 uF pada jalur 5 V/GND di dekat strip. Untuk modul
ESP32-WROOM-32U, antena eksternal U.FL wajib terpasang sebelum penggunaan radio.

## Perintah serial

```text
setgw 1       gunakan Gateway 1
setgw 0       pilih gateway terkuat otomatis
setgw master  hubungkan langsung ke master
setid 21A     atur ID kursi
clearid       hapus ID dan kembali ke RandomID
status        tampilkan status koneksi dan input
```

## Build dan flash

Jalankan dari folder firmware ini:

```powershell
pio run -e esp32-wroom-32u
pio run -e esp32-wroom-32u -t upload --upload-port COM4
pio device monitor -p COM4 -b 115200
```

Untuk beberapa unit sekaligus:

```powershell
.\flash_all.ps1 -Ports COM4,COM5
```

Jika board tidak otomatis masuk bootloader, tahan tombol BOOT, tekan dan lepas
EN/RESET, mulai upload, lalu lepas BOOT ketika proses koneksi dimulai.
