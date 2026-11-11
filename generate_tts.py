#!/usr/bin/env python3
"""
Generate TTS untuk Sistem Monitoring Parameter Fisiologis ESP32
Output: data/0001.mp3 .. data/0013.mp3 (untuk SPIFFS upload via MAX98357A I2S)

Menggunakan edge-tts (Microsoft Edge Neural TTS) untuk suara yang lebih natural.

Teks audio disesuaikan dengan PRD v0.3:
  - Arsitektur modular: Tier 1 (HR + Suhu) wajib, Tier 2 (ECG) opsional
  - ECG ada status STANDBY (bukan "terputus" saat tidak aktif)
  - Logika stabilisasi: sistem tahu "mengukur" vs "siap"
  - Komunikasi via ESP-NOW, audio via MAX98357A
  - Teks first aid mengacu FIX BIESF.docx (sebut nomor darurat 119/112)
  - Maks 10 detik per instruksi, langsung ke inti

Suara:
  - id-ID-GadisNeural (female) untuk info, perhatian, dan gangguan teknis
  - id-ID-ArdiNeural (male) untuk peringatan gawat (prioritas 4)

Track mapping (PRD v0.3 - Lampiran C):
  0001 - Sistem siap                          (info, Gadis)
  0002 - Perhatian detak jantung              (perhatian, Gadis)
  0003 - Peringatan detak jantung gawat       (gawat, Ardi)
  0004 - Perhatian oksigen rendah             (perhatian, Gadis)
  0005 - Peringatan oksigen gawat             (gawat, Ardi)
  0006 - Perhatian suhu rendah                (perhatian, Gadis)
  0007 - Peringatan suhu sangat rendah        (gawat, Ardi)
  0008 - Perhatian suhu meningkat             (perhatian, Gadis)
  0009 - Peringatan suhu tinggi               (gawat, Ardi)
  0010 - Elektroda terlepas                   (teknis, Gadis)
  0011 - HR tidak konsisten                   (teknis, Gadis)
  0012 - Node tidak terhubung                 (teknis, Gadis)
  0013 - Sistem tidak lengkap                 (teknis, Gadis)
"""

import asyncio
import edge_tts
from pathlib import Path
import sys

OUTPUT_DIR = Path(__file__).parent / "data"

# Suara berdasarkan jenis pesan
VOICE_INFO = "id-ID-GadisNeural"    # female, tenang
VOICE_WARN = "id-ID-GadisNeural"    # female, perhatian
VOICE_ALARM = "id-ID-ArdiNeural"    # male, tegas untuk gawat
VOICE_TECH = "id-ID-GadisNeural"    # female, teknis

# Rate dan pitch per jenis pesan
RATE_NORMAL = "+0%"
RATE_URGENT = "+15%"
PITCH_NORMAL = "+0Hz"
PITCH_URGENT = "+10Hz"

TRACKS = [
    # --- A-01: Sistem siap (info, prioritas 1) ---
    # Saat boot selesai, semua node inti terhubung dan stabil
    {
        "file": "0001.mp3",
        "text": "Sistem siap. Sensor detak jantung dan suhu aktif. Pemantauan dimulai.",
        "voice": VOICE_INFO,
        "rate": RATE_NORMAL,
        "pitch": PITCH_NORMAL,
    },
    # --- A-02: HR perhatian (prioritas 2) ---
    # HR di luar 60-100 tapi belum gawat (50-59 bradikardia, 101-120 takikardia)
    {
        "file": "0002.mp3",
        "text": "Perhatian. Detak jantung di luar batas normal. Duduklah dan tenang.",
        "voice": VOICE_WARN,
        "rate": RATE_NORMAL,
        "pitch": PITCH_NORMAL,
    },
    # --- A-03: HR gawat (prioritas 4) ---
    # HR <50 atau >120 bertahan >=3 detik. Sebut nomor darurat (FIX BIESF)
    {
        "file": "0003.mp3",
        "text": "Peringatan. Detak jantung sangat tidak normal. Segera cari bantuan. Hubungi satu satu sembilan atau satu satu dua.",
        "voice": VOICE_ALARM,
        "rate": RATE_URGENT,
        "pitch": PITCH_URGENT,
    },
    # --- A-04: SpO2 perhatian (prioritas 2) ---
    # SpO2 91-94% hipoksia ringan
    {
        "file": "0004.mp3",
        "text": "Perhatian. Kadar oksigen sedikit rendah. Duduk tegak dan tarik napas dalam.",
        "voice": VOICE_WARN,
        "rate": RATE_NORMAL,
        "pitch": PITCH_NORMAL,
    },
    # --- A-05: SpO2 gawat (prioritas 4) ---
    # SpO2 <=90% hipoksia sedang/berat
    {
        "file": "0005.mp3",
        "text": "Peringatan. Kadar oksigen rendah. Segera cari bantuan. Hubungi satu satu sembilan atau satu satu dua.",
        "voice": VOICE_ALARM,
        "rate": RATE_URGENT,
        "pitch": PITCH_URGENT,
    },
    # --- A-06: Suhu perhatian rendah (prioritas 2) ---
    # Suhu 35,0-35,9 hipotermia ringan
    {
        "file": "0006.mp3",
        "text": "Perhatian. Suhu tubuh rendah. Selimuti dan hindari udara dingin.",
        "voice": VOICE_WARN,
        "rate": RATE_NORMAL,
        "pitch": PITCH_NORMAL,
    },
    # --- A-07: Suhu gawat rendah (prioritas 4) ---
    # Suhu <35 hipotermia berat
    {
        "file": "0007.mp3",
        "text": "Peringatan. Suhu tubuh sangat rendah. Segera cari bantuan. Hubungi satu satu sembilan atau satu satu dua.",
        "voice": VOICE_ALARM,
        "rate": RATE_URGENT,
        "pitch": PITCH_URGENT,
    },
    # --- A-08: Suhu perhatian tinggi (prioritas 2) ---
    # Suhu 37,5-39,4 demam
    {
        "file": "0008.mp3",
        "text": "Perhatian. Suhu tubuh meningkat. Minum air dan kompres dengan air dingin.",
        "voice": VOICE_WARN,
        "rate": RATE_NORMAL,
        "pitch": PITCH_NORMAL,
    },
    # --- A-09: Suhu gawat tinggi (prioritas 4) ---
    # Suhu >=39,5 demam tinggi
    {
        "file": "0009.mp3",
        "text": "Peringatan. Suhu tubuh sangat tinggi. Segera cari bantuan. Hubungi satu satu sembilan atau satu satu dua.",
        "voice": VOICE_ALARM,
        "rate": RATE_URGENT,
        "pitch": PITCH_URGENT,
    },
    # --- A-10: Elektroda lepas (prioritas 3) ---
    # Hanya muncul saat ECG aktif dan LO+/LO- HIGH
    {
        "file": "0010.mp3",
        "text": "Elektroda terlepas. Periksa pemasangan elektroda pada dada.",
        "voice": VOICE_TECH,
        "rate": RATE_NORMAL,
        "pitch": PITCH_NORMAL,
    },
    # --- A-11: HR tidak konsisten (prioritas 3) ---
    # Selisih HR ECG vs PPG >15 bpm selama 5 detik (saat ECG aktif)
    {
        "file": "0011.mp3",
        "text": "Pembacaan detak jantung tidak konsisten. Periksa sensor dan elektroda.",
        "voice": VOICE_TECH,
        "rate": RATE_NORMAL,
        "pitch": PITCH_NORMAL,
    },
    # --- A-12: Node terputus (prioritas 3) ---
    # ESP-NOW paket hilang >3 detik untuk node inti
    {
        "file": "0012.mp3",
        "text": "Sensor tidak terhubung. Periksa daya dan posisi node.",
        "voice": VOICE_TECH,
        "rate": RATE_NORMAL,
        "pitch": PITCH_NORMAL,
    },
    # --- A-13: Sistem tidak lengkap (prioritas 3) ---
    # Hanya 1 dari 2 node inti aktif >10 detik (bukan ECG — ECG standby bukan "tidak lengkap")
    {
        "file": "0013.mp3",
        "text": "Sistem tidak lengkap. Salah satu sensor tidak aktif. Periksa semua node sensor.",
        "voice": VOICE_TECH,
        "rate": RATE_NORMAL,
        "pitch": PITCH_NORMAL,
    },
]


async def generate_one(track):
    """Generate satu track MP3 via edge-tts."""
    out_path = OUTPUT_DIR / track["file"]
    voice = track["voice"]
    rate = track.get("rate", RATE_NORMAL)
    pitch = track.get("pitch", PITCH_NORMAL)

    communicate = edge_tts.Communicate(
        text=track["text"],
        voice=voice,
        rate=rate,
        pitch=pitch,
    )
    await communicate.save(str(out_path))
    return out_path.stat().st_size


async def generate_all():
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    ok = 0
    fail = 0

    print("Generate TTS via edge-tts (Microsoft Neural TTS)")
    print(f"Suara info/perhatian/teknis: {VOICE_INFO}")
    print(f"Suara gawat:                 {VOICE_ALARM}")
    print(f"Output: {OUTPUT_DIR}\n")

    for track in TRACKS:
        voice_name = "Ardi" if "Ardi" in track["voice"] else "Gadis"
        print(f"  {track['file']:10s}  [{voice_name:5s}]  {track['text']}")
        try:
            size = await generate_one(track)
            print(f"              -> OK ({size:,} bytes)")
            ok += 1
        except Exception as e:
            print(f"              -> FAIL: {e}")
            fail += 1

    total = sum((OUTPUT_DIR / t["file"]).stat().st_size for t in TRACKS if (OUTPUT_DIR / t["file"]).exists())
    print(f"\nSelesai: {ok} berhasil, {fail} gagal.")
    print(f"Total ukuran: {total:,} bytes ({total/1024:.0f} KB)")
    print(f"Output: {OUTPUT_DIR}")
    return fail == 0


if __name__ == "__main__":
    success = asyncio.run(generate_all())
    sys.exit(0 if success else 1)
