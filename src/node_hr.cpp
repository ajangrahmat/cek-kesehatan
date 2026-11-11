#include <Arduino.h>
#include <Wire.h>
#include <MAX30105.h>
#include <heartRate.h>
#include <spo2_algorithm.h>
#include <espnow_common.h>

namespace {

// PRD Lampiran B: 1=HR, 2=Suhu, 3=ECG. Sebelumnya salah di sini (2).
constexpr uint8_t NODE_ID = NODE_ID_HR;
constexpr uint8_t I2C_SDA = 21;
constexpr uint8_t I2C_SCL = 22;
constexpr uint32_t REPORT_INTERVAL_MS = 1000;  // PRD §6.2: 1 paket/detik
constexpr uint32_t FINGER_IR_THRESHOLD = 5000;
constexpr uint32_t FINGER_LOSS_TIMEOUT_MS = 750;
constexpr uint8_t PPG_SAMPLE_AVERAGE = 4;
constexpr uint32_t PPG_SAMPLE_RATE_HZ = 100;
constexpr uint32_t PPG_SAMPLE_INTERVAL_MS = 1000 / PPG_SAMPLE_RATE_HZ;
constexpr uint32_t SENSOR_SAMPLE_RATE_HZ =
    PPG_SAMPLE_RATE_HZ * PPG_SAMPLE_AVERAGE;
constexpr int32_t SPO2_UPDATE_SAMPLES = 25;
// PRD v0.3: angka BPM baru muncul setelah 10 interval denyut valid dirata-rata.
constexpr uint8_t BEATS_TO_AVERAGE = 10;
constexpr float MIN_VALID_BPM = 30.0f;
constexpr float MAX_VALID_BPM = 220.0f;
constexpr uint32_t MIN_BEAT_INTERVAL_MS = 300;
constexpr float MIN_PULSE_AMPLITUDE = 15.0f;
constexpr float MAX_PULSE_AMPLITUDE = 5000.0f;

MAX30105 sensor;
uint16_t sequenceNumber = 0;
uint32_t lastReportTime = 0;
uint32_t ppgSampleTime = 0;
uint32_t lastFingerSeenTime = 0;
uint32_t lastBeatTime = 0;
uint32_t lastFallbackBeatTime = 0;
float bpmSamples[BEATS_TO_AVERAGE] = {};
uint8_t bpmSampleCount = 0;
float averageBpm = 0;
float irBaseline = 0;
float filteredPulse = 0;
float previousFilteredPulse = 0;
float pulseMinimum = 0;
float pulseMaximum = 0;
int32_t spo2Value = 0;
int32_t algorithmHeartRate = 0;
int8_t spo2Valid = 0;
int8_t algorithmHeartRateValid = 0;
bool fingerPresent = false;
uint32_t irBuffer[BUFFER_SIZE] = {};
uint32_t redBuffer[BUFFER_SIZE] = {};
int32_t ppgSampleCount = 0;
uint32_t pendingIrSamples[SPO2_UPDATE_SAMPLES] = {};
uint32_t pendingRedSamples[SPO2_UPDATE_SAMPLES] = {};
int32_t pendingSampleCount = 0;

bool findSensorAddress() {
  bool found = false;
  Serial.println("Pemindaian alamat I2C:");
  for (uint8_t address = 1; address < 127; ++address) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  Perangkat ditemukan di 0x%02X\n", address);
      found = found || address == MAX30105_ADDRESS;
    }
  }
  if (!found) {
    Serial.println("  MAX30102 biasanya menggunakan alamat 0x57.");
  }
  return found;
}

void resetMeasurement() {
  memset(bpmSamples, 0, sizeof(bpmSamples));
  bpmSampleCount = 0;
  averageBpm = 0;
  lastBeatTime = 0;
  lastFallbackBeatTime = 0;
  irBaseline = 0;
  filteredPulse = 0;
  previousFilteredPulse = 0;
  pulseMinimum = 0;
  pulseMaximum = 0;
  spo2Value = 0;
  algorithmHeartRate = 0;
  spo2Valid = 0;
  algorithmHeartRateValid = 0;
  ppgSampleCount = 0;
  pendingSampleCount = 0;
}

bool detectWeakPulse(uint32_t infrared, uint32_t sampleTimeMs) {
  const float sample = static_cast<float>(infrared);
  if (irBaseline == 0) {
    irBaseline = sample;
    previousFilteredPulse = 0;
    pulseMinimum = 0;
    pulseMaximum = 0;
    return false;
  }

  irBaseline += (sample - irBaseline) * 0.0625f;
  filteredPulse = filteredPulse * 0.75f + (sample - irBaseline) * 0.25f;
  pulseMinimum = min(pulseMinimum, filteredPulse);
  pulseMaximum = max(pulseMaximum, filteredPulse);

  const bool risingZeroCrossing =
      previousFilteredPulse <= 0 && filteredPulse > 0;
  const float pulseAmplitude = pulseMaximum - pulseMinimum;
  const bool beat =
      risingZeroCrossing &&
      pulseAmplitude >= MIN_PULSE_AMPLITUDE &&
      pulseAmplitude <= MAX_PULSE_AMPLITUDE &&
      (lastFallbackBeatTime == 0 ||
       sampleTimeMs - lastFallbackBeatTime >= MIN_BEAT_INTERVAL_MS);

  if (beat) {
    lastFallbackBeatTime = sampleTimeMs;
  }
  if (risingZeroCrossing) {
    pulseMinimum = filteredPulse;
    pulseMaximum = filteredPulse;
  }
  previousFilteredPulse = filteredPulse;
  return beat;
}

void calculateSpO2() {
  maxim_heart_rate_and_oxygen_saturation(
      irBuffer, BUFFER_SIZE, redBuffer, &spo2Value, &spo2Valid,
      &algorithmHeartRate, &algorithmHeartRateValid);

  if (!spo2Valid || spo2Value < 70 || spo2Value > 100) {
    spo2Value = 0;
    spo2Valid = 0;
  }
  if (!algorithmHeartRateValid ||
      algorithmHeartRate < MIN_VALID_BPM ||
      algorithmHeartRate > MAX_VALID_BPM) {
    algorithmHeartRate = 0;
    algorithmHeartRateValid = 0;
  }
}

void processSample(uint32_t infrared, uint32_t red) {
  ppgSampleTime += PPG_SAMPLE_INTERVAL_MS;
  if (infrared >= FINGER_IR_THRESHOLD) {
    if (!fingerPresent) {
      resetMeasurement();
    }
    fingerPresent = true;
    lastFingerSeenTime = ppgSampleTime;
  } else if (fingerPresent &&
             ppgSampleTime - lastFingerSeenTime >= FINGER_LOSS_TIMEOUT_MS) {
    fingerPresent = false;
    resetMeasurement();
    return;
  }

  if (!fingerPresent) {
    return;
  }

  const bool libraryBeat =
      checkForBeat(static_cast<int32_t>(infrared));
  const bool weakPulseBeat =
      detectWeakPulse(infrared, ppgSampleTime);
  if (libraryBeat || weakPulseBeat) {
    if (lastBeatTime != 0 &&
        ppgSampleTime - lastBeatTime >= MIN_BEAT_INTERVAL_MS) {
      const uint32_t interval = ppgSampleTime - lastBeatTime;
      const float bpm = 60000.0f / interval;
      if (bpm >= MIN_VALID_BPM && bpm <= MAX_VALID_BPM) {
        bpmSamples[bpmSampleCount++] = bpm;
        if (bpmSampleCount == BEATS_TO_AVERAGE) {
          float total = 0;
          for (float sample : bpmSamples) {
            total += sample;
          }
          averageBpm = total / BEATS_TO_AVERAGE;
          bpmSampleCount = 0;
        }
      }
      Serial.printf("Denyut terdeteksi: %.0f BPM (%u/%u)\n",
                    bpm, bpmSampleCount, BEATS_TO_AVERAGE);
      lastBeatTime = ppgSampleTime;
    } else if (lastBeatTime == 0) {
      lastBeatTime = ppgSampleTime;
    }
  }

  if (ppgSampleCount < BUFFER_SIZE) {
    irBuffer[ppgSampleCount] = infrared;
    redBuffer[ppgSampleCount] = red;
    ++ppgSampleCount;
    if (ppgSampleCount == BUFFER_SIZE) {
      calculateSpO2();
    }
  } else {
    pendingIrSamples[pendingSampleCount] = infrared;
    pendingRedSamples[pendingSampleCount] = red;
    ++pendingSampleCount;
    if (pendingSampleCount == SPO2_UPDATE_SAMPLES) {
      memmove(irBuffer, irBuffer + SPO2_UPDATE_SAMPLES,
              (BUFFER_SIZE - SPO2_UPDATE_SAMPLES) * sizeof(irBuffer[0]));
      memmove(redBuffer, redBuffer + SPO2_UPDATE_SAMPLES,
              (BUFFER_SIZE - SPO2_UPDATE_SAMPLES) * sizeof(redBuffer[0]));
      memcpy(irBuffer + BUFFER_SIZE - SPO2_UPDATE_SAMPLES, pendingIrSamples,
             sizeof(pendingIrSamples));
      memcpy(redBuffer + BUFFER_SIZE - SPO2_UPDATE_SAMPLES, pendingRedSamples,
             sizeof(pendingRedSamples));
      pendingSampleCount = 0;
      calculateSpO2();
    }
  }
}

void updateSensor() {
  sensor.check();
  while (sensor.available()) {
    const uint32_t infrared = sensor.getFIFOIR();
    const uint32_t red = sensor.getFIFORed();
    sensor.nextSample();
    processSample(infrared, red);
  }
}

void reportNodeData(uint32_t now) {
  // Controller memakai HR dan suhu sebagai sensor inti; SpO2 tidak menjadi
  // syarat validitas HR. Nilai HR baru valid setelah 10 interval denyut.
  const bool heartRateValid = averageBpm > 0;
  const uint8_t valid = fingerPresent && heartRateValid ? 1 : 0;
  const int16_t heartRate = heartRateValid
      ? static_cast<int16_t>(lroundf(averageBpm))
      : NILAI_TIDAK_ADA;

  // Kirim payload biner via ESP-NOW; JSON di bawah hanya untuk log.
  NodePacket pkt = espnowMakePacket(NODE_ID, TIER_CORE, SENSOR_MAX30102,
                                    sequenceNumber, now);
  pkt.q    = valid;
  pkt.err  = fingerPresent ? ERR_OK : ERR_PPG_BURUK;
  pkt.hr   = heartRate;
  pkt.spo2 = spo2Valid ? static_cast<uint8_t>(spo2Value) : 0;

  const bool sent = espnowSendPacket(pkt);
  Serial.printf("[ESP-NOW] %s | finger=%u spo2_valid=%u "
                "hr_samples=%u/%u hr_valid=%u hr=%.0f | ",
                sent ? "OK" : "GAGAL",
                static_cast<unsigned>(fingerPresent),
                static_cast<unsigned>(spo2Valid),
                static_cast<unsigned>(bpmSampleCount),
                static_cast<unsigned>(BEATS_TO_AVERAGE),
                static_cast<unsigned>(heartRateValid),
                averageBpm);
  espnowPrintPacketJson(pkt);
  ++sequenceNumber;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!espnowInitRadio("NODE HR / MAX30102")) {
    Serial.println("ESP-NOW gagal. Hentikan board ini.");
    while (true) { delay(1000); }
  }
  if (!espnowNodeBegin()) {
    while (true) { delay(1000); }
  }

  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.printf("{\"type\":\"max30102_node\",\"node_id\":%u,\"tier\":%u}\n",
                static_cast<unsigned>(NODE_ID),
                static_cast<unsigned>(TIER_CORE));
  Serial.println("{\"note\":\"Tes prototipe, bukan alat diagnosis medis.\"}");

  if (!findSensorAddress()) {
    Serial.println("MAX30102 tidak merespons di alamat 0x57.");
    Serial.println("Periksa daya, GND, SDA=GPIO21, SCL=GPIO22, dan kabel.");
    while (true) {
      delay(1000);
    }
  }

  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    const uint8_t partId = sensor.readPartID();
    Serial.printf("Alamat 0x57 merespons, tetapi Part ID terbaca 0x%02X (harus 0x15).\n",
                  partId);
    Serial.println("Mencoba ulang dengan I2C 100 kHz...");
    if (!sensor.begin(Wire, 100000)) {
      Serial.printf("Inisialisasi tetap gagal; Part ID terakhir 0x%02X.\n",
                    sensor.readPartID());
      Serial.println("Periksa jenis modul, kabel, dan kestabilan catu daya.");
      while (true) {
        delay(1000);
      }
    }
  }

  // Match the test sketch defaults: 400 Hz conversion rate with 4-sample
  // averaging yields the 100 Hz FIFO rate used by the HR interval clock.
  sensor.setup(0x1F, PPG_SAMPLE_AVERAGE, 2, SENSOR_SAMPLE_RATE_HZ, 411, 4096);
  sensor.setPulseAmplitudeRed(0x1F);
  sensor.setPulseAmplitudeIR(0x1F);
  sensor.setPulseAmplitudeGreen(0);
  Serial.println("MAX30102 siap. Letakkan jari dengan stabil selama pengukuran.");
  lastReportTime = millis();
}

void loop() {
  updateSensor();

  const uint32_t now = millis();
  if (now - lastReportTime >= REPORT_INTERVAL_MS) {
    lastReportTime = now;
    reportNodeData(now);
  }
}
