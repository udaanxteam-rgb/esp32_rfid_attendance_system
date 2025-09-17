// ESP32 full sketch: multiple masters, secured add, bind + atc with composite roll shown
// 20x4-safe LCD messages, retries for slow networks.
// Make sure to set Web_App_URL to your deployed Apps Script URL(Version 13).

#include <LiquidCrystal_I2C.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>

#define SS_PIN  5
#define RST_PIN 4
#define BTN_PIN 15
#define BUZZER_PIN 2

// WiFi credentials - update if needed
const char* ssid = "There's nothing";
const char* password = "password";

// Web App (replace with your deployed URL)
String Web_App_URL = "https://script.google.com/macros/s/AKfycbxOHzFKhqHaVrhrgg8FXsRAO6WZeA3n91uhIVsczPlULmqp01n6FAvrL_MF8maxyvtl1Q/exec";

int lcdColumns = 20;
int lcdRows = 4;
LiquidCrystal_I2C lcd(0x27, lcdColumns, lcdRows);

MFRC522 mfrc522(SS_PIN, RST_PIN);
char strUID[32] = "";
String UID_Result = "";
String modes = "atc"; // "atc" or "reg"

Preferences prefs;
const char* PREF_NAMESPACE = "udaan_prefs";
const char* PREF_KEY_MASTERS = "master_uids"; // comma separated
String MASTER_UIDS_CSV = "";
String LAST_MASTER_UID = "";

// master add window / secured add
bool allow_master_add_window = false;
unsigned long allow_master_add_window_ts = 0;
const unsigned long ALLOW_MASTER_WINDOW_MS = 10000; // 10s allowed window after master tap
bool waiting_for_master_capture = false;
unsigned long btnPressStart = 0;
bool btnWasPressed = false;

// tap debounce - prevents immediate duplicate processing (ms)
const unsigned long MIN_TAP_GUARD_MS = 1500;
String lastProcessedUID = "";
unsigned long lastProcessedTS = 0;

// ---------- LCD helpers ----------
void lcdClearAll() {
  for (int r=0; r<lcdRows; ++r) {
    lcd.setCursor(0,r);
    for (int i=0;i<lcdColumns;++i) lcd.print(' ');
  }
  lcd.setCursor(0,0);
}
void lcdPrintLeft(int row, const String &text, int col = 0) {
  String s = text;
  if (col < 0) col = 0;
  if (col >= lcdColumns) return;
  if ((int)s.length() > lcdColumns - col) s = s.substring(0, lcdColumns - col);
  lcd.setCursor(col, row);
  lcd.print(s);
  int rem = lcdColumns - col - s.length();
  for (int i = 0; i < rem; ++i) lcd.print(' ');
}
void lcdPrintCentered(int row, const String &text) {
  String s = text;
  if ((int)s.length() > lcdColumns) s = s.substring(0, lcdColumns);
  int pos = (lcdColumns - s.length()) / 2;
  if (pos < 0) pos = 0;
  lcdPrintLeft(row, s, pos);
}

// ---------- tones ----------
void modeSwitchTone() { tone(BUZZER_PIN, 700, 120); delay(140); tone(BUZZER_PIN, 700, 120); delay(140); }
void wifiConnectedTone() { tone(BUZZER_PIN, 1000, 160); delay(140); tone(BUZZER_PIN, 1500, 160); delay(140); }
void startupTone() { tone(BUZZER_PIN, 300, 180); delay(200); tone(BUZZER_PIN, 500, 180); delay(200); tone(BUZZER_PIN, 800, 240); delay(250); }
void toneTI() { tone(BUZZER_PIN, 1500, 200); }
void toneTO() { tone(BUZZER_PIN, 1200, 200); }
void toneError() { tone(BUZZER_PIN, 500, 200); delay(220); }
void toneInfo() { tone(BUZZER_PIN, 900, 160); delay(180); }

// ---------- small utilities ----------
String trimStr(const String &s) {
  String r = s;
  while (r.length() && isspace(r.charAt(0))) r.remove(0,1);
  while (r.length() && isspace(r.charAt(r.length()-1))) r.remove(r.length()-1,1);
  return r;
}

// Master UID CSV helpers
bool isMasterUID(const String &uid) {
  if (uid.length() == 0) return false;
  if (MASTER_UIDS_CSV.length() == 0) return false;
  int start = 0;
  while (start < MASTER_UIDS_CSV.length()) {
    int idx = MASTER_UIDS_CSV.indexOf(',', start);
    String token;
    if (idx == -1) { token = MASTER_UIDS_CSV.substring(start); start = MASTER_UIDS_CSV.length(); }
    else { token = MASTER_UIDS_CSV.substring(start, idx); start = idx + 1; }
    token = trimStr(token);
    if (token == uid) return true;
  }
  return false;
}
void addMasterUID(const String &uid) {
  String token = trimStr(uid);
  if (token.length() == 0) return;
  if (MASTER_UIDS_CSV.length() == 0) MASTER_UIDS_CSV = token;
  else if (MASTER_UIDS_CSV.indexOf(token) == -1) MASTER_UIDS_CSV += "," + token;
  prefs.putString(PREF_KEY_MASTERS, MASTER_UIDS_CSV);
}
void loadMastersFromPrefs() {
  MASTER_UIDS_CSV = prefs.getString(PREF_KEY_MASTERS, "");
  Serial.print("Loaded MASTER_UIDS: "); Serial.println(MASTER_UIDS_CSV);
}

// Split helper (same as server)
String getValue(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = { 0, -1 };
  int maxIndex = data.length() - 1;
  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i+1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

// Byte array -> hex
void byteArray_to_string(byte array[], unsigned int len, char buffer[]) {
  for (unsigned int i = 0; i < len; i++) {
    byte nib1 = (array[i] >> 4) & 0x0F;
    byte nib2 = (array[i] >> 0) & 0x0F;
    buffer[i*2+0] = nib1  < 0xA ? '0' + nib1  : 'A' + nib1  - 0xA;
    buffer[i*2+1] = nib2  < 0xA ? '0' + nib2  : 'A' + nib2  - 0xA;
  }
  buffer[len*2] = '\0';
}

// Trim payload whitespace
String trimPayload(String p) {
  while (p.length() && (p.charAt(0) == '\n' || p.charAt(0) == '\r' || p.charAt(0) == ' ' || p.charAt(0) == '\t')) p.remove(0,1);
  while (p.length() && (p.charAt(p.length()-1) == '\n' || p.charAt(p.length()-1) == '\r' || p.charAt(p.length()-1) == ' ' || p.charAt(p.length()-1) == '\t')) p.remove(p.length()-1,1);
  return p;
}

// ---------- HTTP request and response processing ----------
void http_Req(String str_modes, String str_uid) {
  if (WiFi.status() != WL_CONNECTED) {
    lcdClearAll(); lcdPrintCentered(1, "WiFi lost"); lcdPrintLeft(2, "Try later"); toneError(); delay(1200); lcdClearAll();
    return;
  }

  String http_req_url = "";
  if (str_modes == "atc") http_req_url = Web_App_URL + "?sts=atc&uid=" + str_uid;
  else if (str_modes == "bind") {
    String adminParam = "";
    if (LAST_MASTER_UID.length() > 0) adminParam = "&admin=" + LAST_MASTER_UID;
    http_req_url = Web_App_URL + "?sts=bind&uid=" + str_uid + adminParam;
  }

  Serial.println("HTTP: "); Serial.println(http_req_url);

  HTTPClient http;
  http.begin(http_req_url.c_str());
  http.setTimeout(9000); // 9 seconds
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  int httpCode = -1;
  String payload = "";

  if (str_modes == "bind") {
    const int MAX_ATTEMPTS = 3;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
      httpCode = http.GET();
      Serial.printf("HTTP attempt %d, code: %d\n", attempt, httpCode);
      if (httpCode > 0) { payload = http.getString(); Serial.print("Raw Payload: "); Serial.println(payload); break; }
      else { Serial.println("HTTP GET failed, retrying..."); lcdClearAll(); lcdPrintCentered(1, "Net retry..."); delay(300 * attempt); }
    }
  } else { // atc
    const int MAX_ATTEMPTS = 2;
    int backoffMs[] = {0, 1200};
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt) {
      if (attempt > 1) { lcdClearAll(); lcdPrintCentered(1, "Retrying..."); }
      if (backoffMs[attempt-1] > 0) delay(backoffMs[attempt-1]);
      httpCode = http.GET();
      Serial.printf("HTTP atc attempt %d, code: %d\n", attempt, httpCode);
      if (httpCode > 0) { payload = http.getString(); Serial.print("Raw Payload: "); Serial.println(payload); break; }
      else { Serial.println("HTTP GET failed for atc"); }
    }
  }

  http.end();

  payload = trimPayload(payload);
  Serial.print("Trimmed Payload: '"); Serial.print(payload); Serial.println("'");

  if (httpCode <= 0) {
    lcdClearAll(); lcdPrintCentered(1, "Network error"); lcdPrintLeft(2, "Try again"); toneError(); delay(1200); lcdClearAll();
    return;
  }

  String sts_Res = getValue(payload, ',', 0); sts_Res.trim();
  if (sts_Res != "OK") {
    lcdClearAll(); lcdPrintCentered(0, "Server resp:"); String frag = payload; if (frag.length()>20) frag = frag.substring(0,20); lcdPrintLeft(2, frag); toneError(); delay(1400); lcdClearAll();
    return;
  }

  if (str_modes == "atc") {
    String atc_Info = getValue(payload, ',', 1); atc_Info.trim();

    if (atc_Info == "TI_Successful") {
      toneTI();
      // payload: OK,TI_Successful,Composite,Date,TimeIn,StudentName
      String composite = getValue(payload, ',', 2);
      String date = getValue(payload, ',', 3);
      String timein = getValue(payload, ',', 4);
      String studentName = getValue(payload, ',', 5);
      lcdClearAll();
      lcdPrintCentered(0, composite);
      if (studentName.length() > 0) lcdPrintLeft(1, studentName);
      lcdPrintLeft(2, "Date:" + date);
      lcdPrintLeft(3, "In:" + timein);
      lastProcessedUID = str_uid; lastProcessedTS = millis();
      delay(2400);
      lcdClearAll();
      return;
    }

    if (atc_Info == "TO_Successful") {
      toneTO();
      // payload: OK,TO_Successful,Composite,Date,TimeIn,TimeOut,StudentName
      String composite = getValue(payload, ',', 2);
      String date = getValue(payload, ',', 3);
      String tin = getValue(payload, ',', 4);
      String tout = getValue(payload, ',', 5);
      String studentName = getValue(payload, ',', 6);
      lcdClearAll();
      lcdPrintCentered(0, composite);
      if (studentName.length() > 0) lcdPrintLeft(1, studentName);
      lcdPrintLeft(2, "Date:" + date);
      lcdPrintLeft(3, "Out:" + tout);
      lastProcessedUID = str_uid; lastProcessedTS = millis();
      delay(2400);
      lcdClearAll();
      return;
    }

    if (atc_Info == "atcErr01") {
      lcdClearAll(); lcdPrintCentered(0, "Card not reg"); lcdPrintLeft(2, "Switch to REG"); toneError(); delay(1800); lcdClearAll(); return;
    }
    if (atc_Info == "atcErrGap") {
      String minsLeft = getValue(payload, ',', 2);
      lcdClearAll(); lcdPrintCentered(0, "Too early for TO"); lcdPrintLeft(2, "Wait (min): " + minsLeft); toneError(); delay(2200); lcdClearAll(); return;
    }
    if (atc_Info == "atcInf01") {
      lcdClearAll(); lcdPrintCentered(0, "Already done"); lcdPrintLeft(2, "Today"); toneError(); delay(1600); lcdClearAll(); return;
    }

    lcdClearAll(); lcdPrintCentered(1, "Unknown atc"); delay(1000); lcdClearAll(); return;
  }

  // bind handling
  if (str_modes == "bind") {
    String bindFlag = getValue(payload, ',', 1); bindFlag.trim();

    if (bindFlag == "BIND_Successful" || bindFlag == "BIND_Exists") {
      String studentName = getValue(payload, ',', 2);
      String sheetRow = getValue(payload, ',', 3);
      String composite = getValue(payload, ',', 4);
      String writeStatus = getValue(payload, ',', 5);
      toneInfo();
      lcdClearAll();
      lcdPrintCentered(0, "Card Bound");
      lcdPrintLeft(1, studentName);
      lcdPrintLeft(2, "Row:" + sheetRow);
      lcdPrintLeft(3, "Roll:" + composite);
      delay(2200);
      lcdClearAll();
      LAST_MASTER_UID = "";
      return;
    }

    if (bindFlag == "bindErr02") { lcdClearAll(); lcdPrintCentered(0, "Bind Error"); lcdPrintLeft(2, "UID exists"); toneError(); delay(1400); lcdClearAll(); return; }
    if (bindFlag == "bindErr03") { lcdClearAll(); lcdPrintCentered(0, "Bind Error"); lcdPrintLeft(1, "No rows ready"); toneError(); delay(1400); lcdClearAll(); return; }
    if (bindFlag == "bindErr05") { lcdClearAll(); lcdPrintCentered(0, "Bind Error"); lcdPrintLeft(1, "Fill Name/Class"); lcdPrintLeft(2, "Section in sheet"); lcdPrintLeft(3, "Then tap"); toneError(); delay(2000); lcdClearAll(); return; }
    if (bindFlag == "bindErr04") { lcdClearAll(); lcdPrintCentered(0, "Bind Error"); lcdPrintLeft(1, "No empty UID slots"); toneError(); delay(1400); lcdClearAll(); return; }
    if (bindFlag == "bindErr01") { lcdClearAll(); lcdPrintCentered(0, "Bind Error"); lcdPrintLeft(1, "Missing UID"); toneError(); delay(1200); lcdClearAll(); return; }

    lcdClearAll(); lcdPrintCentered(1, "Unknown bind"); delay(900); lcdClearAll(); return;
  }
}

// ---------- long-press handler ----------
void handleButtonLongPress() {
  int state = digitalRead(BTN_PIN);
  if (state == LOW) {
    if (!btnWasPressed) { btnWasPressed = true; btnPressStart = millis(); }
    else {
      unsigned long held = millis() - btnPressStart;
      if (held >= 3000) { // >=3s
        if (!waiting_for_master_capture) {
          if (allow_master_add_window) {
            waiting_for_master_capture = true;
            lcdClearAll();
            lcdPrintCentered(1, "ADD MASTER");
            lcdPrintLeft(2, "Tap new card");
            toneInfo(); delay(150);
          } else {
            lcdClearAll();
            lcdPrintCentered(1, "Not allowed");
            lcdPrintLeft(2, "Tap a master first");
            toneError(); delay(900); lcdClearAll();
          }
        }
        while (digitalRead(BTN_PIN) == LOW) delay(10); // wait release
        btnWasPressed = false;
      }
    }
  } else {
    btnWasPressed = false;
  }
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  lcd.init();
  lcd.backlight();
  lcdClearAll();

  prefs.begin(PREF_NAMESPACE, false);
  loadMastersFromPrefs();

  startupTone();
  lcdPrintCentered(0, "ESP32 RFID");
  lcdPrintCentered(1, "Attendance");
  lcdPrintCentered(2, "by Team Udaan");
  delay(1400);
  lcdClearAll();

  SPI.begin();
  mfrc522.PCD_Init();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  lcdPrintCentered(0, "Connecting WiFi");
  Serial.print("Connecting to "); Serial.println(ssid);

  int timeout = 40;
  while (WiFi.status() != WL_CONNECTED && timeout-- > 0) delay(500);

  lcdClearAll();
  if (WiFi.status() == WL_CONNECTED) {
    lcdPrintCentered(0, "WiFi connected");
    wifiConnectedTone();
    delay(900);
    lcdClearAll();
  } else {
    lcdPrintCentered(0, "WiFi failed");
    lcdPrintLeft(1, "Check network");
    toneError(); delay(1200);
    lcdClearAll();
  }
}

// ---------- main loop ----------
void loop() {
  // close master add window if expired
  if (allow_master_add_window && (millis() - allow_master_add_window_ts > ALLOW_MASTER_WINDOW_MS)) {
    allow_master_add_window = false;
  }

  handleButtonLongPress();

  static unsigned long lastIdleTs = 0;
  if (millis() - lastIdleTs > 800) {
    lastIdleTs = millis();
    if (!waiting_for_master_capture) {
      if (modes == "atc") {
        lcdPrintCentered(0, "ATTENDANCE MODE");
        lcdPrintLeft(2, "Tap card to sign");
        lcdPrintLeft(3, "or key chain");
      } else {
        lcdPrintCentered(0, "REGISTRATION MODE");
        lcdPrintLeft(1, "Fill Name/Class");
        lcdPrintLeft(2, "Section in sheet");
        lcdPrintLeft(3, "Then tap card");
      }
    }
  }

  if (mfrc522.PICC_IsNewCardPresent()) {
    if (!mfrc522.PICC_ReadCardSerial()) return;

    byteArray_to_string(mfrc522.uid.uidByte, mfrc522.uid.size, strUID);
    UID_Result = String(strUID);
    Serial.print("UID: "); Serial.println(UID_Result);

    // guard duplicate taps
    if (UID_Result == lastProcessedUID && (millis() - lastProcessedTS) < MIN_TAP_GUARD_MS) {
      Serial.println("Ignored rapid duplicate tap");
      mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
      delay(200);
      return;
    }

    // capture new master
    if (waiting_for_master_capture) {
      addMasterUID(UID_Result);
      waiting_for_master_capture = false;
      allow_master_add_window = false;
      lcdClearAll();
      lcdPrintCentered(0, "MASTER ADDED");
      lcdPrintLeft(2, "UID:");
      lcdPrintLeft(3, UID_Result.substring(0, min((int)UID_Result.length(), 16)));
      toneInfo();
      delay(1400);
      lcdClearAll();

      mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
      return;
    }

    // master tapped -> toggle mode & arm add-window
    if (isMasterUID(UID_Result)) {
      LAST_MASTER_UID = UID_Result;
      if (modes == "atc") modes = "reg"; else modes = "atc";

      allow_master_add_window = true;
      allow_master_add_window_ts = millis();

      lcdClearAll();
      modeSwitchTone();
      if (modes == "reg") {
        lcdPrintCentered(0, "REGISTRATION ON");
        lcdPrintLeft(2, "Tap master -> press");
        lcdPrintLeft(3, "long to add new");
      } else {
        lcdPrintCentered(0, "ATTENDANCE ON");
      }
      delay(1200);
      lcdClearAll();

      mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
      return;
    }

    // double-check guard
    if (UID_Result == lastProcessedUID && (millis() - lastProcessedTS) < MIN_TAP_GUARD_MS) {
      Serial.println("Ignored duplicate after master checks");
      mfrc522.PICC_HaltA(); mfrc522.PCD_StopCrypto1();
      delay(200);
      return;
    }

    // normal flows
    if (modes == "atc") {
      lcdClearAll();
      lcdPrintCentered(0, "ATTENDANCE");
      lcdPrintLeft(2, "Processing...");
      http_Req("atc", UID_Result);
    } else {
      lcdClearAll();
      lcdPrintCentered(0, "REGISTRATION");
      lcdPrintLeft(2, "Binding...");
      // set LAST_MASTER_UID only if a master was used earlier to toggle to reg
      http_Req("bind", UID_Result);
    }

    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    delay(700);
    lcdClearAll();
  }

  delay(120);
}