#pragma once

// =============================================================
// espnow_common.h — lapisan transport ESP-NOW bersama
//
// Dipakai oleh 4 firmware: node_hr, node_suhu, node_ecg, controller.
// Mengacu PRD v0.3 §6.2 (parameter radio) dan Lampiran B (format data).
//
// CATATAN VERSI CORE (penting):
//   framework-arduinoespressif32 yang terpasang = IDF 5.5.5, memakai
//   callback ESP-NOW yang BARU, dua-duanya:
//     recv: void (*)(const esp_now_recv_info_t *, const uint8_t *, int)
//     send: void (*)(const esp_now_send_info_t *, esp_now_send_status_t)
//   Contoh lama Arduino-ESP32 2.x (IDF 4.4) memakai:
//     recv: void (*)(const uint8_t *mac, const uint8_t *, int)
//     send: void (*)(const uint8_t *mac, esp_now_send_status_t)
//   Guard ESP_IDF_VERSION_MAJOR di bawah menormalkan perbedaan recv,
//   sehingga file .cpp tidak perlu tahu versi core-nya.
//
//   Callback SEND sengaja tidak dipakai: untuk peer broadcast tidak ada
//   ACK per-paket, sehingga status kirim cukup dari nilai balik
//   esp_now_send(). Deteksi paket hilang tetap dilakukan controller
//   lewat gap pada field `seq` (sesuai PRD Lampiran B).
// =============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>
#include <stdint.h>
#include <string.h>

// -------------------------------------------------------------
// Identitas node — PRD Lampiran B
//   1 = HR/SpO2 (MAX30102)  [Tier 1, wajib]
//   2 = Suhu    (DS18B20)   [Tier 1, wajib]
//   3 = ECG     (AD8232)    [Tier 2, opsional]
// -------------------------------------------------------------
constexpr uint8_t NODE_ID_HR   = 1;
constexpr uint8_t NODE_ID_SUHU = 2;
constexpr uint8_t NODE_ID_ECG  = 3;

constexpr uint8_t TIER_CORE     = 1;
constexpr uint8_t TIER_LANJUTAN = 2;

constexpr uint8_t SENSOR_MAX30102 = 1;
constexpr uint8_t SENSOR_DS18B20  = 2;
constexpr uint8_t SENSOR_AD8232   = 3;

// err: kode gangguan (PRD Lampiran B)
constexpr uint8_t ERR_OK        = 0;  // tidak ada gangguan
constexpr uint8_t ERR_LEADS_OFF = 1;  // elektroda ECG lepas
constexpr uint8_t ERR_PPG_BURUK = 2;  // jari lepas / sinyal PPG buruk
constexpr uint8_t ERR_SUHU      = 3;  // sensor suhu gagal / belum stabil
constexpr uint8_t ERR_UMUM      = 9;  // gangguan umum

// Konvensi nilai "tidak tersedia"
constexpr int16_t  NILAI_TIDAK_ADA      = -1;  // hr, temp_x10
constexpr uint16_t VBAT_TIDAK_TERSEDIA = 0;    // invoice: tanpa baterai

// Kanal radio. Bisa dioverride lewat -DESPNOW_CHANNEL=n di platformio.ini.
#ifndef ESPNOW_CHANNEL
#define ESPNOW_CHANNEL 1
#endif
constexpr uint8_t kEspnowChannel = ESPNOW_CHANNEL;

// -------------------------------------------------------------
// Payload biner — PRD Lampiran B
// -------------------------------------------------------------
typedef struct __attribute__((packed)) NodePacket {
  uint8_t  node_id;      // 1=HR, 2=Suhu, 3=ECG
  uint8_t  node_tier;    // 1=inti, 2=lanjutan
  uint8_t  sensor_type;  // 1=MAX30102, 2=DS18B20, 3=AD8232
  uint16_t seq;          // naik terus, untuk deteksi paket hilang
  uint32_t t_ms;         // millis() node saat baca
  uint8_t  q;            // 1=valid & stabil, 0=tidak valid / masih mengukur
  uint8_t  err;          // lihat ERR_*
  int16_t  hr;           // bpm, -1=tidak ada
  uint8_t  spo2;         // %, 0=tidak ada
  int16_t  temp_x10;     // °C x10, -1=tidak ada
  uint16_t vbat_mv;      // mV, 0=tidak tersedia
} NodePacket;

// Jumlah field = 1+1+1+2+4+1+1+2+1+2+2 = 18 byte (packed, tanpa padding).
static_assert(sizeof(NodePacket) == 18, "NodePacket harus 18 byte (packed)");
static_assert(sizeof(NodePacket) <= ESP_NOW_MAX_DATA_LEN,
              "NodePacket melebihi batas payload ESP-NOW");

// Paket hasil terima, dibawa dari callback ke loop() lewat queue.
struct IncomingPacket {
  uint8_t   mac[6];
  int       rssi;
  NodePacket pkt;
};

// Handler hasil terima yang sudah dinormalkan (dipanggil di loop(), bukan
// langsung di callback WiFi, supaya aman untuk Serial/SPIFFS/OLED).
typedef void (*EspnowPacketHandler)(const IncomingPacket &incoming);

// -------------------------------------------------------------
// Inisialisasi radio — dipanggil oleh semua 4 board
// -------------------------------------------------------------
inline bool espnowInitRadio(const char *roleLabel, bool log = true,
                            esp_err_t *powerSaveStatus = nullptr) {
  // ESP-NOW butuh driver WiFi aktif, tapi TIDAK butuh AP/router dan tidak
  // perlu WiFi.begin(). Mode STA sudah cukup.
  WiFi.mode(WIFI_STA);

  // Power-save bisa membuat paket ESP-NOW terlewat; matikan.
  const esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
  if (powerSaveStatus != nullptr) {
    *powerSaveStatus = ps;
  }
  if (log && ps != ESP_OK) {
    Serial.printf("[ESP-NOW] Peringatan: gagal matikan power-save (%d)\n", ps);
  }

  // Semua perangkat WAJIB di kanal yang sama (PRD §6.2).
  if (esp_wifi_set_channel(kEspnowChannel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
    if (log) Serial.println("[ESP-NOW] Gagal mengatur kanal WiFi");
    return false;
  }

  if (esp_now_init() != ESP_OK) {
    if (log) Serial.println("[ESP-NOW] esp_now_init() gagal");
    return false;
  }

  if (log) {
    Serial.printf("[ESP-NOW] %s | MAC STA: %s | kanal: %u | payload: %u byte\n",
                  roleLabel, WiFi.macAddress().c_str(),
                  static_cast<unsigned>(kEspnowChannel),
                  static_cast<unsigned>(sizeof(NodePacket)));
  }
  return true;
}

// -------------------------------------------------------------
// Sisi NODE: daftar peer broadcast lalu kirim
//
// Broadcast dipilih agar 4 board bisa langsung dipasangkan tanpa perlu
// mengetahui MAC controller lebih dulu. Controller mencetak MAC tiap
// node ke Serial; daftar MAC tersebut bisa diaktifkan kemudian sebagai
// filter ketat (PRD §6.2 "MAC node inti wajib terdaftar").
// -------------------------------------------------------------
inline const uint8_t *espnowBroadcastMac() {
  static const uint8_t mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return mac;
}

inline bool espnowNodeBegin(bool log = true) {
  if (!esp_now_is_peer_exist(espnowBroadcastMac())) {
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, espnowBroadcastMac(), 6);
    peer.channel = kEspnowChannel;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
      if (log) Serial.println("[ESP-NOW] Gagal menambah peer broadcast");
      return false;
    }
  }
  if (log) Serial.println("[ESP-NOW] Peer broadcast siap; node mulai mengirim");
  return true;
}

inline bool espnowSendPacket(const NodePacket &pkt) {
  const esp_err_t result = esp_now_send(
      espnowBroadcastMac(),
      reinterpret_cast<const uint8_t *>(&pkt),
      sizeof(pkt));
  return result == ESP_OK;
}

inline NodePacket espnowMakePacket(uint8_t nodeId, uint8_t tier,
                                   uint8_t sensorType, uint16_t seq,
                                   uint32_t nowMs) {
  NodePacket p = {};
  p.node_id     = nodeId;
  p.node_tier   = tier;
  p.sensor_type = sensorType;
  p.seq         = seq;
  p.t_ms        = nowMs;
  p.q           = 0;
  p.err         = ERR_OK;
  p.hr          = NILAI_TIDAK_ADA;
  p.spo2        = 0;
  p.temp_x10    = NILAI_TIDAK_ADA;
  p.vbat_mv     = VBAT_TIDAK_TERSEDIA;
  return p;
}

// JSON dipakai HANYA untuk log/paper, bukan lalu lintas ESP-NOW (PRD Lampiran B).
inline void espnowPrintPacketJson(const NodePacket &p) {
  Serial.printf(
      "{\"n\":%u,\"tier\":%u,\"type\":%u,\"seq\":%u,\"t\":%lu,"
      "\"q\":%u,\"err\":%u,\"hr\":%d,\"spo2\":%u,\"temp\":%d,\"vbat\":%u}\n",
      static_cast<unsigned>(p.node_id),
      static_cast<unsigned>(p.node_tier),
      static_cast<unsigned>(p.sensor_type),
      static_cast<unsigned>(p.seq),
      static_cast<unsigned long>(p.t_ms),
      static_cast<unsigned>(p.q),
      static_cast<unsigned>(p.err),
      static_cast<int>(p.hr),
      static_cast<unsigned>(p.spo2),
      static_cast<int>(p.temp_x10),
      static_cast<unsigned>(p.vbat_mv));
}

// -------------------------------------------------------------
// Sisi CONTROLLER: normalisasi callback recv lintas versi IDF
// -------------------------------------------------------------
inline EspnowPacketHandler &espnowHandlerSlot() {
  static EspnowPacketHandler handler = nullptr;  // satu instance lintas TU
  return handler;
}

inline void espnowDispatch(const uint8_t *mac, const uint8_t *data,
                           int len, int rssi) {
  if (data == nullptr || len != static_cast<int>(sizeof(NodePacket))) {
    return;
  }
  IncomingPacket incoming = {};
  if (mac != nullptr) {
    memcpy(incoming.mac, mac, 6);
  }
  incoming.rssi = rssi;
  memcpy(&incoming.pkt, data, sizeof(NodePacket));

  // Callback WiFi berjalan di task WiFi. Handler yang didaftarkan lewat
  // espnowRegisterRecv() WAJIB hanya memasukkan paket ke queue, lalu
  // loop() yang memproses (Serial, SPIFFS, OLED tidak aman di sini).
  EspnowPacketHandler handler = espnowHandlerSlot();
  if (handler != nullptr) {
    handler(incoming);
  }
}

#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
inline void espnowRawRecvCb(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len) {
  int rssi = 0;
  if (info != nullptr && info->rx_ctrl != nullptr) {
    rssi = info->rx_ctrl->rssi;
  }
  espnowDispatch(info != nullptr ? info->src_addr : nullptr, data, len, rssi);
}
#else
inline void espnowRawRecvCb(const uint8_t *mac, const uint8_t *data, int len) {
  espnowDispatch(mac, data, len, 0);
}
#endif

inline bool espnowRegisterRecv(EspnowPacketHandler handler) {
  espnowHandlerSlot() = handler;
  return esp_now_register_recv_cb(espnowRawRecvCb) == ESP_OK;
}
