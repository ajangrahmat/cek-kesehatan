#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <SPIFFS.h>
#include <AudioFileSourceSPIFFS.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <espnow_common.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ============================================================
// CONTROLLER — PRD v0.3
//   - Penerima ESP-NOW untuk 3 node (2 inti wajib + 1 ECG opsional)
//   - OLED SH1106 128x64, audio MAX98357A I2S, 13 MP3 di SPIFFS
//
// Controller mengklasifikasi HR + suhu memakai ambang usulan prototipe
// dewasa saat istirahat. Klasifikasi ini bukan keputusan atau diagnosis medis.
// ============================================================

// ------------------------------------------------------------
// Pendaftaran MAC node (PRD §6.2: "MAC node inti wajib terdaftar")
//
// Mode prototipe: ESPNOW_STRICT_MAC = 0 -> terima MAC apa pun, dan MAC
// tiap node dicetak ke Serial saat pertama kali mengirim. Setelah MAC
// keempat board dicatat, isi tabel di bawah lalu aktifkan filter dengan
// menambahkan -DESPNOW_STRICT_MAC=1 pada env:controller di platformio.ini.
// Entri yang masih nol diperlakukan sebagai "belum dipasangkan".
// ------------------------------------------------------------
#ifndef ESPNOW_STRICT_MAC
#define ESPNOW_STRICT_MAC 0
#endif

static uint8_t NODE_MAC_HR[6]   = {0, 0, 0, 0, 0, 0};
static uint8_t NODE_MAC_SUHU[6] = {0, 0, 0, 0, 0, 0};
static uint8_t NODE_MAC_ECG[6]  = {0, 0, 0, 0, 0, 0};

namespace {

// ---- Perangkat keras ----
constexpr int PIN_I2S_BCLK = 25;
constexpr int PIN_I2S_LRC  = 32;
constexpr int PIN_I2S_DOUT = 33;
constexpr int OLED_SDA = 21;
constexpr int OLED_SCL = 22;
constexpr uint8_t OLED_ADDRESSES[] = {0x3C, 0x3D};

// ---- Audio ----
constexpr int TRACK_COUNT = 13;
constexpr int VOLUME_MAX = 21;
constexpr float GAIN_MAX = 1.5f;
constexpr int DEFAULT_VOLUME = 18;
constexpr int TRACK_SYSTEM_READY = 1;       // A-01, chime startup
constexpr int TRACK_SYSTEM_INCOMPLETE = 13; // A-13, node inti tidak aktif
constexpr uint32_t ALERT_AUDIO_REPEAT_MS = 30000;
constexpr uint8_t ALERT_AUDIO_MAX_PLAYS = 3;

// ---- Radio / status (PRD §6.2) ----
constexpr uint32_t CORE_TIMEOUT_MS = 3000;   // 3 paket hilang -> TERPUTUS
constexpr uint32_t ECG_STANDBY_MS  = 3000;   // 3 heartbeat ECG yang terlewat
constexpr uint32_t BOOT_GRACE_MS   = 5000;   // tunggu paket pertama
constexpr uint32_t STATUS_REFRESH_MS = 500;
constexpr uint32_t STATUS_LOG_MS    = 2000;
constexpr uint32_t INCOMPLETE_AUDIO_COOLDOWN_MS = 30000;
constexpr uint8_t  PACKET_QUEUE_LENGTH = 8;

AudioGeneratorMP3 audio;
AudioFileSourceSPIFFS audioFile;
AudioOutputI2S audioOutput;
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

bool allTracksTestActive = false;
int nextTestTrack = 1;
bool oledReady = false;

QueueHandle_t packetQueue = nullptr;
uint32_t lastStatusRefresh = 0;
uint32_t lastStatusLog = 0;
uint32_t lastIncompleteAudio = 0;
bool wasIncomplete = false;
int lastAlertTrack = 0;
uint8_t alertTrackPlays = 0;
uint32_t lastAlertTrackTime = 0;

// ------------------------------------------------------------
// Status tiap node
// ------------------------------------------------------------
enum class LinkState {
  TERPUTUS,   // node inti tidak mengirim > 3 detik
  MENGUKUR,   // node hidup, q=0 (belum stabil)
  AKTIF,      // node hidup, q=1, err=0
  GANGGUAN,   // node hidup, err != 0
  STANDBY     // KHUSUS ECG: tidak aktif, BUKAN terputus (PRD §6.2/§6.3)
};

struct NodeView {
  uint8_t nodeId;
  bool core;
  bool seen;
  bool macLogged;
  uint32_t lastSeenMs;
  uint16_t lastSeq;
  uint32_t packetCount;
  uint32_t lostPackets;
  int rssi;
  NodePacket last;
  LinkState link;
};

NodeView nodes[3] = {};

enum class VitalState : uint8_t {
  MENUNGGU,
  NORMAL,
  PERHATIAN,
  GAWAT
};

struct VitalAssessment {
  bool ready = false;
  VitalState state = VitalState::MENUNGGU;
  int audioTrack = 0;
  const char *hrStatus = "MENUNGGU";
  const char *tempStatus = "MENUNGGU";
  const char *action = "Menunggu HR dan suhu valid";
};

VitalAssessment vitalAssessment;

const char *nodeShortName(int index) {
  switch (index) {
    case 0: return "HR";
    case 1: return "SUHU";
    case 2: return "ECG";
  }
  return "?";
}

const char *linkLabel(LinkState s) {
  switch (s) {
    case LinkState::TERPUTUS:  return "TERPUTUS";
    case LinkState::MENGUKUR:  return "MENGUKUR";
    case LinkState::AKTIF:     return "AKTIF";
    case LinkState::GANGGUAN:  return "GANGGUAN";
    case LinkState::STANDBY:   return "STANDBY";
  }
  return "?";
}

int nodeIndexFromId(uint8_t nodeId) {
  for (int i = 0; i < 3; ++i) {
    if (nodes[i].nodeId == nodeId) return i;
  }
  return -1;
}

// Hanya 2 node inti yang menentukan "sistem lengkap" (PRD v0.3).
bool systemComplete() {
  for (int i = 0; i < 2; ++i) {
    if (nodes[i].link == LinkState::TERPUTUS ||
        nodes[i].link == LinkState::GANGGUAN) {
      return false;
    }
  }
  return true;
}

bool nodeDataFresh(const NodeView &node, uint32_t now) {
  return node.seen && now - node.lastSeenMs <= CORE_TIMEOUT_MS;
}

VitalState classifyHeartRate(int16_t heartRate, int &audioTrack,
                             const char *&status) {
  if (heartRate < 50) {
    audioTrack = 3;
    status = "RENDAH";
    return VitalState::GAWAT;
  }
  if (heartRate < 60) {
    audioTrack = 2;
    status = "RENDAH";
    return VitalState::PERHATIAN;
  }
  if (heartRate <= 100) {
    audioTrack = 0;
    status = "NORMAL";
    return VitalState::NORMAL;
  }
  if (heartRate <= 120) {
    audioTrack = 2;
    status = "TINGGI";
    return VitalState::PERHATIAN;
  }
  audioTrack = 3;
  status = "TINGGI";
  return VitalState::GAWAT;
}

VitalState classifyTemperature(int16_t temperatureX10, int &audioTrack,
                               const char *&status) {
  if (temperatureX10 < 350) {
    audioTrack = 7;
    status = "RENDAH";
    return VitalState::GAWAT;
  }
  if (temperatureX10 < 360) {
    audioTrack = 6;
    status = "RENDAH";
    return VitalState::PERHATIAN;
  }
  if (temperatureX10 < 375) {
    audioTrack = 0;
    status = "NORMAL";
    return VitalState::NORMAL;
  }
  if (temperatureX10 < 395) {
    audioTrack = 8;
    status = "TINGGI";
    return VitalState::PERHATIAN;
  }
  audioTrack = 9;
  status = "TINGGI";
  return VitalState::GAWAT;
}

void updateVitalAssessment(uint32_t now) {
  const NodeView &hrNode = nodes[0];
  const NodeView &tempNode = nodes[1];
  const bool hrValid = nodeDataFresh(hrNode, now) &&
      hrNode.last.q == 1 && hrNode.last.err == ERR_OK &&
      hrNode.last.hr >= 30 && hrNode.last.hr <= 220;
  const bool tempValid = nodeDataFresh(tempNode, now) &&
      tempNode.last.q == 1 && tempNode.last.err == ERR_OK &&
      tempNode.last.temp_x10 >= 300 && tempNode.last.temp_x10 <= 430;

  vitalAssessment.ready = hrValid && tempValid;
  vitalAssessment.audioTrack = 0;
  if (!vitalAssessment.ready) {
    vitalAssessment.state = VitalState::MENUNGGU;
    vitalAssessment.hrStatus = hrValid ? "SIAP" : "MENUNGGU";
    vitalAssessment.tempStatus = tempValid ? "SIAP" : "MENUNGGU";
    vitalAssessment.action = "Cek sensor, ukur ulang";
    return;
  }

  int hrTrack = 0;
  int tempTrack = 0;
  const VitalState hrState =
      classifyHeartRate(hrNode.last.hr, hrTrack, vitalAssessment.hrStatus);
  const VitalState tempState = classifyTemperature(
      tempNode.last.temp_x10, tempTrack, vitalAssessment.tempStatus);

  if (hrState >= tempState) {
    vitalAssessment.state = hrState;
    vitalAssessment.audioTrack = hrTrack;
  } else {
    vitalAssessment.state = tempState;
    vitalAssessment.audioTrack = tempTrack;
  }

  switch (vitalAssessment.state) {
    case VitalState::NORMAL:
      vitalAssessment.action = "Pantau kembali berkala";
      break;
    case VitalState::PERHATIAN:
      vitalAssessment.action = "Ulangi ukur; konsultasi";
      break;
    case VitalState::GAWAT:
      vitalAssessment.action = "Cari bantuan medis segera";
      break;
    case VitalState::MENUNGGU:
      vitalAssessment.action = "Cek sensor, ukur ulang";
      break;
  }
}

const char *vitalStateLabel(VitalState state) {
  switch (state) {
    case VitalState::MENUNGGU: return "TUNGGU";
    case VitalState::NORMAL: return "NORMAL";
    case VitalState::PERHATIAN: return "PERHATIAN";
    case VitalState::GAWAT: return "GAWAT";
  }
  return "MENUNGGU DATA";
}

// ------------------------------------------------------------
// Audio (kode uji I2S/SPIFFS dipertahankan)
// ------------------------------------------------------------
bool validTrack(int soundNumber) {
  return soundNumber >= 1 && soundNumber <= TRACK_COUNT;
}

String trackPath(int soundNumber) {
  char path[16];
  snprintf(path, sizeof(path), "/%04d.mp3", soundNumber);
  return String(path);
}

void stopSound() {
  if (audio.isRunning()) {
    audio.stop();
  } else if (audioFile.isOpen()) {
    audioFile.close();
  }
}

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

// volume 0..21 -> gain 0..1.5x
void setVolume(int volume) {
  volume = constrain(volume, 0, VOLUME_MAX);
  audioOutput.SetGain(static_cast<float>(volume) / VOLUME_MAX * GAIN_MAX);
}

bool isPlaying() { return audio.isRunning(); }

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
    } else if (command == 'p' || command == 'P') {
      // Cetak MAC yang terdaftar + MAC yang terlihat, untuk pairing PRD §6.2.
      Serial.println("[PAIRING] Daftar MAC (isi ke NODE_MAC_* di controller.cpp):");
      for (int i = 0; i < 3; ++i) {
        Serial.printf("  %-4s node_id=%u seen=%s rx=%lu lost=%lu\n",
                      nodeShortName(i),
                      static_cast<unsigned>(nodes[i].nodeId),
                      nodes[i].seen ? "ya" : "tidak",
                      static_cast<unsigned long>(nodes[i].packetCount),
                      static_cast<unsigned long>(nodes[i].lostPackets));
      }
    }
  }
}

void runAllTracksTest() {
  if (!allTracksTestActive || isPlaying()) return;
  if (nextTestTrack > TRACK_COUNT) {
    allTracksTestActive = false;
    Serial.println("Uji semua track selesai");
    return;
  }
  const int track = nextTestTrack++;
  Serial.printf("Uji track %d/%d\n", track, TRACK_COUNT);
  playSound(track);
}

// ------------------------------------------------------------
// ESP-NOW: callback HANYA memasukkan ke queue
// ------------------------------------------------------------
void onEspnowPacket(const IncomingPacket &incoming) {
  if (packetQueue == nullptr) return;
  // Callback berjalan di task WiFi -> tidak ada Serial/SPIFFS/I2C di sini.
  xQueueSend(packetQueue, &incoming, 0);
}

bool macAllowed(const uint8_t mac[6], uint8_t nodeId) {
#if ESPNOW_STRICT_MAC
  const uint8_t *expected = nullptr;
  switch (nodeId) {
    case NODE_ID_HR:   expected = NODE_MAC_HR;   break;
    case NODE_ID_SUHU: expected = NODE_MAC_SUHU; break;
    case NODE_ID_ECG:  expected = NODE_MAC_ECG;  break;
    default: return false;
  }
  bool belumDiisi = true;
  for (int i = 0; i < 6; ++i) {
    if (expected[i] != 0) { belumDiisi = false; break; }
  }
  if (belumDiisi) return true;  // entri kosong = mode pairing
  return memcmp(mac, expected, 6) == 0;
#else
  (void)mac;
  (void)nodeId;
  return true;
#endif
}

void printMac(const uint8_t mac[6]) {
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                static_cast<unsigned>(mac[0]), static_cast<unsigned>(mac[1]),
                static_cast<unsigned>(mac[2]), static_cast<unsigned>(mac[3]),
                static_cast<unsigned>(mac[4]), static_cast<unsigned>(mac[5]));
}

void processPackets() {
  IncomingPacket incoming;
  while (packetQueue != nullptr &&
         xQueueReceive(packetQueue, &incoming, 0) == pdTRUE) {
    const NodePacket &p = incoming.pkt;
    const int index = nodeIndexFromId(p.node_id);
    if (index < 0) {
      Serial.printf("[ESP-NOW] node_id=%u tidak dikenal, paket diabaikan\n",
                    static_cast<unsigned>(p.node_id));
      continue;
    }
    if (!macAllowed(incoming.mac, p.node_id)) {
      Serial.printf("[ESP-NOW] MAC tidak terdaftar untuk %s, paket ditolak\n",
                    nodeShortName(index));
      continue;
    }

    NodeView &v = nodes[index];
    // Deteksi paket hilang lewat gap seq (PRD Lampiran B). seq uint16_t,
    // jadi pengurangan unsigned sudah wrap-safe.
    if (v.seen && p.seq != v.lastSeq) {
      const uint16_t expected = static_cast<uint16_t>(v.lastSeq + 1);
      if (p.seq != expected) {
        const uint16_t gap = static_cast<uint16_t>(p.seq - expected);
        if (gap < 1000) {
          v.lostPackets += gap;
        } else {
          Serial.printf("[ESP-NOW] %s: seq reset (node baru restart?)\n",
                        nodeShortName(index));
        }
      }
    }

    if (!v.macLogged) {
      Serial.printf("[PAIRING] %s (node_id=%u) MAC=",
                    nodeShortName(index), static_cast<unsigned>(p.node_id));
      printMac(incoming.mac);
      Serial.println("  <- salin ke tabel NODE_MAC_* bila filter diaktifkan");
      v.macLogged = true;
    }

    v.seen = true;
    v.lastSeenMs = millis();
    v.lastSeq = p.seq;
    v.last = p;
    v.rssi = incoming.rssi;
    ++v.packetCount;

    // JSON hanya untuk log/paper, bukan lalu lintas radio (PRD Lampiran B).
    espnowPrintPacketJson(p);
  }
}

void refreshLinkStates(uint32_t now) {
  for (int i = 0; i < 3; ++i) {
    NodeView &v = nodes[i];
    if (!v.core) {
      // ECG TIDAK PERNAH berstatus TERPUTUS (PRD §6.2).
      if (!v.seen || now - v.lastSeenMs > ECG_STANDBY_MS) {
        v.link = LinkState::STANDBY;
      } else if (v.last.err != ERR_OK) {
        v.link = LinkState::GANGGUAN;
      } else if (v.last.q == 0) {
        v.link = LinkState::MENGUKUR;
      } else {
        v.link = LinkState::AKTIF;
      }
      continue;
    }

    if (!v.seen || now - v.lastSeenMs > CORE_TIMEOUT_MS) {
      v.link = LinkState::TERPUTUS;
    } else if (v.last.err != ERR_OK) {
      v.link = LinkState::GANGGUAN;
    } else if (v.last.q == 0) {
      v.link = LinkState::MENGUKUR;
    } else {
      v.link = LinkState::AKTIF;
    }
  }
}

void updateSystemAudio(uint32_t now) {
  if (now < BOOT_GRACE_MS) return;  // beri waktu paket pertama masuk
  const bool incomplete = !systemComplete();
  if (incomplete &&
      (!wasIncomplete ||
       now - lastIncompleteAudio >= INCOMPLETE_AUDIO_COOLDOWN_MS)) {
    // Jangan tabrak uji track manual atau audio yang sedang berjalan.
    if (!allTracksTestActive && !isPlaying()) {
      Serial.println("[AUDIO] Node inti tidak aktif -> putar A-13 (0013.mp3)");
      if (playSound(TRACK_SYSTEM_INCOMPLETE)) {
        lastIncompleteAudio = now;
      }
    }
  }
  wasIncomplete = incomplete;
}

void updateVitalAudio(uint32_t now) {
  const int track = vitalAssessment.ready ? vitalAssessment.audioTrack : 0;
  if (track == 0) {
    lastAlertTrack = 0;
    alertTrackPlays = 0;
    return;
  }

  if (track != lastAlertTrack) {
    lastAlertTrack = track;
    alertTrackPlays = 0;
  }
  if (allTracksTestActive || isPlaying() ||
      alertTrackPlays >= ALERT_AUDIO_MAX_PLAYS ||
      (alertTrackPlays > 0 &&
       now - lastAlertTrackTime < ALERT_AUDIO_REPEAT_MS)) {
    return;
  }

  if (playSound(track)) {
    ++alertTrackPlays;
    lastAlertTrackTime = now;
  } else {
    alertTrackPlays = ALERT_AUDIO_MAX_PLAYS;
  }
}

// ------------------------------------------------------------
// OLED + log status
// ------------------------------------------------------------
bool initOled() {
  Wire.begin(OLED_SDA, OLED_SCL);
  uint8_t address = 0;
  for (uint8_t candidate : OLED_ADDRESSES) {
    Wire.beginTransmission(candidate);
    if (Wire.endTransmission() == 0) {
      address = candidate;
      break;
    }
  }
  if (address == 0) {
    Serial.println("OLED tidak ditemukan di 0x3C/0x3D. Status tetap lewat Serial.");
    return false;
  }
  oled.setI2CAddress(address << 1);
  oled.begin();
  Serial.printf("OLED SH1106 aktif di 0x%02X\n", static_cast<unsigned>(address));
  return true;
}

void drawFittedLine(uint8_t y, const char *text) {
  char fitted[40];
  snprintf(fitted, sizeof(fitted), "%s", text);
  while (fitted[0] != '\0' && oled.getStrWidth(fitted) > 126) {
    fitted[strlen(fitted) - 1] = '\0';
  }
  oled.drawStr(0, y, fitted);
}

void formatEcgStatus(char *out, size_t size, uint32_t now) {
  const NodeView &ecg = nodes[2];
  if (!nodeDataFresh(ecg, now)) {
    if (ecg.seen) {
      snprintf(out, size, "ECG: STANDBY / OFFLINE");
    } else {
      snprintf(out, size, "ECG: STANDBY (OPSIONAL)");
    }
  } else if (ecg.last.err == ERR_LEADS_OFF) {
    snprintf(out, size, "ECG: ONLINE, LEPAS");
  } else if (ecg.last.err != ERR_OK) {
    snprintf(out, size, "ECG: ONLINE, GANGGUAN");
  } else if (ecg.last.q == 1 && ecg.last.hr >= 30 && ecg.last.hr <= 220) {
    snprintf(out, size, "ECG: ONLINE HR=%d", static_cast<int>(ecg.last.hr));
  } else {
    snprintf(out, size, "ECG: ONLINE, MENGUKUR");
  }
}

void drawStatus() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tf);
  const uint32_t now = millis();

  char header[32];
  snprintf(header, sizeof(header), "%s | %s",
           vitalStateLabel(vitalAssessment.state),
           nodeDataFresh(nodes[2], now) ? "+ECG" : "HR+T");
  drawFittedLine(8, header);
  oled.drawHLine(0, 11, 128);

  char line[40];
  const bool hrFresh = nodeDataFresh(nodes[0], now) &&
      nodes[0].last.q == 1 && nodes[0].last.err == ERR_OK &&
      nodes[0].last.hr > 0;
  if (hrFresh) {
    snprintf(line, sizeof(line), "HR: %d bpm (%s)",
             static_cast<int>(nodes[0].last.hr), vitalAssessment.hrStatus);
  } else {
    snprintf(line, sizeof(line), "HR: -- (%s)", vitalAssessment.hrStatus);
  }
  drawFittedLine(21, line);

  const bool tempFresh = nodeDataFresh(nodes[1], now) &&
      nodes[1].last.q == 1 && nodes[1].last.err == ERR_OK &&
      nodes[1].last.temp_x10 >= 0;
  if (tempFresh) {
    const int16_t temp = nodes[1].last.temp_x10;
    snprintf(line, sizeof(line), "Suhu: %d.%d C (%s)",
             temp / 10, abs(temp % 10), vitalAssessment.tempStatus);
  } else {
    snprintf(line, sizeof(line), "Suhu: -- (%s)", vitalAssessment.tempStatus);
  }
  drawFittedLine(31, line);

  formatEcgStatus(line, sizeof(line), now);
  drawFittedLine(41, line);

  oled.drawHLine(0, 45, 128);
  drawFittedLine(55, vitalAssessment.action);
  drawFittedLine(63, "PROTOTIPE - BUKAN DIAGNOSIS");
  oled.sendBuffer();
}

void printSystemStatus(uint32_t now) {
  Serial.printf("[STATUS] %s | ", systemComplete() ? "LENGKAP" : "BELUM LENGKAP");
  for (int i = 0; i < 3; ++i) {
    Serial.printf("%s=%s(rx=%lu,lost=%lu,rssi=%d) ",
                  nodeShortName(i), linkLabel(nodes[i].link),
                  static_cast<unsigned long>(nodes[i].packetCount),
                  static_cast<unsigned long>(nodes[i].lostPackets),
                  nodes[i].rssi);
  }
  Serial.printf("| up=%lus\n", static_cast<unsigned long>(now / 1000));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  nodes[0].nodeId = NODE_ID_HR;   nodes[0].core = true;
  nodes[1].nodeId = NODE_ID_SUHU; nodes[1].core = true;
  nodes[2].nodeId = NODE_ID_ECG;  nodes[2].core = false;
  nodes[0].link = LinkState::TERPUTUS;
  nodes[1].link = LinkState::TERPUTUS;
  nodes[2].link = LinkState::STANDBY;  // ECG opsional, bukan terputus

  if (!espnowInitRadio("CONTROLLER")) {
    Serial.println("ESP-NOW gagal. Hentikan controller.");
    while (true) { delay(1000); }
  }

  packetQueue = xQueueCreate(PACKET_QUEUE_LENGTH, sizeof(IncomingPacket));
  if (packetQueue == nullptr) {
    Serial.println("Gagal membuat queue ESP-NOW");
    while (true) { delay(1000); }
  }
  if (!espnowRegisterRecv(onEspnowPacket)) {
    Serial.println("Gagal mendaftar callback penerima ESP-NOW");
    while (true) { delay(1000); }
  }
  Serial.printf("[ESP-NOW] Menunggu node inti (HR=1, Suhu=2), ECG=3 opsional. "
                "Filter MAC: %s\n", ESPNOW_STRICT_MAC ? "AKTIF" : "nonaktif (pairing)");

  if (!SPIFFS.begin(true)) {
    Serial.println("Gagal mount SPIFFS");
    while (true) { delay(1000); }
  }
  Serial.printf("SPIFFS: %u KB terpakai dari %u KB\n",
                static_cast<unsigned>(SPIFFS.usedBytes() / 1024),
                static_cast<unsigned>(SPIFFS.totalBytes() / 1024));

  if (!audioOutput.SetPinout(PIN_I2S_BCLK, PIN_I2S_LRC, PIN_I2S_DOUT)) {
    Serial.println("Gagal mengatur pin I2S");
    while (true) { delay(1000); }
  }
  setVolume(DEFAULT_VOLUME);

  oledReady = initOled();

  Serial.println("Perintah Serial: 't'=uji semua track, 's'=hentikan, 'p'=daftar MAC node.");
  Serial.println("A-01 (0001.mp3) = chime startup; konfirmasi node menyusul via ESP-NOW.");
  if (!playSound(TRACK_SYSTEM_READY)) {
    Serial.println("Isi folder data/ dengan 0001.mp3..0013.mp3 lalu jalankan uploadfs");
  }

  lastStatusRefresh = millis();
  lastStatusLog = millis();
}

void loop() {
  const uint32_t now = millis();

  handleSerialCommands();
  loopAudio();
  runAllTracksTest();
  processPackets();

  if (now - lastStatusRefresh >= STATUS_REFRESH_MS) {
    lastStatusRefresh = now;
    refreshLinkStates(now);
    updateVitalAssessment(now);
    updateVitalAudio(now);
    updateSystemAudio(now);
    if (oledReady) drawStatus();
  }
  if (now - lastStatusLog >= STATUS_LOG_MS) {
    lastStatusLog = now;
    printSystemStatus(now);
  }

  vTaskDelay(1);  // yield ke task watchdog, dicegah panic
}
