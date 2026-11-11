#include <Arduino.h>
#include <Wire.h>
#include <MAX30105.h>
#include <heartRate.h>

namespace {

constexpr int I2C_SDA = 21;
constexpr int I2C_SCL = 22;
constexpr uint32_t REPORT_INTERVAL_MS = 1000;
constexpr uint32_t FINGER_IR_THRESHOLD = 5000;
constexpr uint32_t FINGER_LOSS_TIMEOUT_MS = 750;
constexpr uint32_t MIN_BEAT_INTERVAL_MS = 300;
constexpr uint8_t BEATS_TO_AVERAGE = 10;
constexpr float MIN_VALID_BPM = 30.0f;
constexpr float MAX_VALID_BPM = 220.0f;
constexpr float MIN_PULSE_AMPLITUDE = 15.0f;
constexpr float MAX_PULSE_AMPLITUDE = 5000.0f;

MAX30105 sensor;
float bpmSamples[BEATS_TO_AVERAGE] = {};
uint8_t bpmSampleCount = 0;
uint32_t lastBeatTime = 0;
uint32_t lastFallbackBeatTime = 0;
uint32_t lastReportTime = 0;
uint32_t lastFingerSeenTime = 0;
float irBaseline = 0;
float filteredPulse = 0;
float previousFilteredPulse = 0;
float pulseMinimum = 0;
float pulseMaximum = 0;
float finalBpm = 0;
uint32_t latestIR = 0;
bool fingerDetected = false;
bool measurementComplete = false;

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

void clearHeartRate() {
  memset(bpmSamples, 0, sizeof(bpmSamples));
  bpmSampleCount = 0;
  finalBpm = 0;
  lastBeatTime = 0;
  lastFallbackBeatTime = 0;
  irBaseline = 0;
  filteredPulse = 0;
  previousFilteredPulse = 0;
  pulseMinimum = 0;
  pulseMaximum = 0;
  measurementComplete = false;
}

bool detectWeakPulse(uint32_t infrared, uint32_t now) {
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
  bool beat = false;
  if (risingZeroCrossing &&
      pulseAmplitude >= MIN_PULSE_AMPLITUDE &&
      pulseAmplitude <= MAX_PULSE_AMPLITUDE &&
      (lastFallbackBeatTime == 0 ||
       now - lastFallbackBeatTime >= MIN_BEAT_INTERVAL_MS)) {
    beat = true;
    lastFallbackBeatTime = now;
  }

  if (risingZeroCrossing) {
    pulseMinimum = filteredPulse;
    pulseMaximum = filteredPulse;
  }
  previousFilteredPulse = filteredPulse;
  return beat;
}

void readSensor() {
  const uint32_t infrared = sensor.getIR();
  const uint32_t now = millis();
  latestIR = infrared;
  const bool libraryBeat = checkForBeat(static_cast<int32_t>(infrared));

  if (infrared >= FINGER_IR_THRESHOLD) {
    if (!fingerDetected) {
      clearHeartRate();
    }
    fingerDetected = true;
    lastFingerSeenTime = now;
  } else if (fingerDetected && now - lastFingerSeenTime >= FINGER_LOSS_TIMEOUT_MS) {
    fingerDetected = false;
    clearHeartRate();
  }

  const bool weakPulseBeat =
      fingerDetected && detectWeakPulse(infrared, now);

  if (fingerDetected && !measurementComplete && (libraryBeat || weakPulseBeat)) {
    if (lastBeatTime == 0) {
      lastBeatTime = now;
    } else if (now - lastBeatTime >= MIN_BEAT_INTERVAL_MS) {
      const uint32_t interval = now - lastBeatTime;
      const float bpm = 60000.0f / interval;
      if (bpm >= MIN_VALID_BPM && bpm <= MAX_VALID_BPM) {
        bpmSamples[bpmSampleCount++] = bpm;
        if (bpmSampleCount == BEATS_TO_AVERAGE) {
          float totalBpm = 0;
          for (float sample : bpmSamples) {
            totalBpm += sample;
          }
          finalBpm = totalBpm / BEATS_TO_AVERAGE;
          measurementComplete = true;
        }
      }
      Serial.printf("Denyut terdeteksi: %.0f BPM (%u/%u)\n",
                    bpm, bpmSampleCount, BEATS_TO_AVERAGE);
      lastBeatTime = now;
    }
  }

  if (now - lastReportTime >= REPORT_INTERVAL_MS) {
    lastReportTime = now;
    Serial.printf("IR=%lu | ", static_cast<unsigned long>(latestIR));
    if (!fingerDetected) {
      Serial.println("Letakkan jari pada sensor");
    } else if (measurementComplete) {
      Serial.printf("HASIL AKHIR: rata-rata %u interval denyut = %.0f BPM\n",
                    BEATS_TO_AVERAGE, finalBpm);
    } else {
      Serial.printf("Jari terdeteksi | pengukuran %u/%u interval denyut\n",
                    bpmSampleCount, BEATS_TO_AVERAGE);
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Wire.begin(I2C_SDA, I2C_SCL);

  Serial.println("Tes MAX30102 - ESP32");
  Serial.println("Sambungkan VIN ke 3V3, GND ke GND, SDA ke GPIO21, SCL ke GPIO22.");

  if (!findSensorAddress()) {
    Serial.println("Tidak ada MAX30102 di bus I2C. Periksa sambungan dan pin.");
    Serial.println("Pastikan SDA/SCL tidak tertukar, GND tersambung, dan modul diberi 3V3.");
    while (true) {
      delay(1000);
    }
  }

  if (!sensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.printf("Alamat 0x57 merespons, tetapi Part ID terbaca 0x%02X (harus 0x15).\n",
                  sensor.readPartID());
    Serial.println("Periksa jenis modul, kualitas kabel, dan kestabilan catu daya.");
    while (true) {
      delay(1000);
    }
  }

  sensor.setup();
  sensor.setPulseAmplitudeRed(0x0A);
  sensor.setPulseAmplitudeIR(0x0A);
  sensor.setPulseAmplitudeGreen(0);
  Serial.printf("Sensor siap. Letakkan jari dengan stabil; hasil dirata-rata dari %u interval denyut.\n",
                BEATS_TO_AVERAGE);
}

void loop() {
  readSensor();
  delay(10);
}
