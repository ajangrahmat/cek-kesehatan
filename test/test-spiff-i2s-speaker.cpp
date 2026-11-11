#include <Arduino.h>
#include <SPIFFS.h>
#include <AudioFileSourceSPIFFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>

namespace {

constexpr int PIN_I2S_BCLK = 25;
constexpr int PIN_I2S_LRC = 32;
constexpr int PIN_I2S_DOUT = 33;

constexpr int TRACK_COUNT = 13;
constexpr int VOLUME_MAX = 21;
constexpr float GAIN_MAX = 1.5f;
constexpr int DEFAULT_VOLUME = 18;

AudioGeneratorMP3 audio;
AudioFileSourceSPIFFS audioFile;
AudioOutputI2S audioOutput;
bool allTracksTestActive = false;
int nextTestTrack = 1;

bool validTrack(int soundNumber) {
  return soundNumber >= 1 && soundNumber <= TRACK_COUNT;
}

String trackPath(int soundNumber) {
  char path[16];
  snprintf(path, sizeof(path), "/%04d.mp3", soundNumber);
  return String(path);
}

}  // namespace

void stopSound();

bool playSound(int soundNumber) {
  if (!validTrack(soundNumber)) {
    Serial.printf("Track %d di luar rentang 1..%d\n", soundNumber, TRACK_COUNT);
    return false;
  }

  String path = trackPath(soundNumber);
  if (!SPIFFS.exists(path)) {
    Serial.printf("Berkas tidak ada di SPIFFS: %s\n", path.c_str());
    return false;
  }

  stopSound();

  if (!audioFile.open(path.c_str())) {
    Serial.printf("Gagal membuka %s\n", path.c_str());
    return false;
  }

  if (!audio.begin(&audioFile, &audioOutput)) {
    audioFile.close();
    audioOutput.stop();
    Serial.printf("Gagal memutar %s\n", path.c_str());
    return false;
  }

  return true;
}

void stopSound() {
  if (audio.isRunning()) {
    audio.stop();
  } else if (audioFile.isOpen()) {
    audioFile.close();
  }
}

// volume: 0..21 maps to gain 0..1.5x
void setVolume(int volume) {
  volume = constrain(volume, 0, VOLUME_MAX);
  audioOutput.SetGain(static_cast<float>(volume) / VOLUME_MAX * GAIN_MAX);
}

bool isPlaying() {
  return audio.isRunning();
}

void loopAudio() {
  if (audio.isRunning() && !audio.loop()) {
    audio.stop();
  }
}

void handleSerialCommands() {
  while (Serial.available() > 0) {
    const char command = static_cast<char>(Serial.read());
    if (command == 't' || command == 'T') {
      stopSound();
      nextTestTrack = 1;
      allTracksTestActive = true;
      Serial.println("Mulai uji semua track 1..13");
    } else if (command == 's' || command == 'S') {
      stopSound();
      allTracksTestActive = false;
      Serial.println("Uji track dihentikan");
    }
  }
}

void runAllTracksTest() {
  if (!allTracksTestActive || isPlaying()) {
    return;
  }

  if (nextTestTrack > TRACK_COUNT) {
    allTracksTestActive = false;
    Serial.println("Uji semua track selesai");
    return;
  }

  const int track = nextTestTrack++;
  Serial.printf("Uji track %d/%d\n", track, TRACK_COUNT);
  playSound(track);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  if (!SPIFFS.begin(true)) {
    Serial.println("Gagal mount SPIFFS");
    while (true) {
      delay(1000);
    }
  }
  Serial.printf("SPIFFS: %u KB terpakai dari %u KB\n",
                static_cast<unsigned>(SPIFFS.usedBytes() / 1024),
                static_cast<unsigned>(SPIFFS.totalBytes() / 1024));

  if (!audioOutput.SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT)) {
    Serial.println("Gagal mengatur pin I2S");
    while (true) {
      delay(1000);
    }
  }
  setVolume(DEFAULT_VOLUME);

  Serial.println("Kirim 't' lewat Serial Monitor untuk menguji semua track; 's' untuk berhenti.");
  if (!playSound(1)) {
    Serial.println("Isi folder data/ dengan 0001.mp3..0013.mp3 lalu jalankan uploadfs");
  }
}

void loop() {
  handleSerialCommands();
  loopAudio();
  runAllTracksTest();
  vTaskDelay(1);  // yield ke task watchdog, dicegah panic
}
