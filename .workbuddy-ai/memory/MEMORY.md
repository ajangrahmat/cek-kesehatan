# MEMORY.md — Project: cek-kesehatan

## Sistem Monitoring Fisiologis ESP32 (AViSha Inovasi Indonesia, Invoice #113)

### Arsitektur (PRD v0.5, 12 Nov 2026)
- **Tier 1 (Inti, wajib):** Node HR (MAX30102) + Node Suhu (DS18B20)
- **Tier 2 (Lanjutan, opsional):** Node ECG (AD8232) — aktif saat Tier 1 anomali atau manual
- **Controller:** ESP32 + OLED SH1106 128x64 + MAX98357A (I2S) + speaker + tombol + LED
- **Komunikasi:** ESP-NOW, 1 paket/detik (inti + ECG heartbeat), BPM ECG diperbarui per window 10 detik, timeout 3 detik
- **ESP-NOW transport:** Header bersama `include/espnow_common.h`. NodePacket 18 byte (packed, static_assert). Broadcast peer. Callback recv IDF 5.x dengan guard versi. Controller: queue-based dispatch (FreeRTOS xQueueSend).
- **Audio:** 13 file MP3 di SPIFFS (data/), diputar via MAX98357A. Library: **ESP8266Audio** (AudioFileSourceSPIFFS/AudioGeneratorMP3/AudioOutputI2S), bukan ESP32-audioI2S.
- **Bukan DFPlayer** — sudah diganti MAX98357A sejak v0.2
- **TTS:** edge-tts (Microsoft Neural), suara Gadis (info/perhatian/teknis) + Ardi (gawat). Script `generate_tts.py`. Teks mengacu FIX BIESF.docx (nomor darurat 119/112).

### Build PlatformIO (4 environment, 1 per board)
| Env | Source | Library | Flash | RAM |
|---|---|---|---|---|
| `node_hr` | `src/node_hr.cpp` | SparkFun MAX3010x | 48.2% | 15.0% |
| `node_suhu` | `src/node_suhu.cpp` | OneWire, DallasTemperature | 46.5% | 14.0% |
| `node_ecg` | `src/node_ecg.cpp` | — (ADC bawaan) | 46.4% | 14.0% |
| `controller` | `src/controller.cpp` | U8g2, ESP8266Audio | 57.5% | 15.0% |

`default_envs = node_hr`. `build_src_filter = +<*> -<*.cpp> +<file>` per env. `upload_port` masih komentar (isi COM port manual). Semua 4 env + `buildfs` terverifikasi sukses dan diuji upload oleh pengguna.
Catatan: `src/main.cpp` sudah di-rename jadi `src/node_hr.cpp`; 3 file lain disalin dari `test/` (kode test tetap utuh di test/).

### Logika stabilisasi sensor
- HR: kumpul 10 denyut, rata-rata interval -> BPM. ~8-15 detik. NODE_ID=1, BEATS_TO_AVERAGE=10.
- Suhu: baca tiap 2 detik, 3 baca stabil (delta < 0.2C) -> 6 detik. Timeout 15 dtk. NODE_ID=2. Kirim ESP-NOW 1/detik.
- ECG: sampling 250 Hz, hitung puncak dalam window 10 detik -> BPM = puncak x 6. NODE_ID=3. GPIO 36 (ADC1_CH0, aman saat WiFi aktif). Heartbeat ESP-NOW 1/detik.

### Pin I2S (MAX98357A)
- BCLK: GPIO 25, LRC: GPIO 32, DIN: GPIO 33

### File test
- test/test-max30102-heart-rate.cpp — HR dengan 10 denyut
- test/test-ds18b20.cpp — suhu dengan stabilisasi
- test/test-ecg-node.cpp — ECG window 10 detik
- test/test-oled-sh1106.cpp — OLED
- test/test-spiff-i2s-speaker.cpp — audio I2S

### Track audio yang dipakai vs belum dipakai di controller
- Dipakai: A-01 (startup), A-02/A-03 (HR), A-06-A-09 (suhu), A-13 (sistem tidak lengkap)
- Belum dipanggil: A-04/A-05 (SpO2), A-10 (elektroda lepas), A-11 (HR tidak konsisten), A-12 (node terputus)
- File MP3 semua lengkap (13 file, ±625 KB), tidak perlu dilengkapi.

### Yang masih terbuka
- Mekanisme aktivasi ECG (otomatis/manual)
- Ambang fisiologis final
- Konten first aid
- Sumber daya (invoice: tanpa baterai)
- Klasifikasi SpO2 di controller
- Audio A-10 untuk elektroda lepas
- Cross-check HR ECG vs PPG (A-11)
- Filter MAC ketat (ESPNOW_STRICT_MAC belum aktif, mode pairing)
