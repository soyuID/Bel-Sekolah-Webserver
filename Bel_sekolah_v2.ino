#include <WiFi.h>
#include <vector>
#include <WebServer.h>
#include <SPIFFS.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <DFRobotDFPlayerMini.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <time.h>

/* ============================================================
   jadwal.json : {
     "Senin": [ {"jam":"07:00","kegiatan":"Masuk","audio":"001.mp3"}, ... ],
     ...
   }
   audio bisa string "001.mp3" ATAU integer 1 — keduanya didukung

   audio.json : [ {"name":"Bel Masuk","file":"001.mp3"}, ... ]
   ============================================================ */

#define EEPROM_SIZE  512
#define EEPROM_MAGIC 0xA5

const char* ssid     = "AL - HIDAYAH";
const char * password = "alhidayah2026";//bismilah2026
#define TZ_OFFSET  (7 * 3600)
#define NTP_SERVER "pool.ntp.org"

LiquidCrystal_I2C lcd(0x27, 16, 2);

HardwareSerial dfSerial(2);
DFRobotDFPlayerMini dfplayer;
bool dfReady     = false;
int  volumeLevel = 20;

WebServer server(80);

// Dokumen JSON — DynamicJsonDocument agar tidak overflow
DynamicJsonDocument jadwalDoc(8192);
DynamicJsonDocument audioDoc(4096);

bool ntpSynced  = false;
int  lastMinute = -99;

bool          bellPlaying = false;
unsigned long bellStartMs = 0;
#define BELL_COOLDOWN_MS 65000UL

enum LcdMode { LCD_CLOCK, LCD_PLAY };
LcdMode lcdMode = LCD_CLOCK;

String nextKeg = "";
String nextJam = "";

String        rtStr  = "";
int           rtPos  = 0;
unsigned long rtLast = 0;
#define RT_MS 400

unsigned long dfPollLast = 0;
#define DF_POLL_MS 500

/* --- Relay Amplifier ---
   Relay aktif HIGH (HIGH = ON). Ganti RELAY_ACTIVE menjadi LOW
   jika modul relay kamu aktif LOW.
   Pin default: GPIO 26                                          */
#define RELAY_PIN         26//23
#define RELAY_ACTIVE      HIGH       // HIGH = relay ON
#define RELAY_INACTIVE    LOW        // LOW  = relay OFF
#define AMPLI_WARMUP_S    10         // detik sebelum jadwal → relay ON
#define AMPLI_COOLDOWN_MS 10000UL   // 10 detik setelah audio selesai → relay OFF

bool          ampliOn       = false;  // status relay saat ini
unsigned long ampliOffAt    = 0;      // waktu relay harus dimatikan (millis), 0=tidak dijadwalkan

/* --- Play state --- */
#define PLAY_TIMEOUT_MS  600000UL   // 10 menit fallback kembali ke jam (jika dur tidak ada)

unsigned long playStartMs    = 0;   // waktu mulai play (ms)
unsigned long playDurMs      = 0;   // durasi track dari audio.json (0 = tidak diketahui)
int           playTrackNum   = 0;   // nomor track sedang diputar
String        playFileName   = "";  // nama file sedang diputar
unsigned long lcdPlayUpdLast = 0;   // kapan terakhir update baris 1 LCD

/* ============================================================
   EEPROM
   ============================================================ */
void saveEEPROM(){
  EEPROM.write(0, EEPROM_MAGIC);
  EEPROM.put(1, volumeLevel);
  EEPROM.commit();
}
void loadEEPROM(){
  if(EEPROM.read(0) == EEPROM_MAGIC) EEPROM.get(1, volumeLevel);
}

/* ============================================================
   RELAY AMPLIFIER
   ============================================================ */
void relayOn(){
  if(ampliOn) return;
  ampliOn   = true;
  ampliOffAt = 0;                         // batalkan timer OFF jika ada
  digitalWrite(RELAY_PIN, RELAY_ACTIVE);
  Serial.println("[RELAY] ON - Amplifier hidup");
}

void relayOff(){
  if(!ampliOn) return;
  ampliOn    = false;
  ampliOffAt = 0;
  digitalWrite(RELAY_PIN, RELAY_INACTIVE);
  Serial.println("[RELAY] OFF - Amplifier mati");
}

// Jadwalkan relay OFF setelah AMPLI_COOLDOWN_MS
void relayScheduleOff(){
  ampliOffAt = millis() + AMPLI_COOLDOWN_MS;
  Serial.printf("[RELAY] OFF dijadwalkan %lu ms lagi\n", AMPLI_COOLDOWN_MS);
}

// Dipanggil tiap loop — cek apakah sudah waktunya mematikan relay
void cekAmpli(){
  if(ampliOn && ampliOffAt > 0 && millis() >= ampliOffAt){
    relayOff();
  }
}

/* ============================================================
   HELPER: ekstrak nomor track dari field "audio"
   Mendukung:
     - integer  : 1        -> 1
     - string   : "001.mp3" -> 1
     - string   : "1"       -> 1
   ============================================================ */
int audioToTrack(JsonVariant v){
  if(v.is<int>())    return v.as<int>();
  if(v.is<float>())  return (int)v.as<float>();
  // string
  String s = v.as<String>();
  s.trim();
  // Ambil angka di awal string: "001.mp3" -> 1, "3" -> 3
  return s.toInt();   // toInt() berhenti di karakter non-digit
}

/* ============================================================
   HELPER: "HH:MM" -> menit integer
   ============================================================ */
int toMenit(const char* s){
  if(!s || strlen(s) < 5) return -1;
  return (s[0]-'0')*600 + (s[1]-'0')*60 + (s[3]-'0')*10 + (s[4]-'0');
}

/* ============================================================
   NAMA HARI
   tm_wday: 0=Minggu,1=Senin,2=Selasa,3=Rabu,4=Kamis,5=Jumat,6=Sabtu
   ============================================================ */
const char* DAY_KEY[]   = {"Minggu","Senin","Selasa","Rabu","Kamis","Jumat","Sabtu"};
const char* DAY_SHORT[] = {"Min","Sen","Sel","Rab","Kam","Jum","Sab"};

/* ============================================================
   LOAD JSON
   ============================================================ */
// Key hari yang valid
const char* VALID_KEYS[] = {"Senin","Selasa","Rabu","Kamis","Jumat","Sabtu","Minggu"};
const int   VALID_KEYS_N = 7;

bool isValidKey(const char* k){
  for(int i=0;i<VALID_KEYS_N;i++)
    if(strcmp(k, VALID_KEYS[i])==0) return true;
  return false;
}

// Bersihkan key asing dari jadwalDoc, simpan ulang ke SPIFFS
void cleanAndSaveJadwal(){
  // Kumpulkan key yang tidak valid
  std::vector<String> toRemove;
  for(JsonPair p : jadwalDoc.as<JsonObject>())
    if(!isValidKey(p.key().c_str())) toRemove.push_back(p.key().c_str());

  if(toRemove.size()>0){
    Serial.print("Hapus key asing: ");
    for(auto& k : toRemove){
      Serial.print("["+k+"] ");
      jadwalDoc.remove(k.c_str());
    }
    Serial.println();
    // Simpan ulang ke SPIFFS
    File f = SPIFFS.open("/jadwal.json","w");
    serializeJson(jadwalDoc, f);
    f.close();
    Serial.println("jadwal.json dibersihkan & disimpan ulang");
  }
}

/* ============================================================
   Hitung total entri di jadwalDoc (semua hari)
   ============================================================ */
int countJadwalEntries(){
  int total = 0;
  for(JsonPair p : jadwalDoc.as<JsonObject>())
    total += (int)p.value().as<JsonArray>().size();
  return total;
}

/* ============================================================
   Simpan backup jadwal ke /jadwal_backup.json
   Dipanggil setiap kali jadwal berhasil disimpan dari web
   ============================================================ */
void backupJadwal(){
  File f = SPIFFS.open("/jadwal_bak.json","w");
  if(!f){ Serial.println("[backup] ERR buka file"); return; }
  serializeJson(jadwalDoc, f);
  f.close();
  Serial.printf("[backup] jadwal_bak.json disimpan (%d entri)\n", countJadwalEntries());
}

/* ============================================================
   Simpan backup kegiatan ke /kegiatan_bak.json
   ============================================================ */
void backupKegiatan(){
  if(!SPIFFS.exists("/kegiatan.json")) return;
  File src = SPIFFS.open("/kegiatan.json","r");
  if(!src) return;
  String content = src.readString(); src.close();
  if(content.length() < 3 || content == "[]") return; // jangan timpa backup dengan kosong
  File dst = SPIFFS.open("/kegiatan_bak.json","w");
  if(!dst) return;
  dst.print(content); dst.close();
  Serial.println("[backup] kegiatan_bak.json disimpan");
}

void loadJadwal(){
  jadwalDoc.clear();
  const char* defaultJson =
    "{\"Senin\":[],\"Selasa\":[],\"Rabu\":[],\"Kamis\":[],\"Jumat\":[],\"Sabtu\":[],\"Minggu\":[]}";

  // --- Baca jadwal.json utama ---
  bool mainOk = false;
  if(SPIFFS.exists("/jadwal.json")){
    File f = SPIFFS.open("/jadwal.json","r");
    if(f){
      DeserializationError e = deserializeJson(jadwalDoc, f);
      f.close();
      if(!e && countJadwalEntries() > 0){
        mainOk = true;
        Serial.println("=== jadwal.json LOADED ===");
      } else {
        Serial.println("[jadwal] file utama kosong atau error, coba backup...");
        jadwalDoc.clear();
      }
    }
  }

  // --- Fallback ke backup jika utama kosong/error ---
  if(!mainOk && SPIFFS.exists("/jadwal_bak.json")){
    File fb = SPIFFS.open("/jadwal_bak.json","r");
    if(fb){
      DeserializationError e = deserializeJson(jadwalDoc, fb);
      fb.close();
      if(!e && countJadwalEntries() > 0){
        mainOk = true;
        Serial.println("=== jadwal DIPULIHKAN dari backup! ===");
        // Tulis balik ke jadwal.json utama
        File fw = SPIFFS.open("/jadwal.json","w");
        serializeJson(jadwalDoc, fw); fw.close();
        Serial.println("[restore] jadwal.json diperbarui dari backup");
      } else {
        jadwalDoc.clear();
      }
    }
  }

  // --- Jika masih kosong, pakai default ---
  if(!mainOk){
    Serial.println("[jadwal] tidak ada data valid, pakai default kosong");
    deserializeJson(jadwalDoc, defaultJson);
    File fw = SPIFFS.open("/jadwal.json","w");
    fw.print(defaultJson); fw.close();
  }

  // Bersihkan key asing
  cleanAndSaveJadwal();

  // Debug
  Serial.print("Keys: ");
  for(JsonPair p : jadwalDoc.as<JsonObject>())
    Serial.printf("[%s]=%d ", p.key().c_str(), (int)p.value().as<JsonArray>().size());
  Serial.println();
}

void loadAudio(){
  audioDoc.clear();
  if(!SPIFFS.exists("/audio.json")){
    File f = SPIFFS.open("/audio.json","w"); f.print("[]"); f.close();
  }
  File f = SPIFFS.open("/audio.json","r");
  if(!f){ Serial.println("ERR buka audio.json"); return; }
  DeserializationError e = deserializeJson(audioDoc, f);
  f.close();
  if(e){ Serial.print("ERR parse audio: "); Serial.println(e.c_str()); }
  else  Serial.println("audio.json loaded OK");
}

void loadKegiatan(){
  // Jika file utama tidak ada atau kosong, coba restore dari backup
  bool needRestore = false;
  if(!SPIFFS.exists("/kegiatan.json")){
    needRestore = true;
  } else {
    File f = SPIFFS.open("/kegiatan.json","r");
    if(f){
      String content = f.readString(); f.close();
      if(content.length() < 3 || content == "[]") needRestore = true;
    } else needRestore = true;
  }

  if(needRestore && SPIFFS.exists("/kegiatan_bak.json")){
    File fb = SPIFFS.open("/kegiatan_bak.json","r");
    if(fb){
      String bak = fb.readString(); fb.close();
      if(bak.length() >= 3 && bak != "[]"){
        File fw = SPIFFS.open("/kegiatan.json","w");
        fw.print(bak); fw.close();
        Serial.println("[restore] kegiatan.json dipulihkan dari backup");
        return;
      }
    }
  }

  // Buat file kosong jika belum ada
  if(!SPIFFS.exists("/kegiatan.json")){
    File f = SPIFFS.open("/kegiatan.json","w"); f.print("[]"); f.close();
  }
}

/* ============================================================
   DFPLAYER
   ============================================================ */
void initDFPlayer(){
  dfSerial.begin(9600, SERIAL_8N1, 16, 17);
  delay(2000);
  if(!dfplayer.begin(dfSerial)){
    Serial.println("DFPlayer GAGAL");
    lcd.clear(); lcd.print("DFPlayer GAGAL");
    return;
  }
  dfplayer.reset(); delay(2000);
  dfplayer.volume(volumeLevel);
  dfReady = true;
  Serial.print("DFPlayer OK, files=");
  Serial.println(dfplayer.readFileCounts());
}

/* ============================================================
   RUNTEXT
   ============================================================ */
void rtInit(String teks){
  if(teks.length() > 8){
    rtStr = teks + "        ";
    rtPos = 0; rtLast = millis();
  } else { rtStr = ""; rtPos = 0; }
}

String rtSlice(){
  if(rtStr.length() == 0) return "";
  if(millis() - rtLast >= RT_MS){ rtLast = millis(); rtPos = (rtPos+1) % rtStr.length(); }
  String s = "";
  for(int i=0;i<8;i++) s += rtStr[(rtPos+i) % rtStr.length()];
  return s;
}

/* ============================================================
   LCD PLAY
   Baris 0: "PLAY 001.mp3"
   Baris 1: nama kegiatan
   ============================================================ */
/*
  Tampilan saat PLAY:
  Baris 0: "PLAY 001.mp3    "  (tetap, tidak berubah)
  Baris 1: "00:05  001.mp3  "  (elapsed MM:SS + nama file, update tiap detik)
*/
/*
  Tampilan saat PLAY:
  Baris 0: "PLAY 001.mp3    "  (tetap, tidak berubah)
  Baris 1: "NamaAudi 03:45  "  (name dari audioDoc + dur MM:SS)
           Jika name > 8 char → run text di 8 char kiri, dur 5 char kanan
*/

// Variabel untuk run text baris 1 saat PLAY
String  playName    = "";   // nama dari audioDoc
String  playDurStr  = "";   // durasi format "MM:SS"
String  playRtStr   = "";   // string run text (name + padding)
int     playRtPos   = 0;
unsigned long playRtLast = 0;
#define PLAY_RT_MS  300     // kecepatan scroll ms

void lcdShowPlay(String file, String keg){
  lcdMode      = LCD_PLAY;
  playFileName = file;

  // Ambil name dan dur dari audioDoc
  playName   = keg;   // fallback = nama kegiatan
  playDurStr = "     ";
  for(JsonObject o : audioDoc.as<JsonArray>()){
    if(o["file"].as<String>() == file){
      if(o.containsKey("name")) playName = o["name"].as<String>();
      if(o.containsKey("dur")){
        int total = o["dur"].as<int>();
        char buf[6]; snprintf(buf,sizeof(buf),"%02d:%02d", total/60, total%60);
        playDurStr = String(buf);
      }
      break;
    }
  }

  // Siapkan run text jika name > 8 karakter
  if(playName.length() > 8){
    playRtStr = playName + "        ";  // 8 spasi padding
    playRtPos = 0;
    playRtLast = millis();
  } else {
    playRtStr = "";
  }

  lcd.clear();

  // Baris 0: "PLAY 001.mp3    "
  String b0 = "PLAY " + file;
  b0 = (b0.length()>16) ? b0.substring(0,16) : b0;
  lcd.setCursor(0,0); lcd.print(b0);
  for(int i=b0.length();i<16;i++) lcd.print(' ');

  // Baris 1 awal
  updateLcdPlayLine();
  lcdPlayUpdLast = millis();
}

/* Update baris 1 saat PLAY tiap 300ms (run text) atau 1 detik (dur statis)
   Layout: [8 char name/runtext][spasi][5 char MM:SS][2 spasi]
   Contoh: "IstirahaT 03:45  " atau "Bel In   01:30  "          */
void updateLcdPlayLine(){
  if(lcdMode != LCD_PLAY) return;

  unsigned long now = millis();
  bool doUpdate = false;

  if(playRtStr.length() > 0){
    // Run text aktif — update tiap PLAY_RT_MS
    if(now - playRtLast >= PLAY_RT_MS){
      playRtLast = now;
      playRtPos  = (playRtPos + 1) % playRtStr.length();
      doUpdate   = true;
    }
  } else {
    // Statis — update tiap 1 detik
    if(now - lcdPlayUpdLast >= 1000){
      lcdPlayUpdLast = now;
      doUpdate = true;
    }
  }

  if(!doUpdate) return;

  // Susun 8 char bagian kiri (name / run text)
  String left;
  if(playRtStr.length() > 0){
    left = "";
    for(int i=0;i<8;i++) left += playRtStr[(playRtPos+i) % playRtStr.length()];
  } else {
    // Pad atau potong name ke 8 char
    left = playName;
    while((int)left.length()<8) left += ' ';
    if(left.length()>8) left = left.substring(0,8);
  }

  // Susun baris 1: "NamaNama dur  "
  // 8 char + 1 spasi + 5 char dur + 2 spasi = 16
  char b1[17];
  snprintf(b1, sizeof(b1), "%-8s %-5s  ", left.c_str(), playDurStr.c_str());
  b1[16] = 0;

  lcd.setCursor(0,1);
  lcd.print(b1);
}
 
 
/* ============================================================
   LCD CLOCK
   Baris 0: "Sen 08:30:15"
   Baris 1: [kegiatan 8char] [spasi] [HH:MM]
   ============================================================ */
void lcdShowClock(){
  struct tm t;
  if(!getLocalTime(&t)) return;

  // Baris 0
  char b0[17];
  snprintf(b0,sizeof(b0),"%s %02d:%02d:%02d   ",
           DAY_KEY[t.tm_wday], t.tm_hour, t.tm_min, t.tm_sec);
  lcd.setCursor(0,0); lcd.print(b0);

  // Baris 1
  String jamLabel = nextJam.length()>0 ? nextJam : "--:--:--";
  String kegShow;
  if(nextKeg.length()==0){
    kegShow = "--------";
  } else if(nextKeg.length()<=7){
    kegShow = nextKeg;
    while(kegShow.length()<7) kegShow += ' ';
  } else {
    if(rtStr.length()==0) rtInit(nextKeg);
    kegShow = rtSlice();
    if(kegShow.length()==0) kegShow = nextKeg.substring(0,7);
  }

  char b1[17];
  snprintf(b1,sizeof(b1),"%-8s %s  ", kegShow.c_str(), jamLabel.c_str());
  b1[16]=0;
  lcd.setCursor(0,1); lcd.print(b1);
}

/* ============================================================
   UPDATE JADWAL BERIKUTNYA
   ============================================================ */
void updateNextSchedule(){
  if(!ntpSynced) return;
  struct tm t;
  if(!getLocalTime(&t)) return;
  int nowMenit = t.tm_hour*60 + t.tm_min;

  for(int d=0; d<7; d++){
    int wday = (t.tm_wday + d) % 7;
    const char* key = DAY_KEY[wday];
    if(!jadwalDoc.containsKey(key)) continue;

    JsonArray arr = jadwalDoc[key].as<JsonArray>();
    if(arr.isNull() || arr.size()==0) continue;

    int    bMenit = 9999;
    String bJam   = "";
    String bKeg   = "";

    for(JsonObject o : arr){
      const char* j  = o["jam"];
      const char* kg = o["kegiatan"];
      if(!j || !kg) continue;
      int jm = toMenit(j);
      if(jm < 0) continue;
      if(d==0 && jm <= nowMenit) continue;  // sudah lewat hari ini
      if(jm < bMenit){ bMenit=jm; bJam=j; bKeg=kg; }
    }

    if(bJam.length()>0){
      if(bJam!=nextJam || bKeg!=nextKeg){
        nextJam=bJam; nextKeg=bKeg;
        rtInit(nextKeg);
        Serial.println("[Next] "+nextKeg+" @ "+nextJam);
      }
      return;
    }
  }
  if(nextJam.length()>0||nextKeg.length()>0){
    nextJam=""; nextKeg=""; rtStr="";
    Serial.println("[Next] kosong");
  }
}

/* ============================================================
   PLAY TRACK
   ============================================================ */
void playTrack(int track, String keg){
  if(!dfReady){ Serial.println(">>> DFPlayer belum siap!"); return; }
  if(track<=0){ Serial.println(">>> Track tidak valid: "+String(track)); return; }
  Serial.println(">>> dfplayer.play("+String(track)+") keg="+keg);

  relayOn();    // pastikan amplifier sudah hidup sebelum play

  dfplayer.play(track);
  delay(100);
  playStartMs  = millis();
  playDurMs    = 0;
  playTrackNum = track;

  // Cari durasi dari audioDoc berdasarkan nomor track
  char buf[10]; snprintf(buf,sizeof(buf),"%03d.mp3",track);
  String targetFile = String(buf);
  for(JsonObject o : audioDoc.as<JsonArray>()){
    if(o["file"].as<String>() == targetFile){
      if(o.containsKey("dur")){
        playDurMs = (unsigned long)o["dur"].as<int>() * 1000UL;
        Serial.printf("[play] dur=%lu ms dari audio.json\n", playDurMs);
      }
      break;
    }
  }
  if(playDurMs == 0)
    Serial.println("[play] dur tidak ada di audio.json, pakai event DFPlayerPlayFinished");

  lcdShowPlay(targetFile, keg);
}

void stopAudio(){
  if(dfReady) dfplayer.stop();
  lcdMode     = LCD_CLOCK;
  bellPlaying = false;
  lcd.clear();
  updateNextSchedule();
  relayScheduleOff();   // relay mati 10 detik setelah stop
}

/* ============================================================
   CEK STATUS DFPLAYER
   ============================================================ */
void backToClock(){
  lcdMode     = LCD_CLOCK;
  bellPlaying = false;
  lcd.clear();
  updateNextSchedule();
  relayScheduleOff();   // relay mati 10 detik setelah audio selesai
  Serial.println("[LCD] Kembali ke tampilan jam");
}

void cekDFPlayer(){
  if(!dfReady) return;

  // Update elapsed time di baris 1 LCD setiap 1 detik
  updateLcdPlayLine();

  // Drain semua event dari buffer DFPlayer serial
  while(dfplayer.available()){
    uint8_t tp = dfplayer.readType();
    int     val = dfplayer.read();
    Serial.print("[DF] type="); Serial.print((int)tp);
    Serial.print(" val=");      Serial.println(val);

    if(tp == DFPlayerPlayFinished){
      Serial.println("[DF] Selesai -> backToClock");
      backToClock();
      return;
    }
    if(tp == DFPlayerError){
      Serial.print("[DF] Error -> backToClock, code="); Serial.println(val);
      backToClock();
      return;
    }
  }

  // Fallback timeout berdasarkan dur dari audio.json
  if(lcdMode == LCD_PLAY && playStartMs > 0){
    unsigned long elapsed = millis() - playStartMs;
    // Jika dur tersedia di audio.json, pakai itu + toleransi 3 detik
    unsigned long timeout = (playDurMs > 0) ? (playDurMs + 3000UL) : PLAY_TIMEOUT_MS;
    if(elapsed > timeout){
      Serial.printf("[DF] Timeout (%lums) -> backToClock\n", elapsed);
      backToClock();
    }
  }
}

/* ============================================================
   CEK JADWAL
   ============================================================ */
void cekJadwal(){
  if(!ntpSynced) return;
  struct tm t;
  if(!getLocalTime(&t)) return;

  if(lcdMode == LCD_CLOCK) lcdShowClock();

  // ── Warmup relay: ON jika jadwal ≤ AMPLI_WARMUP_S detik lagi ──
  if(!ampliOn && nextJam.length() == 5){
    int nowDetik = t.tm_hour*3600 + t.tm_min*60 + t.tm_sec;
    int belDetik = ((nextJam[0]-'0')*10+(nextJam[1]-'0'))*3600
                 + ((nextJam[3]-'0')*10+(nextJam[4]-'0'))*60;
    int selisih  = belDetik - nowDetik;
    if(selisih > 0 && selisih <= AMPLI_WARMUP_S){
      Serial.printf("[RELAY] Warmup: jadwal dalam %d detik\n", selisih);
      relayOn();
    }
  }

  if(t.tm_min == lastMinute) return;
  lastMinute = t.tm_min;

  Serial.printf("\n[Menit baru] %s %02d:%02d\n", DAY_KEY[t.tm_wday], t.tm_hour, t.tm_min);

  updateNextSchedule();

  if(bellPlaying){ Serial.println("[skip] cooldown"); return; }
  if(!dfReady)   { Serial.println("[skip] DFPlayer not ready"); return; }

  char jamNow[6];
  snprintf(jamNow,sizeof(jamNow),"%02d:%02d", t.tm_hour, t.tm_min);

  const char* key = DAY_KEY[t.tm_wday];

  // Debug: cek keberadaan key
  Serial.printf("[cekJadwal] key=%s jam=%s containsKey=%d\n",
                key, jamNow, jadwalDoc.containsKey(key));

  if(!jadwalDoc.containsKey(key)){
    // Debug: tampilkan semua key yang tersedia
    Serial.print("Keys tersedia: ");
    for(JsonPair p : jadwalDoc.as<JsonObject>())
      Serial.printf("[%s] ", p.key().c_str());
    Serial.println();
    return;
  }

  JsonArray arr = jadwalDoc[key].as<JsonArray>();
  if(arr.isNull()){ Serial.println("[skip] array null"); return; }

  Serial.printf("[cekJadwal] jumlah entri = %d\n", (int)arr.size());

  for(JsonObject o : arr){
    // Baca field dengan toleransi tipe
    const char* jadJam = o["jam"];
    const char* jadKeg = o["kegiatan"];
    int         track  = audioToTrack(o["audio"]);

    Serial.printf("  >> jam=[%s] keg=[%s] audio_raw=[%s] track=%d | cocok=%d\n",
                  jadJam ? jadJam : "NULL",
                  jadKeg ? jadKeg : "NULL",
                  o["audio"].as<String>().c_str(),
                  track,
                  jadJam ? (strcmp(jadJam, jamNow)==0 ? 1:0) : 0);

    if(!jadJam) continue;
    if(strcmp(jadJam, jamNow) == 0){
      Serial.println("  *** COCOK! Mainkan bel ***");
      playTrack(track, jadKeg ? String(jadKeg) : "");
      bellPlaying = true;
      bellStartMs = millis();
      return;
    }
  }
  Serial.println("[cekJadwal] tidak ada jadwal cocok");
}

/* ============================================================
   NTP
   ============================================================ */
void syncNTP(){
  configTime(TZ_OFFSET, 0, NTP_SERVER);
  Serial.print("NTP sync");
  struct tm t;
  int retry=0;
  while(!getLocalTime(&t) && retry<20){ Serial.print("."); delay(500); retry++; }
  if(retry<20){
    ntpSynced = true;
    char buf[20]; strftime(buf,sizeof(buf),"%d/%m/%Y %H:%M",&t);
    Serial.println(" OK: "+String(buf));
    lcd.clear(); lcd.setCursor(0,0); lcd.print("NTP OK");
    lcd.setCursor(0,1); lcd.print(buf);
    lastMinute = -99;
  } else {
    Serial.println(" GAGAL");
    lcd.clear(); lcd.print("NTP GAGAL");
  }
  delay(1500);
}

/* ============================================================
   WIFI
   ============================================================ */
void connectWiFi(){
  WiFi.begin(ssid, password);
  lcd.clear(); lcd.print("WiFi...");
  int r=0;
  while(WiFi.status()!=WL_CONNECTED && r<20){ delay(500); r++; }
  lcd.clear();
  if(WiFi.status()==WL_CONNECTED){
    lcd.print("IP:"); lcd.setCursor(0,1); lcd.print(WiFi.localIP());
    Serial.print("WiFi OK IP="); Serial.println(WiFi.localIP());
  } else {
    lcd.print("WiFi GAGAL"); Serial.println("WiFi GAGAL");
  }
  delay(1200);
}

/* ============================================================
   HELPER: stream file SPIFFS ke browser
   ============================================================ */
void serveFile(const char* path, const char* mime){
  if(!SPIFFS.exists(path)){
    server.send(404,"text/plain",String(path)+" not found");
    Serial.printf("[404] %s\n", path);
    return;
  }
  File f = SPIFFS.open(path,"r");
  server.streamFile(f, mime);
  f.close();
}

/* ============================================================
   WEB SERVER
   ============================================================ */
void setupServer(){

  // ── Static files ──────────────────────────────────────────
  server.on("/",          [](){ serveFile("/index.html","text/html");       });
  server.on("/index.html",[](){ serveFile("/index.html","text/html");       });
  server.on("/style.css", [](){ serveFile("/style.css", "text/css");        });
  server.on("/app.js",    [](){ serveFile("/app.js",    "application/javascript"); });

  // ── Jadwal ────────────────────────────────────────────────
  server.on("/jadwal.json", HTTP_GET, [](){
    serveFile("/jadwal.json","application/json");
  });

  server.on("/save-jadwal", HTTP_POST, [](){
    String body = server.arg("plain");
    File f = SPIFFS.open("/jadwal.json","w"); f.print(body); f.close();
    jadwalDoc.clear();
    DeserializationError e = deserializeJson(jadwalDoc, body);
    if(e){ Serial.print("[save-jadwal] parse err: "); Serial.println(e.c_str()); }
    Serial.print("[save-jadwal] Keys: ");
    for(JsonPair p : jadwalDoc.as<JsonObject>())
      Serial.printf("[%s]=%d ", p.key().c_str(), (int)p.value().as<JsonArray>().size());
    Serial.println();
    backupJadwal();
    lastMinute = -99;
    updateNextSchedule();
    server.send(200,"text/plain","OK");
  });

  // ── Audio ─────────────────────────────────────────────────
  server.on("/audio-list", HTTP_GET, [](){
    String out; serializeJson(audioDoc, out);
    server.send(200,"application/json", out);
  });

  server.on("/play-audio", HTTP_GET, [](){
    String file = server.arg("file");
    if(file.length() < 3){ server.send(400,"text/plain","Invalid"); return; }
    int track = file.substring(0,3).toInt();
    String keg = file;
    for(JsonObject o : audioDoc.as<JsonArray>())
      if(o["file"].as<String>() == file){ keg = o["name"].as<String>(); break; }
    playTrack(track, keg);
    server.send(200,"text/plain","PLAY");
  });

  server.on("/stop-audio", HTTP_GET, [](){
    stopAudio();
    server.send(200,"text/plain","STOP");
  });

  server.on("/volume", HTTP_GET, [](){
    volumeLevel = constrain(server.arg("val").toInt(), 0, 30);
    if(dfReady) dfplayer.volume(volumeLevel);
    saveEEPROM();
    server.send(200,"text/plain","OK");
  });

  // ── Status ────────────────────────────────────────────────
  server.on("/status", HTTP_GET, [](){
    struct tm t; String w = "N/A";
    if(getLocalTime(&t)){ char b[20]; strftime(b,sizeof(b),"%d/%m/%Y %H:%M:%S",&t); w = b; }
    String json = "{";
    json += "\"ntp\":"       + String(ntpSynced  ? "true":"false") + ",";
    json += "\"waktu\":\""   + w                                   + "\",";
    json += "\"isRinging\":" + String(bellPlaying? "true":"false") + ",";
    json += "\"ampliOn\":"   + String(ampliOn    ? "true":"false") + ",";
    json += "\"nextJam\":\""  + nextJam + "\",";
    json += "\"nextKeg\":\""  + nextKeg + "\"";
    json += "}";
    server.send(200,"application/json", json);
  });

  // ── Kegiatan ──────────────────────────────────────────────
  server.on("/kegiatan-list", HTTP_GET, [](){
    serveFile("/kegiatan.json","application/json");
  });

  server.on("/save-kegiatan", HTTP_POST, [](){
    File f = SPIFFS.open("/kegiatan.json","w");
    f.print(server.arg("plain")); f.close();
    backupKegiatan();
    server.send(200,"text/plain","OK");
  });

  // ── Backup & Restore ──────────────────────────────────────
  server.on("/backup-all", HTTP_GET, [](){
    String jadwalStr, kegStr;
    serializeJson(jadwalDoc, jadwalStr);
    File fk = SPIFFS.open("/kegiatan.json","r");
    if(fk){ kegStr = fk.readString(); fk.close(); } else kegStr = "[]";
    String out = "{\"jadwal\":" + jadwalStr + ",\"kegiatan\":" + kegStr + "}";
    server.sendHeader("Content-Disposition","attachment; filename=backup_bel.json");
    server.send(200,"application/json", out);
    Serial.println("[backup] backup-all dikirim");
  });

  server.on("/restore-backup", HTTP_POST, [](){
    String body = server.arg("plain");
    DynamicJsonDocument doc(12288);
    DeserializationError e = deserializeJson(doc, body);
    if(e){ server.send(400,"text/plain","Parse error: "+String(e.c_str())); return; }
    if(doc.containsKey("jadwal")){
      jadwalDoc.clear();
      jadwalDoc.set(doc["jadwal"]);
      File fj = SPIFFS.open("/jadwal.json","w");
      serializeJson(jadwalDoc, fj); fj.close();
      backupJadwal();
      lastMinute = -99;
      updateNextSchedule();
      Serial.printf("[restore] jadwal dipulihkan (%d entri)\n", countJadwalEntries());
    }
    if(doc.containsKey("kegiatan")){
      String kegStr; serializeJson(doc["kegiatan"], kegStr);
      File fk = SPIFFS.open("/kegiatan.json","w");
      fk.print(kegStr); fk.close();
      backupKegiatan();
      Serial.println("[restore] kegiatan dipulihkan");
    }
    server.send(200,"text/plain","RESTORE OK");
  });

  // ── Debug ─────────────────────────────────────────────────
  server.on("/debug-jadwal", HTTP_GET, [](){
    String out; serializeJsonPretty(jadwalDoc, out);
    server.send(200,"application/json", out);
  });

  server.on("/reset-jadwal", HTTP_GET, [](){
    const char* def =
      "{\"Senin\":[],\"Selasa\":[],\"Rabu\":[],\"Kamis\":"
      "[],\"Jumat\":[],\"Sabtu\":[],\"Minggu\":[]}";
    jadwalDoc.clear();
    deserializeJson(jadwalDoc, def);
    File f = SPIFFS.open("/jadwal.json","w"); f.print(def); f.close();
    lastMinute = -99; updateNextSchedule();
    Serial.println("[reset] jadwal.json dikosongkan");
    server.send(200,"text/plain","RESET OK");
  });

  // ── 404 fallback ──────────────────────────────────────────
  server.onNotFound([](){
    server.send(404,"text/plain","Not found: "+server.uri());
    Serial.println("[404] "+server.uri());
  });

  server.begin();
  Serial.println("Web server started");
}

/* ============================================================
   SETUP
   ============================================================ */
void setup(){
  Serial.begin(115200);

  // ── Matikan relay SEBELUM apapun ──────────────────────────
  // Cegah relay ON saat boot akibat GPIO floating
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_INACTIVE);
  delay(10);  // beri waktu pin stabil
  Serial.println("[RELAY] Boot → OFF");
  delay(300);
  Serial.println("\n\n=== BOOT ===");

  EEPROM.begin(EEPROM_SIZE);
  loadEEPROM();

  Wire.begin(21,22);
  lcd.begin(); lcd.backlight();
  lcd.clear(); lcd.print("Booting...");

  if(!SPIFFS.begin(true)){
    Serial.println("SPIFFS ERROR");
    lcd.clear(); lcd.print("SPIFFS ERROR");
    while(1) delay(1000);
  }

  loadAudio();
  loadJadwal();
  loadKegiatan();

  connectWiFi();
  if(WiFi.status()==WL_CONNECTED) syncNTP();

  setupServer();
  delay(300);
  initDFPlayer();

  lcdMode = LCD_CLOCK;
  lcd.clear();
  updateNextSchedule();
}

/* ============================================================
   LOOP
   ============================================================ */
void loop(){
  server.handleClient();
  cekJadwal();
  cekDFPlayer();
  cekAmpli();    // cek timer OFF relay amplifier

  if(bellPlaying && (millis()-bellStartMs >= BELL_COOLDOWN_MS)){
    bellPlaying = false;
    Serial.println("[cooldown] selesai");
  }

  delay(200);
}
