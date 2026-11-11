#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <espnow_common.h>

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
//   - Interval baca: 1 detik (DS18B20 11-bit butuh ~375ms konversi)
//   - Threshold stabil: perubahan < 0.3°C antar baca dianggap stabil
//   - Dua perubahan kecil berturut-turut mengunci nilai
//   - Timeout: kalau 5 detik belum stabil, pakai rata-rata terakhir
// ============================================================

namespace {

constexpr uint8_t DS18B20_PIN = 22;
constexpr uint32_t READ_INTERVAL_MS = 1000;       // baca tiap 1 detik
constexpr float STABILITY_THRESHOLD = 0.3f;       // selisih < 0.3C = stabil
constexpr uint8_t STABLE_READS_NEEDED = 2;        // 2 perubahan kecil berturut-turut
constexpr uint32_t STABILITY_TIMEOUT_MS = 5000;   // maks 5 detik nunggu

// PRD Lampiran B: 2=Suhu, Tier 1 (inti, wajib).
// PRD §6.2: node inti kirim 1 paket/detik, terpisah dari laju baca sensor.
constexpr uint8_t NODE_ID = NODE_ID_SUHU;
constexpr uint32_t ESPNOW_SEND_INTERVAL_MS = 1000;
constexpr uint32_t SENSOR_RETRY_MS = 5000;  // coba deteksi ulang sensor tiap 5 dtk

// Ring buffer untuk rata-rata
constexpr uint8_t BUFFER_SIZE = 5;
float readings[BUFFER_SIZE] = {};
uint8_t bufferIndex = 0;
uint8_t bufferCount = 0;

OneWire oneWire(DS18B20_PIN);
DallasTemperature temperatureSensor(&oneWire);

uint32_t lastReadTime = 0;
uint32_t firstReadTime = 0;
uint32_t lastEspnowSendTime = 0;
uint32_t lastSensorProbeTime = 0;
uint16_t sequenceNumber = 0;
bool sensorAvailable = false;

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
    sensorAvailable = false;
    finalTemperature = 0.0f;
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

// Kirim 1 paket/detik; pembacaan sensor berjalan tiap 1 detik.
// Saat TIMEOUT, rata-rata fallback tetap dipakai jika nilainya dalam rentang
// teknis; sensor yang gagal dibaca tetap ditandai sebagai error.
void sendEspnowPacket(uint32_t now) {
  NodePacket pkt = espnowMakePacket(NODE_ID, TIER_CORE, SENSOR_DS18B20,
                                    sequenceNumber, now);

  const bool temperatureInRange =
      finalTemperature >= 30.0f && finalTemperature <= 43.0f;
  if (!sensorAvailable) {
    pkt.err = ERR_SUHU;
  } else if ((state == TempState::LOCKED ||
              state == TempState::TIMEOUT) && temperatureInRange) {
    pkt.q = 1;
    pkt.err = ERR_OK;
  } else if (state == TempState::TIMEOUT) {
    // Fallback di luar rentang teknis tidak boleh dipakai controller.
    pkt.err = ERR_SUHU;
  } else {
    // INIT / STABILIZING: sensor hidup, tetapi nilai belum siap.
    pkt.err = ERR_OK;
  }

  if (temperatureInRange) {
    pkt.temp_x10 = static_cast<int16_t>(lroundf(finalTemperature * 10.0f));
  }

  const bool sent = espnowSendPacket(pkt);
  Serial.printf("[ESP-NOW] %s | state=%s | ",
                sent ? "OK" : "GAGAL", stateLabel(state));
  espnowPrintPacketJson(pkt);
  ++sequenceNumber;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!espnowInitRadio("NODE SUHU / DS18B20")) {
    Serial.println("ESP-NOW gagal. Hentikan board ini.");
    while (true) { delay(1000); }
  }
  if (!espnowNodeBegin()) {
    while (true) { delay(1000); }
  }

  Serial.println("Tes DS18B20 + Stabilisasi - ESP32");
  Serial.printf("Pin data DQ: GPIO %u\n", DS18B20_PIN);
  Serial.println("Gunakan pull-up 4,7 kOhm dari DQ ke 3V3.");
  Serial.println("Resolusi 11-bit: konversi lebih cepat, ketelitian 0,125 C.");
  Serial.printf("Threshold stabil: < %.1f C antar baca\n", STABILITY_THRESHOLD);
  Serial.printf("Butuh %u perubahan stabil (sekitar %lu detik)\n",
                STABLE_READS_NEEDED,
                static_cast<unsigned long>(
                    STABLE_READS_NEEDED * (READ_INTERVAL_MS / 1000)));
  Serial.printf("Timeout: %lu detik\n",
                static_cast<unsigned long>(STABILITY_TIMEOUT_MS / 1000));
  Serial.println("---");

  temperatureSensor.begin();
  temperatureSensor.setWaitForConversion(true);

  const int sensorCount = temperatureSensor.getDeviceCount();
  Serial.printf("Sensor ditemukan: %d\n", sensorCount);
  sensorAvailable = sensorCount > 0;
  if (!sensorAvailable) {
    // Jangan berhenti: tetap kirim paket err=3 agar controller menandai
    // "sensor suhu gagal", bukan sekadar "node terputus".
    Serial.println("DS18B20 tidak ditemukan. Periksa DQ, 3V3, GND, dan pull-up.");
    Serial.println("Node tetap mengirim paket gangguan tiap detik.");
  } else {
    temperatureSensor.setResolution(11);
  }
  lastReadTime = millis();
  lastEspnowSendTime = millis();
}

void loop() {
  const uint32_t now = millis();

  if (sensorAvailable) {
    if (now - lastReadTime >= READ_INTERVAL_MS) {
      lastReadTime = now;
      readTemperature();
    }
  } else if (now - lastSensorProbeTime >= SENSOR_RETRY_MS) {
    // Sensor bisa dicolok ulang tanpa reset board.
    lastSensorProbeTime = now;
    if (temperatureSensor.getDeviceCount() > 0) {
      Serial.println("DS18B20 terdeteksi kembali. Ulangi stabilisasi.");
      temperatureSensor.setResolution(11);
      sensorAvailable = true;
      resetMeasurement();
      lastReadTime = now;
    }
  }

  // PRD §6.2: node inti wajib kirim 1 paket/detik. Tetap kirim walau
  // sensor hilang, supaya controller membedakan "sensor gagal" (err=3)
  // dari "node terputus" (timeout 3 detik).
  if (now - lastEspnowSendTime >= ESPNOW_SEND_INTERVAL_MS) {
    lastEspnowSendTime = now;
    sendEspnowPacket(now);
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
