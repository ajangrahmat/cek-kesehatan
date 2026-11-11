#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ============================================================
// Tes DS18B20 dengan logika stabilisasi suhu
//
// Konsep (mirip HR yang butuh 10 denyut baru muncul angka):
// Suhu tubuh tidak langsung stabil saat sensor menempel.
// Sensor baca terus, tapi nilai "final" baru muncul setelah
// pembacaan stabil dalam rentang tertentu selama N detik.
//
// Tahapan:
//   INIT    -> sensor baru menempel, suhu masih naik/turun cepat
//   STABIL  -> perubahan antar baca sudah kecil, mulai dihitung
//   LOCKED  -> stabil selama X detik, nilai final keluar
//
// Parameter yang bisa diatur:
//   - Interval baca: 2 detik (DS18B20 12-bit butuh ~750ms konversi)
//   - Threshold stabil: perubahan < 0.2°C antar baca dianggap stabil
//   - Durasi stabil: 5 detik (3 baca berturut-turut dalam threshold)
//   - Timeout: kalau 15 detik belum stabil, pakai rata-rata terakhir
// ============================================================

namespace {

constexpr uint8_t DS18B20_PIN = 22;
constexpr uint32_t READ_INTERVAL_MS = 2000;       // baca tiap 2 detik
constexpr float STABILITY_THRESHOLD = 0.2f;       // selisih < 0.2C = stabil
constexpr uint8_t STABLE_READS_NEEDED = 3;        // 3x baca stabil = 6 detik
constexpr uint32_t STABILITY_TIMEOUT_MS = 15000;  // maks 15 detik nunggu

// Ring buffer untuk rata-rata
constexpr uint8_t BUFFER_SIZE = 5;
float readings[BUFFER_SIZE] = {};
uint8_t bufferIndex = 0;
uint8_t bufferCount = 0;

OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureSensor(&oneWire);

uint32_t lastReadTime = 0;
uint32_t firstReadTime = 0;

float lastReading = 0.0f;
float finalTemperature = 0.0f;

// State machine
enum class TempState {
  INIT,       // baru mulai, suhu masih berayun
  STABILIZING,// perubahan sudah kecil, menghitung
  LOCKED,     // stabil, nilai final keluar
  TIMEOUT     // tidak stabil dalam batas waktu, pakai rata-rata
};

TempState state = TempState::INIT;
uint8_t stableCount = 0;

float bufferAverage() {
  if (bufferCount == 0) return 0.0f;
  float sum = 0.0f;
  const uint8_t count = min(bufferCount, BUFFER_SIZE);
  for (uint8_t i = 0; i < count; ++i) {
    sum += readings[i];
  }
  return sum / count;
}

void pushReading(float temp) {
  readings[bufferIndex] = temp;
  bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
  if (bufferCount < BUFFER_SIZE) ++bufferCount;
}

const char* stateLabel(TempState s) {
  switch (s) {
    case TempState::INIT:        return "INIT";
    case TempState::STABILIZING: return "STABIL";
    case TempState::LOCKED:      return "LOCKED";
    case TempState::TIMEOUT:     return "TIMEOUT";
  }
  return "?";
}

void readTemperature() {
  temperatureSensor.requestTemperatures();
  const float temperatureC = temperatureSensor.getTempCByIndex(0);

  if (temperatureC == DEVICE_DISCONNECTED_C || !isfinite(temperatureC)) {
    Serial.println("Gagal membaca DS18B20. Periksa kabel dan resistor pull-up.");
    return;
  }

  const uint32_t now = millis();
  pushReading(temperatureC);
  const float avg = bufferAverage();

  Serial.printf("[%s] Baca: %.2f C | Avg: %.2f C",
                stateLabel(state), temperatureC, avg);

  if (state == TempState::LOCKED) {
    // Sudah locked: tetap kirim update tapi tidak menunggu stabil lagi
    finalTemperature = avg;
    Serial.printf(" | FINAL: %.2f C\n", finalTemperature);
    return;
  }

  if (state == TempState::INIT) {
    if (firstReadTime == 0) {
      firstReadTime = now;
    }
    const float delta = abs(temperatureC - lastReading);
    if (lastReading > 0 && delta < STABILITY_THRESHOLD) {
      state = TempState::STABILIZING;
      stableCount = 1;
      Serial.printf(" | mulai stabil (1/%u)\n", STABLE_READS_NEEDED);
    } else {
      Serial.printf(" | delta %.2f C, menunggu stabil\n", delta);
    }
  } else if (state == TempState::STABILIZING) {
    const float delta = abs(temperatureC - lastReading);
    if (delta < STABILITY_THRESHOLD) {
      ++stableCount;
      if (stableCount >= STABLE_READS_NEEDED) {
        state = TempState::LOCKED;
        finalTemperature = avg;
        Serial.printf(" | STABIL! FINAL: %.2f C\n", finalTemperature);
      } else {
        Serial.printf(" | stabil (%u/%u)\n", stableCount, STABLE_READS_NEEDED);
      }
    } else {
      // Berayun lagi, reset counter
      stableCount = 0;
      state = TempState::INIT;
      Serial.printf(" | delta %.2f C, reset stabil\n", delta);
    }
  }

  // Timeout: kalau terlalu lama tidak stabil, pakai rata-rata
  if (state != TempState::LOCKED && firstReadTime > 0 &&
      now - firstReadTime >= STABILITY_TIMEOUT_MS) {
    state = TempState::TIMEOUT;
    finalTemperature = avg;
    Serial.printf(" | TIMEOUT! Pakai rata-rata: %.2f C\n", finalTemperature);
  }

  lastReading = temperatureC;
}

void resetMeasurement() {
  state = TempState::INIT;
  stableCount = 0;
  firstReadTime = 0;
  lastReading = 0.0f;
  finalTemperature = 0.0f;
  bufferIndex = 0;
  bufferCount = 0;
  memset(readings, 0, sizeof(readings));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("Tes DS18B20 + Stabilisasi - ESP32");
  Serial.printf("Pin data DQ: GPIO %u\n", DS18B20_PIN);
  Serial.println("Gunakan pull-up 4,7 kOhm dari DQ ke 3V3.");
  Serial.printf("Threshold stabil: < %.1f C antar baca\n", STABILITY_THRESHOLD);
  Serial.printf("Butuh %u baca stabil (= %u detik)\n",
                STABLE_READS_NEEDED,
                STABLE_READS_NEEDED * (READ_INTERVAL_MS / 1000));
  Serial.printf("Timeout: %u detik\n", STABILITY_TIMEOUT_MS / 1000);
  Serial.println("---");

  temperatureSensor.begin();
  temperatureSensor.setWaitForConversion(true);

  const int sensorCount = temperatureSensor.getDeviceCount();
  Serial.printf("Sensor ditemukan: %d\n", sensorCount);
  if (sensorCount == 0) {
    Serial.println("DS18B20 tidak ditemukan. Periksa DQ, 3V3, GND, dan pull-up.");
    return;
  }

  temperatureSensor.setResolution(12);
  lastReadTime = millis();
}

void loop() {
  if (temperatureSensor.getDeviceCount() == 0) {
    delay(READ_INTERVAL_MS);
    return;
  }

  const uint32_t now = millis();
  if (now - lastReadTime >= READ_INTERVAL_MS) {
    lastReadTime = now;
    readTemperature();
  }

  // Tombol simulasi reset (GPIO 0 = BOOT button di ESP32 DevKit)
  // Tekan untuk reset pengukuran (mis. ganti subjek)
  if (digitalRead(0) == LOW) {
    delay(50);
    if (digitalRead(0) == LOW) {
      Serial.println("\n>>> RESET pengukuran <<<\n");
      resetMeasurement();
      while (digitalRead(0) == LOW) {
        delay(10);
      }
    }
  }
}
