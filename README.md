# Sistem Bel Sekolah Otomatis
**ESP32 + WebServer + DFPlayer Mini + LCD I2C + Relay Amplifier**  
Versi 2.0 — Tanpa RTC, Tanpa Thinger.io

---

## Daftar Komponen

| Komponen | Jumlah | Keterangan |
|---|---|---|
| ESP32 DevKit v1 | 1 | Mikrokontroler utama |
| DFPlayer Mini | 1 | Modul audio MP3 |
| MicroSD Card | 1 | Maks 32GB, format FAT32 |
| LCD 16×2 I2C | 1 | Tampilan jam & info bel (alamat 0x27) |
| Relay 5V 1-channel | 1 | Kontrol power amplifier otomatis |
| Amplifier | 1 | Perkuat audio ke speaker (adaptor terpisah) |
| Speaker | 1 | Min 8Ω / 5W+ |
| Power Supply | 1 | **5V 3A (15W)** untuk ESP32, DFPlayer, LCD, Relay |
| Resistor 1kΩ | 1 | Jalur TX ESP32 → RX DFPlayer (level shift) |
| Kapasitor 100µF | 1 | Filter noise power DFPlayer |

---

## Skema Wiring

### DFPlayer Mini → ESP32
```
DFPlayer VCC  → 5V
DFPlayer GND  → GND
DFPlayer RX   → GPIO 17  (via resistor 1kΩ)
DFPlayer TX   → GPIO 16
DFPlayer SPK+ → Amplifier input +
DFPlayer SPK- → Amplifier input -
```

### LCD 16×2 I2C → ESP32
```
LCD VCC → 5V
LCD GND → GND
LCD SDA → GPIO 21
LCD SCL → GPIO 22
```

### Relay Amplifier → ESP32
```
Relay VCC → 5V
Relay GND → GND
Relay IN  → GPIO 26
Relay COM → Power amplifier (sumber listrik)
Relay NO  → Amplifier (terminal power)
```

### Estimasi Konsumsi Arus (5V 3A)
| Komponen | Arus | |
|---|---|---|
| ESP32 (WiFi aktif) | ~240 mA | |
| DFPlayer Mini | ~24 mA | saat play |
| LCD + backlight | ~25 mA | |
| Relay module | ~70 mA | saat coil aktif |
| **Total** | **~360 mA** | **Sisa ~2.6A ✓** |

> Amplifier menggunakan adaptor terpisah — hanya dikontrol ON/OFF via relay NO.

---

## Struktur File SPIFFS

Upload semua file berikut ke SPIFFS menggunakan **ESP32 Sketch Data Upload**:

```
/data
├── index.html          ← UI halaman web (HTML murni)
├── style.css           ← CSS tampilan
├── app.js              ← JavaScript logika web
├── jadwal.json         ← jadwal per hari
├── audio.json          ← daftar file MP3 + nama + durasi
└── kegiatan.json       ← daftar nama kegiatan
```

File backup dibuat otomatis oleh ESP32 — tidak perlu dibuat manual:
```
/jadwal_bak.json        ← backup otomatis setiap save jadwal
/kegiatan_bak.json      ← backup otomatis setiap save kegiatan
```

---

## Format File Data

### jadwal.json
```json
{
  "Senin": [
    {"jam": "07:00", "kegiatan": "Masuk Kelas",  "audio": 1},
    {"jam": "10:00", "kegiatan": "Istirahat",     "audio": 2},
    {"jam": "12:00", "kegiatan": "Sholat Dzuhur", "audio": 3},
    {"jam": "14:30", "kegiatan": "Pulang",         "audio": 4}
  ],
  "Selasa": [...],
  "Rabu":   [...],
  "Kamis":  [...],
  "Jumat":  [...],
  "Sabtu":  [...],
  "Minggu": [...]
}
```
> Field `audio` bisa integer (nomor track) atau string `"001.mp3"` — keduanya didukung.

### audio.json
```json
[
  {"name": "Bel Masuk",     "file": "001.mp3", "dur": 120},
  {"name": "Istirahat",     "file": "002.mp3", "dur": 240},
  {"name": "Sholat Dzuhur", "file": "003.mp3", "dur": 180},
  {"name": "Pulang",        "file": "004.mp3", "dur": 150}
]
```
> `dur` dalam **detik** — digunakan sebagai timeout fallback LCD dan progress bar web.  
> Jika `dur` tidak ada, sistem tetap berjalan menggunakan event `DFPlayerPlayFinished`.

### kegiatan.json
```json
[
  {"nama": "Masuk Kelas"},
  {"nama": "Istirahat"},
  {"nama": "Sholat Dzuhur"},
  {"nama": "Pulang"}
]
```

### SD Card (DFPlayer)
```
SD Card root /
├── 001.mp3   → Track 1
├── 002.mp3   → Track 2
├── 003.mp3   → Track 3
└── 004.mp3   → dst...
```
> Format FAT32. Nama file **wajib 3 digit**. Tidak boleh ada subfolder.

---

## Library Arduino

Install via Library Manager di Arduino IDE:

```
DFRobotDFPlayerMini    by DFRobot
LiquidCrystal_I2C      by Frank de Brabander
ArduinoJson            by Benoit Blanchon  (versi 6.x)
```

Library bawaan ESP32 (tidak perlu install):
```
WiFi / WebServer / SPIFFS / EEPROM / Wire / time.h
```

---

## Konfigurasi Awal

Edit bagian ini di `Bel_sekolah_v2.ino` sebelum upload:

```cpp
const char* ssid     = "NamaWiFi_Kamu";
const char* password = "PasswordWiFi";
#define TZ_OFFSET  (7 * 3600)      // WIB = UTC+7
#define NTP_SERVER "pool.ntp.org"
```

Jika relay module aktif LOW (kebanyakan modul 5V murah):
```cpp
#define RELAY_ACTIVE   LOW
#define RELAY_INACTIVE HIGH
```

---

## Endpoint WebServer ESP32

Akses via browser: `http://[IP_ESP32]`  
IP ditampilkan di Serial Monitor saat boot: `WiFi OK IP=192.168.x.x`

| Method | URL | Fungsi |
|---|---|---|
| GET | `/` | Halaman utama dashboard |
| GET | `/style.css` | CSS tampilan |
| GET | `/app.js` | JavaScript |
| GET | `/jadwal.json` | Baca jadwal saat ini |
| POST | `/save-jadwal` | Simpan jadwal baru |
| GET | `/audio-list` | Daftar file audio |
| GET | `/play-audio?file=001.mp3` | Putar audio + relay ON |
| GET | `/stop-audio` | Stop audio + relay cooldown |
| GET | `/volume?val=20` | Set volume (0–30) |
| GET | `/status` | Status ESP32 (JSON) |
| GET | `/kegiatan-list` | Daftar kegiatan |
| POST | `/save-kegiatan` | Simpan kegiatan |
| GET | `/backup-all` | Download backup jadwal+kegiatan |
| POST | `/restore-backup` | Restore dari file backup |
| GET | `/debug-jadwal` | Debug: isi jadwal saat ini |
| GET | `/reset-jadwal` | Kosongkan semua jadwal |

---

## Fitur Web Dashboard

### Tab Dashboard
- Jam real-time + hari + tanggal (dari NTP via browser)
- Indikator koneksi ESP32 dan status relay amplifier
- 3 metrik: **Sudah berbunyi / Sedang bunyi / Menunggu**
- Badge "Sedang bunyi" merah berkedip saat audio aktif
- Info bel berikutnya + countdown menit
- Tabel jadwal hari ini dengan status tiap baris
- Update otomatis tanpa reload halaman

### Tab Jadwal
- 7 tab hari, hari ini ditandai khusus
- Kolom keterangan: **dropdown** dari daftar kegiatan
- Kolom track: **dropdown** dari daftar audio
- Salin jadwal antar hari dengan satu klik

### Tab Kegiatan
- Kelola daftar nama kegiatan
- Menjadi sumber dropdown di tab Jadwal

### Tab Audio
- Preview putar tiap track langsung ke DFPlayer
- Progress bar + timer berdasarkan field `dur` di audio.json
- Pengaturan volume

### Tab JSON
- **Backup** jadwal + kegiatan ke file `.json` di komputer
- **Restore** dari file backup ke ESP32 kapan saja
- Pratinjau + kirim jadwal langsung

---

## Logika Relay Amplifier

```
T − 10 detik  →  Relay ON   (warmup amplifier)
T + 00 detik  →  DFPlayer mulai play
T + dur detik →  Audio selesai (event DFPlayerPlayFinished)
T + dur + 10s →  Relay OFF  (cooldown amplifier)
```

Play manual dari tab Audio → relay ON langsung, OFF 10 detik setelah stop.

---

## Tampilan LCD 16×2

### Mode Jam (LCD_CLOCK)
```
Baris 0: SEN 08:30:15          ← Hari + jam real-time
Baris 1: Istirahat 10:00       ← Kegiatan + jam bel berikutnya
```
Jika nama kegiatan > 15 karakter → run text otomatis (scroll 400ms).

### Mode Play (LCD_PLAY)
```
Baris 0: PLAY 001.mp3          ← File yang diputar
Baris 1: NamaAudio 03:45       ← Nama dari audio.json + durasi MM:SS
```
Jika nama audio > 8 karakter → run text di 8 karakter kiri (scroll 300ms).

---

## Backup & Restore Data (Penting!)

Setiap kali jadwal atau kegiatan disimpan dari web, ESP32 otomatis membuat
file backup (`jadwal_bak.json`, `kegiatan_bak.json`). File ini **tidak ada**
di folder `data/` project Arduino sehingga **tidak tertimpa** saat upload SPIFFS.

**Alur aman sebelum upload SPIFFS:**
1. Buka tab JSON → **Unduh backup** → simpan di komputer
2. Upload SPIFFS seperti biasa
3. ESP32 boot → data dipulihkan otomatis dari `*_bak.json`
4. Jika masih kosong → tab JSON → **Restore dari file** → pilih backup tadi

---

## Troubleshooting

**DFPlayer tidak bunyi:**
- Pastikan SD card format FAT32, file bernama `001.mp3`, `002.mp3`, dst
- Cek resistor 1kΩ di jalur TX ESP32 (GPIO17) → RX DFPlayer
- Tambahkan kapasitor 100µF di VCC–GND DFPlayer
- Cek di Serial Monitor: `DFPlayer OK, files=N` (N harus > 0)

**LCD tidak tampil:**
- Cek alamat I2C: default `0x27`, coba `0x3F` jika tidak muncul
- Jalankan I2C scanner sketch untuk deteksi alamat

**Jadwal tidak berbunyi:**
- Cek Serial Monitor: `WiFi OK` dan `NTP sync OK`
- Buka `http://[IP]/debug-jadwal` untuk cek isi jadwal di memori ESP32
- Pastikan field `jam` format `HH:MM` (dua digit, titik dua)

**Relay tidak menyala:**
- Cek `#define RELAY_ACTIVE` — sesuaikan HIGH/LOW dengan modul relay
- Cek di Serial Monitor: `[RELAY] Warmup: jadwal dalam N detik`

**Web tidak tampil / file 404:**
- Pastikan `style.css` dan `app.js` sudah ada di folder `/data/` dan ter-upload ke SPIFFS
- Buka `http://[IP]/style.css` langsung di browser untuk cek

**Waktu tidak sinkron:**
- Pastikan WiFi terhubung saat boot
- Cek Serial Monitor: `NTP sync OK` atau error
- Ganti NTP server ke `id.pool.ntp.org` jika bermasalah

---

## Lisensi

Bebas digunakan untuk proyek sekolah, pesantren, perkantoran, dan institusi pendidikan.
