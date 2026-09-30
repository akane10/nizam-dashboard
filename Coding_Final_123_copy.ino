#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <EEPROM.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

//======================================================
// CATATAN VERSI V3.3.1 (revisi bug queueReset)
// Logic input mengikuti modul optocoupler yang digunakan:
// - LOW  = PLC 24V tersedia = RUNNING/ONLINE
// - HIGH = PLC 24V hilang = OFFLINE atau TRIP
// - LOW -> HIGH = TRIP
// - HIGH -> LOW = RUNNING KEMBALI
// - HIGH saat startup = OFFLINE, bukan TRIP.
// - Reset tidak membutuhkan transporter hidup.
// Pesan WhatsApp TRIP dipertahankan.
//
// PERBAIKAN DARI V3.3:
// - queueReset() dulu mengirim status "NORMAL" ke SEMUA channel tanpa
//   syarat, menimpa status ONLINE/OFFLINE yang sudah benar dikirim
//   per-channel di checkReset(). Sekarang dihapus - queueReset() cuma
//   kirim pesan WA, status dashboard sepenuhnya diatur checkReset().
//
// Perbaikan dari V3.0: sendDashboard() & sendHeartbeat()
// TIDAK LAGI dipanggil langsung di dalam checkTrip()/checkReset().
// Keduanya sekarang lewat antrean sendiri (dashQueue), sama seperti
// pesan WA. Jadi checkTrip() HANYA baca pin + push ke antrean,
// TIDAK PERNAH menunggu jaringan -> pin transporter lain tetap
// terbaca real-time walau ada trip beruntun atau server lambat.
//======================================================

//======================================================
// KONFIGURASI UNIT
// GANTI ANGKA INI DI TIAP ESP: 1, 2, atau 3
//======================================================
#define UNIT_ID 1

//======================================================
// WIFI
//======================================================
const char* ssid     = "";
const char* password = "";

//======================================================
// FONNTE (WhatsApp Gateway)
//======================================================
String fonnteToken = "";
// Pisahkan beberapa nomor tujuan dengan koma, TANPA spasi
String targets = ",";

//======================================================
// DASHBOARD SERVER
//======================================================
bool dashboardEnabled = true;
const char* dashboardHost   = "";
const char* dashboardApiKey = "nizam123";

//======================================================
// PIN MAPPING
// (D0/GPIO16 tidak mendukung INPUT_PULLUP -> tidak dipakai untuk input)
// (D3/D4/D8 adalah boot-strap pin -> dihindari untuk input)
//======================================================
#define RESET_PIN    D7   // GPIO13
#define BUZZER_PIN   D8   // GPIO15 (output saja, aman)
#define LED_PIN      D0   // GPIO16 (output saja, aman)

const uint8_t TRIP_PIN_COUNT = 4;
const uint8_t tripPins[TRIP_PIN_COUNT] = { D1, D2, D5, D6 }; // GPIO5, GPIO4, GPIO14, GPIO12

const char* tripLabels[TRIP_PIN_COUNT] = {
  "No.1", "No.2", "No.3", "No.4"
};

//======================================================
// EEPROM
// Layout: 4 x unsigned long (4 byte) = 16 byte, offset = index * 4
//======================================================
#define EEPROM_SIZE 64

//======================================================
// STRUCT TRANSPORTER (1 struct per sinyal)
//======================================================
enum TransporterStatus {
  STATUS_OFFLINE,
  STATUS_ONLINE,
  STATUS_TRIP
};

struct Transporter {
  uint8_t pin;
  bool lastState;
  bool tripDetected;
  bool messageSent;
  unsigned long tripCounter;
  const char* label;
  TransporterStatus status;
};

Transporter trans[TRIP_PIN_COUNT];

//======================================================
// ANTREAN PESAN WHATSAPP (FIFO)
//======================================================
#define WA_QUEUE_SIZE 12
String waQueue[WA_QUEUE_SIZE];
uint8_t waHead  = 0;
uint8_t waTail  = 0;
uint8_t waCount = 0;

void enqueueWA(String msg) {
  if (waCount >= WA_QUEUE_SIZE) {
    Serial.println("WA QUEUE PENUH - pesan tertua dibuang");
    waHead = (waHead + 1) % WA_QUEUE_SIZE;
    waCount--;
  }
  waQueue[waTail] = msg;
  waTail = (waTail + 1) % WA_QUEUE_SIZE;
  waCount++;
}

bool waPeek(String &msg) {
  if (waCount == 0) return false;
  msg = waQueue[waHead];
  return true;
}

void waPop() {
  if (waCount == 0) return;
  waHead = (waHead + 1) % WA_QUEUE_SIZE;
  waCount--;
}

//======================================================
// ANTREAN DASHBOARD (FIFO) - path & payload disimpan di 2 array
// terpisah (bukan struct) supaya tidak kena bug auto-prototype
// Arduino IDE, yang menaruh forward declaration SEBELUM struct
// custom didefinisikan kalau struct dipakai sebagai parameter fungsi.
//======================================================
#define DASH_QUEUE_SIZE 12
String dashPathQueue[DASH_QUEUE_SIZE];
String dashPayloadQueue[DASH_QUEUE_SIZE];
uint8_t dashHead  = 0;
uint8_t dashTail  = 0;
uint8_t dashCount = 0;

void enqueueDash(String path, String payload) {
  if (dashCount >= DASH_QUEUE_SIZE) {
    Serial.println("DASH QUEUE PENUH - item tertua dibuang");
    dashHead = (dashHead + 1) % DASH_QUEUE_SIZE;
    dashCount--;
  }
  dashPathQueue[dashTail] = path;
  dashPayloadQueue[dashTail] = payload;
  dashTail = (dashTail + 1) % DASH_QUEUE_SIZE;
  dashCount++;
}

bool dashPeek(String &path, String &payload) {
  if (dashCount == 0) return false;
  path = dashPathQueue[dashHead];
  payload = dashPayloadQueue[dashHead];
  return true;
}

void dashPop() {
  if (dashCount == 0) return;
  dashHead = (dashHead + 1) % DASH_QUEUE_SIZE;
  dashCount--;
}

//======================================================
// NTP
//======================================================
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 25200, 60000);

//======================================================
// STATUS GLOBAL
//======================================================
bool resetButtonLast   = HIGH;
bool wasWifiConnected  = true;

unsigned long lastReconnect  = 0;
unsigned long lastWaRetry    = 0;
unsigned long lastDashRetry  = 0;
unsigned long lastDebug      = 0;
unsigned long lastHeartbeat  = 0;
unsigned long lastResetCheck = 0;

//======================================================
// EEPROM HELPER
//======================================================
void saveCounter(uint8_t idx) {
  EEPROM.put(idx * sizeof(unsigned long), trans[idx].tripCounter);
  EEPROM.commit();
}

void loadCounters() {
  for (uint8_t i = 0; i < TRIP_PIN_COUNT; i++) {
    unsigned long val;
    EEPROM.get(i * sizeof(unsigned long), val);
    if (val > 999999UL) val = 0;
    trans[i].tripCounter = val;
  }
}

//======================================================
// JAM
//======================================================
String getTimeNow() {
  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
  }
  char jam[20];
  sprintf(jam, "%02d:%02d:%02d",
          timeClient.getHours(),
          timeClient.getMinutes(),
          timeClient.getSeconds());
  return String(jam);
}

//======================================================
// CONNECT WIFI
//======================================================
void connectWiFi() {
  Serial.println();
  Serial.println("==============================");
  Serial.println("CONNECTING WIFI");
  Serial.println("==============================");

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi Connected");
    Serial.print("IP Address : ");
    Serial.println(WiFi.localIP());
    timeClient.begin();
    timeClient.update();
  } else {
    Serial.println("WiFi Connection Failed");
  }
}

//======================================================
// HEADER
//======================================================
void printHeader() {
  Serial.println();
  Serial.println("==============================================");
  Serial.println("        BUMER V3.1 MULTI-TRANSPORTER");
  Serial.print(" UNIT ");
  Serial.println(UNIT_ID);
  Serial.println(" PLC TRANSPORTER MONITORING SYSTEM");
  Serial.println(" (dashboard & WA non-blocking queue)");
  Serial.println("==============================================");
}

//======================================================
// KIRIM WHATSAPP (Fonnte) - HANYA dipanggil dari processWaQueue()
// Timeout diperpendek supaya kalaupun blocking, jedanya singkat.
//======================================================
bool sendWhatsAppNow(String pesan) {
  if (WiFi.status() != WL_CONNECTED) return false;

  BearSSL::WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, "https://api.fonnte.com/send");
  http.setTimeout(8000); // diperpendek dari 15000

  http.addHeader("Authorization", fonnteToken);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String data = "target=" + targets + "&message=" + pesan;

  Serial.println();
  Serial.println("==================================");
  Serial.println("MENGIRIM WHATSAPP");
  Serial.println("==================================");

  int httpCode = http.POST(data);
  Serial.print("HTTP CODE : ");
  Serial.println(httpCode);

  bool sukses = false;
  if (httpCode > 0) {
    Serial.println(http.getString());
    sukses = true;
  } else {
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
  return sukses;
}

//======================================================
// KIRIM KE DASHBOARD - HANYA dipanggil dari processDashQueue()
// Mengembalikan true/false supaya antrean tahu apakah harus di-pop.
//======================================================
String dashboardUrl(const char* path) {
  return String(dashboardHost) + String(path);
}

bool sendDashboardRequest(HTTPClient &http, const String &payload) {
  http.addHeader("Content-Type", "application/json");
  if (strlen(dashboardApiKey) > 0) {
    http.addHeader("X-API-KEY", dashboardApiKey);
  }
  http.setTimeout(5000); // diperpendek dari 8000

  int code = http.POST(payload);
  Serial.print("Dashboard POST code: ");
  Serial.println(code);
  http.end();
  return (code >= 200 && code < 300);
}

bool postDashboardNow(const char* path, const String &payload) {
  if (!dashboardEnabled) return true;       // dianggap "selesai" kalau memang dimatikan
  if (WiFi.status() != WL_CONNECTED) return false;

  String url = dashboardUrl(path);
  bool isHttps = url.startsWith("https://");
  HTTPClient http;

  if (isHttps) {
    BearSSL::WiFiClientSecure secureClient;
    secureClient.setInsecure();
    if (!http.begin(secureClient, url)) return false;
    return sendDashboardRequest(http, payload);
  }

  WiFiClient client;
  if (!http.begin(client, url)) return false;
  return sendDashboardRequest(http, payload);
}

//======================================================
// BANGUN PAYLOAD & MASUKKAN KE ANTREAN (TIDAK BLOCKING)
//======================================================
void queueDashboardStatus(uint8_t idx, String status) {
  String payload = "{";
  payload += "\"unit\":" + String(UNIT_ID) + ",";
  payload += "\"transporter\":\"" + String(trans[idx].label) + "\",";
  payload += "\"status\":\"" + status + "\",";
  payload += "\"counter\":" + String(trans[idx].tripCounter) + ",";
  payload += "\"time\":\"" + getTimeNow() + "\"";
  payload += "}";

  enqueueDash("/api/status", payload);
}

void queueHeartbeat() {
  String payload = "{\"unit\":" + String(UNIT_ID) + "}";
  enqueueDash("/api/heartbeat", payload);
}

//======================================================
// CEK PERMINTAAN RESET DARI DASHBOARD (tombol "Reset counter" di web)
// GET /api/reset-check/<UNIT_ID> -> balasan teks polos: "No.1,No.3" atau kosong.
// Header X-API-KEY ikut dikirim kalau dashboardApiKey diisi.
// Kalau ada, reset counter transporter itu di EEPROM juga (supaya
// tidak "muncul lagi" saat trip berikutnya), lalu laporkan balik
// status terkininya (counter=0) ke dashboard.
//======================================================
void checkRemoteReset() {
  if (!dashboardEnabled) return;
  if (WiFi.status() != WL_CONNECTED) return;

  String url = dashboardUrl(("/api/reset-check/" + String(UNIT_ID)).c_str());
  bool isHttps = url.startsWith("https://");
  HTTPClient http;
  int httpCode = -1;
  String body = "";

  if (isHttps) {
    BearSSL::WiFiClientSecure secureClient;
    secureClient.setInsecure();
    if (http.begin(secureClient, url)) {
      http.setTimeout(5000);
      if (strlen(dashboardApiKey) > 0) {
        http.addHeader("X-API-KEY", dashboardApiKey);
      }
      httpCode = http.GET();
      if (httpCode > 0) body = http.getString();
      http.end();
    }
  } else {
    WiFiClient client;
    if (http.begin(client, url)) {
      http.setTimeout(5000);
      if (strlen(dashboardApiKey) > 0) {
        http.addHeader("X-API-KEY", dashboardApiKey);
      }
      httpCode = http.GET();
      if (httpCode > 0) body = http.getString();
      http.end();
    }
  }

  if (httpCode != 200 || body.length() == 0) return;

  Serial.print("Permintaan reset dari dashboard: ");
  Serial.println(body);

  // body contoh: "No.1,No.3" -> pecah per koma
  int start = 0;
  while (start < (int)body.length()) {
    int comma = body.indexOf(',', start);
    String label = (comma == -1) ? body.substring(start) : body.substring(start, comma);
    label.trim();

    for (uint8_t i = 0; i < TRIP_PIN_COUNT; i++) {
      if (label == trans[i].label) {
        trans[i].tripCounter = 0;
        saveCounter(i);
        Serial.print(trans[i].label);
        Serial.println(" - counter direset dari dashboard");

        // Laporkan status aktual setelah counter direset.
        // LOW = ONLINE/RUNNING, HIGH = OFFLINE.
        bool nowRunning = digitalRead(trans[i].pin);
        if (nowRunning == LOW) {
          trans[i].status = STATUS_ONLINE;
          queueDashboardStatus(i, "ONLINE");
        } else {
          trans[i].status = STATUS_OFFLINE;
          queueDashboardStatus(i, "OFFLINE");
        }
        break;
      }
    }

    if (comma == -1) break;
    start = comma + 1;
  }
}

//======================================================
// PROSES ANTREAN - dipanggil berkala dari loop()
// Masing-masing MAKSIMAL proses 1 item per pemanggilan,
// supaya loop() tidak lama berhenti dan pin tetap terbaca.
//======================================================
void processWaQueue() {
  String msg;
  if (!waPeek(msg)) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (sendWhatsAppNow(msg)) {
    waPop();
    Serial.println("WA terkirim, dihapus dari antrean");
  } else {
    Serial.println("WA gagal, tetap di antrean, dicoba lagi nanti");
  }
}

void processDashQueue() {
  String path, payload;
  if (!dashPeek(path, payload)) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (postDashboardNow(path.c_str(), payload)) {
    dashPop();
    Serial.println("Dashboard terkirim, dihapus dari antrean");
  } else {
    Serial.println("Dashboard gagal, tetap di antrean, dicoba lagi nanti");
  }
}

//======================================================
// BANGUN & ANTRE PESAN WA (teks notifikasi)
//======================================================
void queueOnline() {
  String msg = "\xF0\x9F\x9F\xA2 SYSTEM ONLINE\n\n";
  msg += "UNIT " + String(UNIT_ID) + " - PLC TRANSPORTER MONITORING AKTIF\n\n";
  msg += "IP : " + WiFi.localIP().toString();
  msg += "\nJam : " + getTimeNow() + " WIB";
  enqueueWA(msg);
}

void queueTrip(uint8_t idx) {
  trans[idx].tripCounter++;
  saveCounter(idx);

  String msg = "\xF0\x9F\x9A\xA8 TRANSPORTER TRIP \xF0\x9F\x9A\xA8\n\n";
  msg += "Unit " + String(UNIT_ID) + " - Transporter #" + String(UNIT_ID) + " " + String(trans[idx].label) + "\n";
  msg += "Status : TRIP ACTIVE\n";
  msg += "Counter : " + String(trans[idx].tripCounter) + "\n";
  msg += "Jam : " + getTimeNow() + " WIB\n\n";
  msg += "SEGERA RESET\n";
  msg += "INSPEKSI FLUSHING FLM\n";
  msg += "& DIOPERASIKAN KEMBALI\n";

  enqueueWA(msg);
  queueDashboardStatus(idx, "TRIP");   // <-- tidak blocking, cuma masuk antrean
}

//======================================================
// NOTIFIKASI SAAT TEGANGAN 24V BENAR-BENAR KEMBALI
// Dipanggil dari checkTrip() saat transisi HIGH->LOW (tegangan PLC
// nyata kembali ada), BUKAN saat tombol reset ditekan.
//======================================================
void queueRunningNormal(uint8_t idx) {
  String msg = "\xE2\x9C\x85 TRANSPORTER RUNNING NORMAL KEMBALI\n\n";
  msg += "Unit " + String(UNIT_ID) + " - Transporter #" + String(UNIT_ID) + " " + String(trans[idx].label) + "\n";
  msg += "Status : Tegangan 24V PLC sudah tersedia kembali\n";
  msg += "Jam : " + getTimeNow() + " WIB";

  enqueueWA(msg);
}

void queueReset() {
  String msg = "\xE2\x9C\x85 INSPEKSI KONDISI EQUIPMENT\n\n";
  msg += "Unit " + String(UNIT_ID) + " - Status : RESET OLEH OPERATOR\n";
  msg += "Jam : " + getTimeNow() + " WIB";
  enqueueWA(msg);

  // CATATAN REVISI: loop pengiriman queueDashboardStatus(i, "NORMAL") di sini
  // SENGAJA DIHAPUS. Status per-channel (ONLINE/OFFLINE) sudah dikirim dengan
  // benar di checkReset() SEBELUM fungsi ini dipanggil - kalau di sini masih
  // kirim "NORMAL" untuk semua channel tanpa syarat, itu akan menimpa status
  // yang sudah benar (termasuk channel yang seharusnya tetap OFFLINE karena
  // belum ada tegangan PLC, sesuai state machine TRIP+RESET+0V -> OFFLINE).
}

void queueWifiLost() {
  String msg = "\xE2\x9A\xA0\xEF\xB8\x8F WARNING\n\n";
  msg += "Unit " + String(UNIT_ID) + " - WiFi Connection Lost\nMonitoring Offline";
  enqueueWA(msg);
}

void queueWifiBack() {
  String msg = "\xF0\x9F\x9F\xA2 Unit " + String(UNIT_ID) + " WiFi Reconnected\n\nMonitoring Aktif Kembali";
  enqueueWA(msg);
}

//======================================================
// ALARM
//======================================================
void alarmON() {
  digitalWrite(LED_PIN, HIGH);
  tone(BUZZER_PIN, 2500);
}

void alarmOFF() {
  digitalWrite(LED_PIN, LOW);
  noTone(BUZZER_PIN);
}

//======================================================
// SETUP
//======================================================
void setup() {
  Serial.begin(115200);
  printHeader();

  EEPROM.begin(EEPROM_SIZE);

  pinMode(RESET_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  alarmOFF();

  for (uint8_t i = 0; i < TRIP_PIN_COUNT; i++) {
    trans[i].pin = tripPins[i];
    trans[i].label = tripLabels[i];
    trans[i].tripDetected = false;
    trans[i].messageSent = false;
    trans[i].status = STATUS_OFFLINE;
    pinMode(trans[i].pin, INPUT_PULLUP);
  }

  loadCounters();
  connectWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    queueOnline();
    queueHeartbeat();
  }

  for (uint8_t i = 0; i < TRIP_PIN_COUNT; i++) {
    trans[i].lastState = digitalRead(trans[i].pin);

    // STARTUP:
    // LOW  = tegangan PLC tersedia -> ONLINE / RUNNING
    // HIGH = tegangan PLC belum tersedia -> OFFLINE
    // HIGH saat startup TIDAK dianggap TRIP.
    if (trans[i].lastState == LOW) {
      trans[i].status = STATUS_ONLINE;
      queueDashboardStatus(i, "ONLINE");
    } else {
      trans[i].status = STATUS_OFFLINE;
      queueDashboardStatus(i, "OFFLINE");
    }

    Serial.print(trans[i].label);
    Serial.print(" - Kondisi Awal : ");
    if (trans[i].status == STATUS_ONLINE) {
      Serial.println("ONLINE / RUNNING");
    } else {
      Serial.println("OFFLINE");
    }
  }

  Serial.println();
  Serial.print("BUMER READY - UNIT ");
  Serial.println(UNIT_ID);
  Serial.println("==============================");
}

//======================================================
// WIFI MONITOR
//======================================================
void wifiMonitor() {
  if (millis() - lastReconnect < 30000) return;
  lastReconnect = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!wasWifiConnected) {
      queueWifiBack();
      queueHeartbeat();
      wasWifiConnected = true;
    }
    return;
  }

  if (wasWifiConnected) {
    queueWifiLost();
    wasWifiConnected = false;
  }

  Serial.println();
  Serial.println("WiFi Disconnect... mencoba reconnect");
  WiFi.disconnect(true);
  delay(1000);
  connectWiFi();
}

//======================================================
// DETEKSI TRIP - semua 4 transporter dalam unit ini
// PENTING: fungsi ini HANYA baca pin + push ke antrean.
// TIDAK ADA panggilan jaringan di sini -> selalu cepat (~mikrodetik),
// jadi trip di 1 channel TIDAK PERNAH menghalangi pembacaan channel lain.
//======================================================
void checkTrip() {
  for (uint8_t i = 0; i < TRIP_PIN_COUNT; i++) {
    bool state = digitalRead(trans[i].pin);

    // LOW = RUNNING / tegangan PLC tersedia
    // HIGH = tegangan PLC hilang
    //
    // LOW -> HIGH = TRIP
    // HIGH -> LOW = RUNNING KEMBALI

    // LOW -> HIGH = tegangan PLC hilang = TRIP
    if (trans[i].lastState == LOW && state == HIGH) {
      Serial.println();
      Serial.print(trans[i].label);
      Serial.println(" TRIP (tegangan PLC hilang)");

      trans[i].status = STATUS_TRIP;

      if (!trans[i].messageSent) {
        trans[i].tripDetected = true;
        trans[i].messageSent = true;

        alarmON();
        queueTrip(i);
      }
    }

    // HIGH -> LOW = tegangan PLC kembali = RUNNING
    if (trans[i].lastState == HIGH && state == LOW) {
      Serial.print(trans[i].label);
      Serial.println(" RUNNING KEMBALI");

      trans[i].status = STATUS_ONLINE;
      trans[i].tripDetected = false;
      trans[i].messageSent = false;

      queueDashboardStatus(i, "ONLINE");
      queueRunningNormal(i);   // <-- notifikasi WA: tegangan 24V benar-benar kembali
    }

    trans[i].lastState = state;
  }
}

//======================================================
// RESET SISTEM
// 1 tombol reset -> mereset ke-4 transporter dalam unit ini sekaligus
//======================================================
void checkReset() {
  bool resetState = digitalRead(RESET_PIN);

  if (resetState == LOW && resetButtonLast == HIGH) {
    delay(40); // debounce
    if (digitalRead(RESET_PIN) == LOW) {
      Serial.println("SYSTEM RESET");

      for (uint8_t i = 0; i < TRIP_PIN_COUNT; i++) {
        trans[i].tripDetected = false;
        trans[i].messageSent = false;

        // Setelah reset, status mengikuti input aktual:
        // LOW = ONLINE/RUNNING, HIGH = OFFLINE.
        if (digitalRead(trans[i].pin) == LOW) {
          trans[i].status = STATUS_ONLINE;
          queueDashboardStatus(i, "ONLINE");
        } else {
          trans[i].status = STATUS_OFFLINE;
          queueDashboardStatus(i, "OFFLINE");
        }
      }

      alarmOFF();
      queueReset();

      while (digitalRead(RESET_PIN) == LOW) {
        delay(10);
      }
    }
  }

  resetButtonLast = resetState;
}

//======================================================
// LOOP
//======================================================
void loop() {
  wifiMonitor();

  if (WiFi.status() == WL_CONNECTED) {
    timeClient.update();
  }

  checkTrip();   // <-- cepat, tanpa jaringan
  checkReset();  // <-- cepat, tanpa jaringan

  // Debug serial tiap 2 detik
  if (millis() - lastDebug >= 2000) {
    lastDebug = millis();

    Serial.println();
    Serial.print("========== UNIT ");
    Serial.print(UNIT_ID);
    Serial.println(" STATUS ==========");

    Serial.print("WiFi          : ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");

    for (uint8_t i = 0; i < TRIP_PIN_COUNT; i++) {
      Serial.print(trans[i].label);
      Serial.print(" : ");

      if (trans[i].status == STATUS_OFFLINE) {
        Serial.print("OFFLINE");
      } else {
        Serial.print(digitalRead(trans[i].pin) == LOW ? "RUNNING" : "TRIP");
      }

      Serial.print(" | Input: ");
      Serial.print(digitalRead(trans[i].pin) == LOW ? "LOW" : "HIGH");
      Serial.print(" | Counter: ");
      Serial.println(trans[i].tripCounter);
    }

    Serial.print("Antrean WA       : ");
    Serial.println(waCount);
    Serial.print("Antrean Dashboard: ");
    Serial.println(dashCount);
    Serial.println("===================================");
  }

  // Proses 1 pesan WA setiap 4 detik
  if (millis() - lastWaRetry >= 4000) {
    lastWaRetry = millis();
    processWaQueue();
  }

  // Proses 1 update dashboard setiap 4 detik (timer terpisah dari WA,
  // supaya keduanya tidak selalu blocking bersamaan di loop yang sama)
  if (millis() - lastDashRetry >= 4000) {
    lastDashRetry = millis();
    processDashQueue();
  }

  // Heartbeat tiap 30 detik (lewat antrean, bukan langsung)
  if (millis() - lastHeartbeat >= 30000) {
    lastHeartbeat = millis();
    queueHeartbeat();
  }

  // Cek permintaan reset counter dari dashboard tiap 15 detik
  if (millis() - lastResetCheck >= 15000) {
    lastResetCheck = millis();
    checkRemoteReset();
  }

  yield();
  delay(20);
}
