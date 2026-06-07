#include <Arduino.h>
#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <time.h> 

#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

// ==================== KONFIGURASI WIFI & FIREBASE ====================
#define WIFI_SSID "4G-MIFI-442F"
#define WIFI_PASSWORD "1234567890"

#define FIREBASE_HOST "monitoring-sungai-eeb0b-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "hGJ5eRc25bDTQ6oZne2CywgcaRu8a77F29xUjxGZ"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 25200; 
const int   daylightOffset_sec = 0;

// ==================== PIN SENSOR & AKTUATOR ====================
#define TRIG_PIN 18
#define ECHO_PIN 19
#define FLOW_SENSOR_PIN 14
#define BUZZER_PIN 23 

// ==================== VARIABEL SENSOR & FUZZY ====================
volatile int flowPulse = 0;
float debit = 0.0;
float tinggiAir = 0.0;

const float TINGGI_DASAR_SUNGAI = 205.0; 
float nilaiKondisi = 0.0; 

// INTERRUPT FLOW SENSOR
void IRAM_ATTR flowInterrupt() {
  flowPulse++;
}

// ==================== FUNGSI KEANGGOTAAN FUZZY ====================
float trapmf(float x, float a, float b, float c, float d) {
  if (x < a || x > d) return 0.0;
  if (x >= b && x <= c) return 1.0;
  if (x >= a && x < b) return (b == a) ? 1.0 : (x - a) / (b - a);
  if (x > c && x <= d) return (d == c) ? 1.0 : (d - x) / (d - c);
  return 0.0;
}

float trimf(float x, float a, float b, float c) {
  if (x <= a || x >= c) return 0.0;
  if (x == b) return 1.0;
  if (x > a && x < b) return (b == a) ? 1.0 : (x - a) / (b - a);
  if (x > b && x < c) return (c == b) ? 1.0 : (c - x) / (c - b);
  return 0.0;
}

// ==================== FUNGSI PEMBACAAN SENSOR ====================
float bacaTinggiAir() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000); 
  if (duration == 0) return 0.0; 
  
  float jarakSensorKeAir = duration * 0.034 / 2;
  float kalkulasiTinggi = TINGGI_DASAR_SUNGAI - jarakSensorKeAir;
  
  if (kalkulasiTinggi < 0) {
    kalkulasiTinggi = 0.0;
  }
  return kalkulasiTinggi;
}

float bacaDebit() {
  flowPulse = 0;
  detachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN));
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flowInterrupt, FALLING);
  
  delay(1000); 
  
  detachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN));
  return (flowPulse / 7.5);
}

// ==================== FUNGSI WAKTU FIREBASE ====================
String dapatkanWaktu() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "Gagal";
  }
  char timeStringBuff[15];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M", &timeinfo);
  return String(timeStringBuff);
}

// ==================== LOGIKA FUZZY MAMDANI ====================
String fuzzyMamdani(float tinggi, float flow) {
  // --- Fuzzifikasi ---
  float tinggiRendah = trapmf(tinggi, 0, 0, 75, 85);
  float tinggiSedang = trimf(tinggi, 75, 113, 150);
  float tinggiTinggi = trapmf(tinggi, 140, 150, 200, 200);

  float normal = trapmf(flow, 0, 0, 5, 7);
  float deras = trimf(flow, 5, 8, 11);
  float sangatDeras = trapmf(flow, 9, 11, 16, 16);

  // --- Rule Base (Evaluasi Aturan Berdasarkan Tabel 3.4) ---
  float r1 = min(tinggiRendah, normal);       // No.1: Rendah & Normal -> Aman
  float r2 = min(tinggiRendah, deras);        // No.2: Rendah & Deras -> Aman
  float r3 = min(tinggiRendah, sangatDeras);  // No.3: Rendah & Sangat Deras -> Waspada

  float r4 = min(tinggiSedang, normal);       // No.4: Sedang & Normal -> Waspada
  float r5 = min(tinggiSedang, deras);        // No.5: Sedang & Deras -> Waspada
  float r6 = min(tinggiSedang, sangatDeras);  // No.6: Sedang & Sangat Deras -> Bahaya

  float r7 = min(tinggiTinggi, normal);       // No.7: Tinggi & Normal -> Bahaya
  float r8 = min(tinggiTinggi, deras);        // No.8: Tinggi & Deras -> Bahaya
  float r9 = min(tinggiTinggi, sangatDeras);  // No.9: Tinggi & Sangat Deras -> Bahaya

  // Pengelompokan status (Agregasi Rule)
  float outAman = max(r1, r2);
  float outWaspada = max(r3, max(r4, r5));
  float outBahaya = max(r6, max(r7, max(r8, r9)));

  // --- Defuzzifikasi (Metode Centroid / Center of Area) ---
  float pembilang = 0.0;
  float penyebut = 0.0;
  int langkah = 2;

  for (int i = 0; i <= 100; i += langkah) {
    float x = (float)i;

    float muAman = trapmf(x, 0, 0, 30, 40); 
    float muWaspada = trimf(x, 30, 50, 70);
    float muBahaya = trapmf(x, 60, 80, 100, 100);

    float implAman = min(muAman, outAman);
    float implWaspada = min(muWaspada, outWaspada);
    float implBahaya = min(muBahaya, outBahaya);

    float muAgg = max(implAman, max(implWaspada, implBahaya));

    pembilang += (x * muAgg);
    penyebut += muAgg;
  }

  if (penyebut > 0) {
    nilaiKondisi = pembilang / penyebut; 
  } else {
    nilaiKondisi = 0.0; 
  }

  // --- Penentuan Status ---
  if (nilaiKondisi >= 66.7) {
    return "Bahaya";
  } else if (nilaiKondisi >= 33.4) {
    return "Waspada";
  } else {
    return "Aman";
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  
  pinMode(BUZZER_PIN, OUTPUT);
  noTone(BUZZER_PIN); // Memastikan buzzer pasif diam saat alat baru menyala

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Menghubungkan ke Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println("\nTerhubung ke Wi-Fi!");

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Sinkronisasi waktu selesai!");

  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  Serial.println("Sistem Monitoring Banjir Siap...");
}

void loop() {
  tinggiAir = bacaTinggiAir();
  tinggiAir = round(tinggiAir * 10.0) / 10.0;

  debit = bacaDebit(); 
  debit = round(debit * 10.0) / 10.0; 
  
  String statusBanjir = fuzzyMamdani(tinggiAir, debit);
  
  // Menggunakan satu fungsi waktu terpadu tanpa Sheets
  String waktuSekarang = dapatkanWaktu(); 

  // ==================== KONTROL BUZZER PASIF ====================
  if (statusBanjir == "Bahaya") {
    tone(BUZZER_PIN, 2300); // Mengirimkan sinyal frekuensi 2300 Hz
  } else {
    noTone(BUZZER_PIN);     // Mematikan sinyal frekuensi sepenuhnya
  }

  // ==================== TAMPILKAN DI SERIAL ====================
  Serial.print("Waktu        : "); Serial.println(waktuSekarang);
  Serial.print("Tinggi Air   : "); Serial.print(tinggiAir, 1); Serial.println(" cm");
  Serial.print("Debit Air    : "); Serial.print(debit, 1); Serial.println(" L/min");
  Serial.print("Status       : "); Serial.println(statusBanjir);

  // ==================== KIRIM DATA KE FIREBASE ====================
  if (Firebase.ready()) {
    // 1. Catat Waktu Mulai (Untuk pengujian Serial Monitor)
    unsigned long waktuMulaiKirim = millis();

    // 2. Mengambil Waktu Epoch Presisi Milidetik (Untuk Aplikasi Kodular)
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double epochMillis = (double)(tv.tv_sec) * 1000.0 + (double)(tv.tv_usec) / 1000.0;

    // 3. Proses Push Data
    Firebase.RTDB.setString(&fbdo, "/Pemantauan/Waktu", waktuSekarang);
    Firebase.RTDB.setFloat(&fbdo, "/Pemantauan/TinggiAir", tinggiAir);
    Firebase.RTDB.setFloat(&fbdo, "/Pemantauan/DebitAir", debit);
    Firebase.RTDB.setString(&fbdo, "/Pemantauan/Status", statusBanjir);
    
    // Baris baru untuk menembakkan tag "Timestamp" ke Aplikasi
    Firebase.RTDB.setDouble(&fbdo, "/Pemantauan/Timestamp", epochMillis); 

    // 4. Catat Waktu Selesai & Tampilkan Latensi Lokal
    unsigned long waktuSelesaiKirim = millis();
    unsigned long delayFirebase = waktuSelesaiKirim - waktuMulaiKirim;

    Serial.println("-> Firebase Updated");
    Serial.print("   Kecepatan Transmisi ESP32: ");
    Serial.print(delayFirebase);
    Serial.println(" ms");
  }

  Serial.println("-----------------------------");
}