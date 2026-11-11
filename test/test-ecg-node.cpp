#include <Arduino.h>

// ============================================================
// Tes ECG (AD8232) — Hitung BPM berdasarkan window waktu
//
// Logika: mirip dokter hitung denyut dari strip ECG.
// Ambil sinyal selama N detik, hitung berapa puncak (R-peak)
// yang lewat threshold, baru konversi ke BPM.
//
//   BPM = (jumlah puncak dalam window) * (60 / window_detik)
//
// Contoh: window 10 detik, dapat 12 puncak -> 12 * 6 = 72 BPM
//
// State machine:
//   INIT      -> mulai, belum ada data
//   SAMPLING  -> elektroda OK, sedang kumpul window
//   LOCKED    -> window selesai, HR keluar, mulai window baru
//   LEADS_OFF -> elektroda lepas, reset
//
// Parameter:
//   - Window: 10 detik (cukup untuk stabil, tidak terlalu lama)
//   - Sample rate: 250 Hz (2500 sample per window)
//   - Threshold: dinamis dari rata-rata + margin
//   - Min amplitude: di bawah ini = sinyal datar
// ============================================================

namespace {

// Pin
constexpr uint8_t ECG_PIN = 36;       // ADC1_CH0, input only
constexpr uint8_t LO_PLUS_PIN = 26;   // deteksi elektroda lepas (+)
constexpr uint8_t LO_MINUS_PIN = 25;  // deteksi elektroda lepas (-)

// Window waktu
constexpr uint32_t WINDOW_MS = 10000;             // 10 detik per window
constexpr uint32_t SAMPLE_RATE_HZ = 250;
constexpr uint32_t SAMPLE_INTERVAL_US = 1000000UL / SAMPLE_RATE_HZ;

// Threshold deteksi puncak
constexpr float THRESHOLD_MARGIN = 0.4f;   // puncak harus > baseline + 40% amplitudo
constexpr uint16_t MIN_AMPLITUDE = 50;      // P2P di bawah ini = sinyal datar

// BPM valid range
constexpr float MIN_VALID_BPM = 30.0f;
constexpr float MAX_VALID_BPM = 220.0f;

// Reporting
constexpr uint32_t REPORT_INTERVAL_MS = 1000;

// State machine
enum class EcgState {
  INIT,
  SAMPLING,
  LOCKED,
  LEADS_OFF
};

EcgState state = EcgState::INIT;

// Sampling
uint32_t nextSampleTimeUs = 0;
uint16_t ecgSample = 0;
uint8_t loPlus = LOW;
uint8_t loMinus = LOW;

// Window buffer — simpan sample untuk hitung baseline & threshold
// Tidak simpan semua 2500 sample, cukup track min/max/sum on the fly
uint32_t windowStartTime = 0;
uint16_t windowSampleCount = 0;
uint32_t windowSum = 0;        // untuk rata-rata (baseline)
uint16_t windowMin = UINT16_MAX;
uint16_t windowMax = 0;
uint16_t peakCount = 0;

// Threshold tracking
float runningBaseline = 2048.0f;
float runningAmplitude = 0;
bool aboveThreshold = false;

// Hasil
float finalBpm = 0;
// Timing
uint32_t lastReportTime = 0;

const char* stateLabel(EcgState s) {
  switch (s) {
    case EcgState::INIT:      return "init";
    case EcgState::SAMPLING:  return "sampling";
    case EcgState::LOCKED:    return "locked";
    case EcgState::LEADS_OFF: return "leads_off";
  }
  return "unknown";
}

void changeState(EcgState newState) {
  if (state == newState) return;
  state = newState;
}

void resetWindow() {
  windowSampleCount = 0;
  windowSum = 0;
  windowMin = UINT16_MAX;
  windowMax = 0;
  peakCount = 0;
  aboveThreshold = false;
}

void startNewWindow(uint32_t nowMs) {
  resetWindow();
  windowStartTime = nowMs;
}

void countPeak(uint16_t sample, float threshold) {
  if (!aboveThreshold && static_cast<float>(sample) > threshold) {
    aboveThreshold = true;
  } else if (aboveThreshold && static_cast<float>(sample) < threshold) {
    // Puncak terlewati (naik lalu turun melewati threshold)
    aboveThreshold = false;
    peakCount++;
  }
}

void processWindow(uint32_t nowMs) {
  const float windowDurationSec = static_cast<float>(nowMs - windowStartTime) / 1000.0f;
  const uint16_t p2p = windowMax - windowMin;

  if (p2p < MIN_AMPLITUDE) {
    finalBpm = 0;
    changeState(EcgState::INIT);
  } else {
    finalBpm = static_cast<float>(peakCount) * (60.0f / windowDurationSec);

    if (finalBpm < MIN_VALID_BPM || finalBpm > MAX_VALID_BPM) {
      finalBpm = 0;
      changeState(EcgState::INIT);
    } else {
      changeState(EcgState::LOCKED);
    }
  }

  startNewWindow(nowMs);
}

void sampleEcg() {
  const uint16_t ecgValue = static_cast<uint16_t>(analogRead(ECG_PIN));
  const uint32_t nowMs = millis();

  ecgSample = ecgValue;
  loPlus = static_cast<uint8_t>(digitalRead(LO_PLUS_PIN));
  loMinus = static_cast<uint8_t>(digitalRead(LO_MINUS_PIN));

  if (loPlus == HIGH || loMinus == HIGH) {
    if (state != EcgState::LEADS_OFF) {
      changeState(EcgState::LEADS_OFF);
      resetWindow();
    }
    return;
  }

  // Elektroda menempel kembali
  if (state == EcgState::LEADS_OFF) {
    changeState(EcgState::INIT);
  }

  if (state == EcgState::INIT) {
    changeState(EcgState::SAMPLING);
    startNewWindow(nowMs);
  }

  if (state == EcgState::SAMPLING || state == EcgState::LOCKED) {
    // Akumulasi window
    windowSampleCount++;
    windowSum += ecgValue;
    if (ecgValue < windowMin) windowMin = ecgValue;
    if (ecgValue > windowMax) windowMax = ecgValue;

    // Update baseline & amplitude tracking (running)
    const float avg = static_cast<float>(windowSum) / windowSampleCount;
    runningBaseline = avg;
    const float p2p = static_cast<float>(windowMax - windowMin);
    runningAmplitude = p2p;

    // Threshold: baseline + margin dari amplitudo
    const float threshold = runningBaseline + (runningAmplitude * THRESHOLD_MARGIN);

    // Hitung puncak
    countPeak(ecgValue, threshold);

    // Cek apakah window sudah selesai
    if (nowMs - windowStartTime >= WINDOW_MS) {
      processWindow(nowMs);
    }
  }
}

void reportStatus(uint32_t nowMs) {
  if (nowMs - lastReportTime < REPORT_INTERVAL_MS) return;
  lastReportTime = nowMs;

  const uint32_t elapsed = nowMs - windowStartTime;
  const uint16_t p2p = windowSampleCount > 0 ? windowMax - windowMin : 0;
  const bool valid = state == EcgState::LOCKED && finalBpm > 0;

  Serial.printf(
      "{\"ms\":%lu,\"state\":\"%s\",\"adc\":%u,\"lo_plus\":%u,"
      "\"lo_minus\":%u,\"elapsed_ms\":%lu,\"samples\":%u,"
      "\"p2p\":%u,\"peaks\":%u,\"bpm\":%.1f,\"valid\":%s}\n",
      static_cast<unsigned long>(nowMs), stateLabel(state), ecgSample,
      loPlus, loMinus, static_cast<unsigned long>(elapsed), windowSampleCount,
      p2p, peakCount, valid ? finalBpm : 0.0f, valid ? "true" : "false");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  analogReadResolution(12);
  pinMode(LO_PLUS_PIN, INPUT);
  pinMode(LO_MINUS_PIN, INPUT);

  nextSampleTimeUs = micros();
  lastReportTime = millis();
  windowStartTime = millis();
}

void loop() {
  const uint32_t nowUs = micros();

  if (static_cast<int32_t>(nowUs - nextSampleTimeUs) >= 0) {
    nextSampleTimeUs += SAMPLE_INTERVAL_US;
    if (static_cast<int32_t>(nowUs - nextSampleTimeUs) >= 0) {
      nextSampleTimeUs = nowUs + SAMPLE_INTERVAL_US;
    }
    sampleEcg();
  }

  const uint32_t nowMs = millis();
  reportStatus(nowMs);
}
