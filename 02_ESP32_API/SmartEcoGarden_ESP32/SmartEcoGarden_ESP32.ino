/*
 * SMART ECO GARDEN - ESP32 DEV MODULE 30 PIN | API ONLY
 * - WiFi AP + API ONLY (Dashboard Github Next.js tren laptop)
 * - Keypad 4x4 + servo SG90
 * - HC-SR04 + NeoPixel 12
 * - UART 2 chieu voi Arduino UNO
 * - Cam bien muc nuoc GPIO34 -> gui F<RAW>\n cho UNO
 *
 * TUONG THICH CHUONG TRINH UNO 2 BOM ACTIVE HIGH:
 *   ESP32 -> UNO: 'A', 'M', 'W', 'S' va 'F<RAW>\n'
 *   UNO -> ESP32: DATA,T,H,SOIL,LDR,RAIN,VIB,PUMP1,AUTO,ROOF_ANGLE,ROOF_TARGET,ROOF_COVER,PUMP2,FLOOD_ALARM
 *
 * THU VIEN CAN CO:
 *   WiFi, WebServer (ESP32 board package)
 *   Keypad, ESP32Servo, Adafruit_NeoPixel
 */

#include <WiFi.h>
#include <WebServer.h>
#include <Keypad.h>
#include <ESP32Servo.h>
#include <Adafruit_NeoPixel.h>
#include <LittleFS.h>

// =============== WIFI + MAT KHAU WEB ===============
const char* WIFI_NAME = "SmartEcoGarden";
const char* WIFI_PASS = "12345678";
const char* WEB_USER  = "admin";
const char* WEB_PASS  = "EcoHome2026!";
WebServer server(80);

// =============== KEYPAD 4x4 ===============
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {13, 14, 16, 17};
byte colPins[COLS] = {18, 19, 21, 22};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
String enteredPassword = "";
const String KEYPAD_PASSWORD = "1234";

// =============== SERVO CUA ===============
Servo doorServo;
const int SERVO_PIN = 23;
const int CLOSE_ANGLE = 0;
const int OPEN_ANGLE = 90;
const unsigned long AUTO_CLOSE_MS = 5000UL;
bool doorOpen = false;
unsigned long doorOpenTime = 0;

// =============== HC-SR04 ===============
const int TRIG_PIN = 25;
const int ECHO_PIN = 26; // Qua chia ap 5V -> 3.3V
float distanceCm = -1;
unsigned long lastDistanceRead = 0;

// =============== NEOPIXEL ===============
Adafruit_NeoPixel ring(12, 27, NEO_GRB + NEO_KHZ800);
uint32_t lastColor = 0xFFFFFFFF;

// =============== UART ESP32 <-> UNO ===============
// UNO D10 TX -> chia ap -> ESP32 GPIO32 RX
// ESP32 GPIO33 TX -> UNO D11 RX
const int UNO_RX_PIN = 32;
const int UNO_TX_PIN = 33;
String unoLine = "";
unsigned long lastUnoData = 0;
bool hasUnoData = false;
bool unoWasOnline = false;
unsigned long lastVibrationLogged = 0;

String temperature = "--";
String humidity = "--";
String soil = "--";
String lightValue = "--";
String rain = "--";
String vibration = "--";
String relayState = "--";  // Bom 1
String waterMode = "--";   // 1 = AUTO; 0 = MANUAL

// Extended roof and pump telemetry (requires updated UNO firmware).
// UNO roof angles: 180=RETRACTED (THU MAI), 90=COVERED (CHE MAI).
const int ROOF_RETRACT_ANGLE = 180;
const int ROOF_COVER_ANGLE = 90;
int roofAngle = -1;       // Last angle COMMANDED to SG90 (no physical feedback)
int roofTarget = -1;
bool roofCoverRequested = false;
bool roofDataValid = false;
bool floodPump2 = false;
bool floodAlarmUNO = false;
bool roofAutoMode = false; // gia tri ban dau; UNO gui trang thai thuc te
bool floodAutoMode = false; // gia tri ban dau; UNO gui trang thai thuc te
bool modesDataValid = false;

// =============== CAM BIEN MUC NUOC ===============
// S -> GPIO34; + -> 3V3; - -> GND chung
const int WATER_LEVEL_PIN = 34;
unsigned long lastFloodSend = 0;
int floodRaw = 0;

// =============== NHAT KY SU KIEN BEN VUNG ===============
// Ghi vao LittleFS: mat laptop/khong co Internet van tam luu su kien.
// Chi su kien thay doi, KHONG ghi moi lan doc cam bien vao flash.
const int EVENT_LIMIT = 80;
const int EVENT_BATCH = 40;
struct GardenEvent {
  String id;
  unsigned long tick;
  String kind;
  String detail;
  String source;
};
GardenEvent events[EVENT_LIMIT];
int eventCount = 0;
unsigned long localEventSeq = 0;
String bootKey;
bool fsReady = false;
unsigned long lostEventCount = 0;

bool authorized(); // khai bao truoc cho API /events

String safeField(String s) {
  s.replace("|", "-");
  s.replace("\n", " ");
  s.replace("\r", " ");
  if (s.length() > 75) s = s.substring(0, 75);
  return s;
}

String jsonEscape(const String &s) {
  String out;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') out += '\\';
    if ((unsigned char)c >= 32) out += c;
  }
  return out;
}

String eventLine(const GardenEvent &e) {
  return e.id + "|" + String(e.tick) + "|" + e.kind + "|" + e.detail + "|" + e.source;
}

bool parseEvent(String line, GardenEvent &e) {
  line.trim();
  int p[4], start = 0;
  for (int i=0;i<4;i++) {
    p[i]=line.indexOf('|', start);
    if (p[i]<0) return false;
    start=p[i]+1;
  }
  e.id=line.substring(0,p[0]);
  e.tick=(unsigned long)line.substring(p[0]+1,p[1]).toInt();
  e.kind=line.substring(p[1]+1,p[2]);
  e.detail=line.substring(p[2]+1,p[3]);
  e.source=line.substring(p[3]+1);
  return e.id.length()>0 && e.kind.length()>0;
}

void writeEventsFile() {
  if (!fsReady) return;
  File out = LittleFS.open("/events.new", "w");
  if (!out) return;
  for (int i=0;i<eventCount;i++) out.println(eventLine(events[i]));
  out.close();
  // Backup protects against power failure during queue compaction.
  LittleFS.remove("/events.old");
  if (LittleFS.exists("/events.log")) LittleFS.rename("/events.log", "/events.old");
  if (LittleFS.rename("/events.new", "/events.log")) LittleFS.remove("/events.old");
}

void initJournal() {
  fsReady = LittleFS.begin(false); // Khong tu dong format: uu tien giu nhat ky cu.
  if (!fsReady) {
    Serial.println("[NHAT KY] LITTLEFS CHUA SAN SANG; SU KIEN CHI LUU RAM");
  } else {
    // Recover if reset occurred while compacting.
    if (!LittleFS.exists("/events.log") && LittleFS.exists("/events.old"))
      LittleFS.rename("/events.old", "/events.log");
    File in = LittleFS.open("/events.log", "r");
    while (in && in.available()) {
      GardenEvent e;
      if (parseEvent(in.readStringUntil('\n'),e)) {
        if (eventCount == EVENT_LIMIT) {
          for (int i=1;i<eventCount;i++) events[i-1]=events[i];
          --eventCount;
        }
        events[eventCount++]=e;
      }
    }
    if (in) in.close();
  }
  bootKey = String((unsigned long)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX)
          + "-" + String(esp_random(),HEX);
}

void logEvent(const char *kind, String detail, const char *source) {
  GardenEvent e;
  e.id = bootKey + "-" + String(++localEventSeq);
  e.tick = millis();
  e.kind = safeField(kind);
  e.detail = safeField(detail);
  e.source = safeField(source);
  if (eventCount == EVENT_LIMIT) {
    // Het dung luong queue: bo su kien cu nhat; tang bo dem de canh bao.
    ++lostEventCount;
    for (int i=1;i<eventCount;i++) events[i-1]=events[i];
    eventCount--;
  }
  events[eventCount++]=e;
  if (fsReady) {
    if (eventCount == EVENT_LIMIT) writeEventsFile();
    else {
      File f=LittleFS.open("/events.log","a");
      if (f) {f.println(eventLine(e));f.close();}
    }
  }
  Serial.print("[EVENT] "); Serial.println(eventLine(e));
}

void handleEvents() {
  if (!authorized()) return;
  String out="{\"persistent\":";
  out+=(fsReady?"true":"false");
  out+=",\"queued\":"+String(eventCount);
  out+=",\"bootKey\":\""+jsonEscape(bootKey)+"\"";
  out+=",\"uptimeMs\":"+String(millis());
  out+=",\"lostEvents\":"+String(lostEventCount);
  out+=",\"events\":[";
  int limit = eventCount < EVENT_BATCH ? eventCount : EVENT_BATCH;
  for (int i=0;i<limit;i++) {
    if(i)out+=",";
    const GardenEvent &e=events[i];
    out+="{\"id\":\""+jsonEscape(e.id)+"\",\"kind\":\""+jsonEscape(e.kind)
        +"\",\"detail\":\""+jsonEscape(e.detail)+"\",\"source\":\""
        +jsonEscape(e.source)+"\",\"uptimeMs\":"+String(e.tick)+"}";
  }
  out+="]}";
  server.send(200,"application/json; charset=utf-8",out);
}

void handleEventsAck() {
  if (!authorized()) return;
  if (!server.hasArg("last")) {server.send(400,"text/plain","last required");return;}
  String id = server.arg("last");
  int found=-1;
  for(int i=0;i<eventCount;i++) if(events[i].id==id){found=i;break;}
  if(found<0){server.send(409,"text/plain","ID not found");return;}
  int rem = found+1;
  for (int i=rem;i<eventCount;i++) events[i-rem]=events[i];
  eventCount-=rem;
  writeEventsFile();
  server.send(200,"application/json","{\"ok\":true}");
}

// Chi ghi su kien xe khi thay doi on dinh, de tranh log khi HC-SR04 nhieu.
bool stableCar = false;
bool carInit = false;
bool candidateCar = false;
unsigned long candidateCarSince = 0;
void checkParkingEvent() {
  if(distanceCm < 2.0f) return; // Bo qua ket qua khong hop le.
  bool present = distanceCm <= 5.5f;
  if(!carInit){stableCar=present;candidateCar=present;carInit=true;return;}
  if(present != candidateCar){candidateCar=present;candidateCarSince=millis();}
  if(candidateCar!=stableCar && millis()-candidateCarSince>=1000UL){
    stableCar=candidateCar;
    logEvent(present?"CAR_IN":"CAR_OUT", "Khoang cach "+String(distanceCm,1)+"cm", "HC-SR04");
  }
}

// =============== KHONG CO DASHBOARD HTML TREN ESP32 ===============
// Giao dien duy nhat: Next.js GitHub tren laptop (cong 3000).

// =============== CAC HAM CUA ===============
void openDoor(const char* source) {
  doorServo.write(OPEN_ANGLE);
  doorOpen = true;
  doorOpenTime = millis();
  logEvent("DOOR_OPEN", "Nguon "+String(source), source);
  Serial.print("[CUA] MO TU ");
  Serial.println(source);
}

void closeDoor(const char* source) {
  doorServo.write(CLOSE_ANGLE);
  doorOpen = false;
  logEvent("DOOR_CLOSE", "Nguon "+String(source), source);
  Serial.print("[CUA] DONG TU ");
  Serial.println(source);
}

void checkKeypad() {
  char key = keypad.getKey();
  if (!key) return;

  // Khong in tung phim ra Serial de tranh lo mat khau.

  if (key == '*') {
    enteredPassword = "";
    Serial.println("[KEYPAD] XOA MAT KHAU");
    return;
  }
  if (key == '#') {
    if (enteredPassword == KEYPAD_PASSWORD) {
      openDoor("KEYPAD");
    } else {
      logEvent("ACCESS_DENIED", "Sai ma PIN (khong luu ma)", "KEYPAD");
      Serial.println("[KEYPAD] SAI MAT KHAU");
    }
    enteredPassword = "";
    return;
  }
  if (key >= '0' && key <= '9' && enteredPassword.length() < 8) {
    enteredPassword += key;
  }
}

// =============== HC-SR04 + RGB ===============
float readDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 25000UL);
  if (duration == 0) return -1;
  return duration * 0.0343f / 2.0f;
}

// NeoPixel CHI phu thuoc vao bai do xe, khong phu thuoc servo cua.
// Xe cach <= 5.5cm: DO, trong: XANH, do loi: VANG.
void updateLights() {
  uint32_t color;
  if (distanceCm < 2.0f) {
    color = ring.Color(255, 180, 0);    // Khong do duoc: vang
  } else if (distanceCm <= 5.5f) {
    color = ring.Color(255, 0, 0);      // Co xe: do
  } else {
    color = ring.Color(0, 255, 0);      // Cho trong: xanh
  }
  if (color != lastColor) {
    ring.fill(color);
    ring.show();
    lastColor = color;
  }
}

// =============== NHAN DU LIEU UNO ===============
void processUnoLine(String line) {
  line.trim();
  if (!line.startsWith("DATA,")) return;

  // Accept legacy 8 fields or the new 13-field format.
  // This allows flashing ESP32 first while UNO still has old firmware.
  String fields[15];
  int count = 0, start = 5;
  while (count < 15) {
    int comma = line.indexOf(',', start);
    if (comma < 0) {
      fields[count++] = line.substring(start);
      break;
    }
    fields[count++] = line.substring(start, comma);
    start = comma + 1;
  }
  if (count != 8 && count != 13 && count != 15) return;
  for (int i = 0; i < count; i++) fields[i].trim();

  // Record transitions only, not repeated telemetry samples.
  bool hadOld = hasUnoData;
  String oldRain = rain, oldPump1 = relayState, oldMode = waterMode;
  String oldVib = vibration;
  bool oldRoofValid = roofDataValid;
  int oldRoofTarget = roofTarget, oldRoofAngle = roofAngle;
  bool oldPump2 = floodPump2, oldFloodAlarm = floodAlarmUNO;
  bool oldRoofMode = roofAutoMode, oldFloodMode = floodAutoMode;

  temperature = fields[0];
  humidity    = fields[1];
  soil        = fields[2];
  lightValue  = fields[3];
  rain        = fields[4];
  vibration   = fields[5];
  relayState  = fields[6];
  waterMode   = fields[7];

  if (count == 13 || count == 15) {
    int a = fields[8].toInt();
    int t = fields[9].toInt();
    if (a >= 0 && a <= 180 && t >= 0 && t <= 180 &&
        (fields[10] == "0" || fields[10] == "1") &&
        (fields[11] == "0" || fields[11] == "1") &&
        (fields[12] == "0" || fields[12] == "1")) {
      roofAngle = a;
      roofTarget = t;
      roofCoverRequested = fields[10] == "1";
      floodPump2 = fields[11] == "1";
      floodAlarmUNO = fields[12] == "1";
      roofDataValid = true;
      // Old firmware sent 13 fields (all automatic).
      modesDataValid = count == 15 &&
          (fields[13] == "0" || fields[13] == "1") &&
          (fields[14] == "0" || fields[14] == "1");
      roofAutoMode = modesDataValid ? fields[13] == "1" : true;
      floodAutoMode = modesDataValid ? fields[14] == "1" : true;
    } else { roofDataValid = false; modesDataValid = false; }
  } else {
    roofDataValid = false;
    roofAngle = -1;
    roofTarget = -1;
    floodPump2 = false;
    floodAlarmUNO = false;
    roofAutoMode = true;
    floodAutoMode = true;
    modesDataValid = false;
  }

  if (hadOld) {
    if (oldPump1 != relayState)
      logEvent(relayState == "1" ? "PUMP1_ON" : "PUMP1_OFF", "Trang thai bom tuoi", "UNO");
    if (oldMode != waterMode)
      logEvent(waterMode == "1" ? "MODE_AUTO" : "MODE_MANUAL", "Che do tuoi", "UNO");
    if (oldVib != "1" && vibration == "1" &&
        (lastVibrationLogged == 0 || millis() - lastVibrationLogged >= 15000UL)) {
      logEvent("VIBRATION", "SW-420 kich hoat", "UNO");
      lastVibrationLogged = millis();
    }
    if ((oldRain.toInt()<900) != (rain.toInt()<900))
      logEvent(rain.toInt()<900 ? "RAIN_ON" : "RAIN_OFF", "Mua RAW="+rain, "UNO");
    if (roofDataValid && oldRoofValid) {
      if (oldRoofTarget != roofTarget)
        logEvent(roofTarget == ROOF_COVER_ANGLE ? "ROOF_COVER" : "ROOF_RETRACT", "Goc dich "+String(roofTarget), "UNO");
      if (oldRoofAngle != oldRoofTarget && roofAngle == roofTarget)
        logEvent("ROOF_REACHED", "Goc lenh "+String(roofAngle), "UNO");
      if (oldRoofMode != roofAutoMode)
        logEvent(roofAutoMode ? "ROOF_AUTO" : "ROOF_MANUAL", "Chuyen che do mai che", "UNO");
      if (oldFloodMode != floodAutoMode)
        logEvent(floodAutoMode ? "FLOOD_AUTO" : "FLOOD_MANUAL", "Chuyen che do chong ngap", "UNO");
      if (oldPump2 != floodPump2)
        logEvent(floodPump2 ? "PUMP2_ON" : "PUMP2_OFF", "Trang thai bom chong ngap", "UNO");
      if (oldFloodAlarm != floodAlarmUNO)
        logEvent(floodAlarmUNO ? "FLOOD_ON" : "FLOOD_OFF", "Muc nuoc RAW="+String(floodRaw), "UNO");
    }
  }
  lastUnoData = millis();
  hasUnoData = true;
  Serial.print("[UNO] ");
  Serial.println(line);
}

void receiveUno() {
  while (Serial2.available()) {
    char c = (char)Serial2.read();
    if (c == '\r') continue;
    if (c == '\n') {
      processUnoLine(unoLine);
      unoLine = "";
    } else if (unoLine.length() < 110) {
      unoLine += c;
    } else {
      unoLine = "";
    }
  }
}

bool unoConnected() {
  return hasUnoData && (millis() - lastUnoData < 8000UL);
}

void sendWaterCommand(char cmd) {
  Serial2.write((uint8_t)cmd);
  logEvent("WATER_CMD", "Lenh "+String(cmd)+" da gui UNO", "WEB");
  Serial.print("[ESP32 -> UNO] LENH: ");
  Serial.println(cmd);
}

// =============== MUC NUOC GPIO34 ===============
// Lay trung vi 7 mau de giam anh huong cua xung nhieu don le.
int readFloodMedian() {
  const int N = 7;
  int v[N];
  (void)analogRead(WATER_LEVEL_PIN);  // Bo mau dau tien
  for (int i = 0; i < N; i++) {
    v[i] = analogRead(WATER_LEVEL_PIN);
    delayMicroseconds(350);
  }
  for (int i = 1; i < N; i++) {
    int key = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > key) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = key;
  }
  return v[N / 2];
}

void sendFloodToUno() {
  floodRaw = readFloodMedian();
  // Phai dung dung dinh dang UNO dang cho: F<RAW>\n
  Serial2.print('F');
  Serial2.println(floodRaw);

  Serial.print("[MUC NUOC GUI UNO] ");
  Serial.println(floodRaw);
}

// =============== XAC THUC + CAC DUONG WEB ===============
bool authorized() {
  if (server.authenticate(WEB_USER, WEB_PASS)) return true;
  server.requestAuthentication();
  return false;
}

void handleData() {
  if (!authorized()) return;
  String json;
  json.reserve(950);
  json += "{\"unoConnected\":";
  json += (unoConnected() ? "true" : "false");
  json += ",\"doorOpen\":";
  json += (doorOpen ? "true" : "false");
  json += ",\"distance\":";
  json += String(distanceCm, 1);
  json += ",\"carDetected\":";
  json += ((distanceCm >= 2.0f && distanceCm <= 5.5f) ? "true" : "false");
  json += ",\"floodRaw\":";
  json += String(floodRaw);
  json += ",\"temperature\":\"" + temperature + "\"";
  json += ",\"humidity\":\"" + humidity + "\"";
  json += ",\"soil\":\"" + soil + "\"";
  json += ",\"light\":\"" + lightValue + "\"";
  json += ",\"rain\":\"" + rain + "\"";
  json += ",\"vibration\":\"" + vibration + "\"";
  json += ",\"relayState\":\"" + relayState + "\"";
  json += ",\"waterPump\":\"" + relayState + "\"";
  json += ",\"waterMode\":\"" + waterMode + "\"";
  json += ",\"roofWeather\":\"";
  json += (unoConnected() ? (rain.toInt() < 900 ? "RAIN" : "DRY") : "UNKNOWN");
  json += "\"";
  // The angle is a commanded setpoint, NOT a measured servo position.
  bool roofLive = unoConnected() && roofDataValid;
  json += ",\"roofAngleReported\":";
  json += (roofLive ? "true" : "false");
  json += ",\"roofAngle\":";
  json += String(roofLive ? roofAngle : -1);
  json += ",\"roofTargetAngle\":";
  json += String(roofLive ? roofTarget : -1);
  json += ",\"roofCoverRequested\":";
  json += (roofLive && roofCoverRequested ? "true" : "false");
  json += ",\"roofState\":\"";
  if (!roofLive) json += "UNKNOWN";
  else if (roofAngle != roofTarget) json += "MOVING";
  else if (roofTarget == ROOF_COVER_ANGLE) json += "COVERED";
  else if (roofTarget == ROOF_RETRACT_ANGLE) json += "RETRACTED";
  else json += "UNKNOWN";
  json += "\"";
  json += ",\"floodPump\":";
  json += (roofLive && floodPump2 ? "true" : "false");
  json += ",\"floodAlarm\":";
  json += (roofLive && floodAlarmUNO ? "true" : "false");
  json += ",\"roofAutoMode\":";
  json += (roofLive && roofAutoMode ? "true" : "false");
  json += ",\"floodAutoMode\":";
  json += (roofLive && floodAutoMode ? "true" : "false");
  json += ",\"manualModesReported\":";
  json += (roofLive && modesDataValid ? "true" : "false");
  json += ",\"eventQueue\":" + String(eventCount);
  json += ",\"lostEvents\":" + String(lostEventCount);
  json += "}";
  server.send(200, "application/json; charset=utf-8", json);
}

// Tra ve JSON cho Backend Next.js/FastAPI, khong redirect ve ESP32.
void sendCommandResponse(const char* command) {
  String out = "{\"ok\":true,\"command\":\"" + String(command) + "\"}";
  server.send(200, "application/json; charset=utf-8", out);
}

void handleDoorOpen() {
  if (!authorized()) return;
  openDoor("GITHUB_DASHBOARD");
  sendCommandResponse("door/open");
}
void handleDoorClose() {
  if (!authorized()) return;
  closeDoor("GITHUB_DASHBOARD");
  sendCommandResponse("door/close");
}
void handleSubsystemCommand(char cmd, const char* name) {
  if (!authorized()) return;
  if (!unoConnected() || !modesDataValid) {
    server.send(503, "application/json",
                "{\"ok\":false,\"error\":\"UNO chua ho tro lenh moi\"}");
    return;
  }
  bool isRoofAction = cmd == 'O' || cmd == 'C';
  bool isFloodAction = cmd == 'P' || cmd == 'Q';
  if ((isRoofAction && roofAutoMode) || (isFloodAction && floodAutoMode)) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"Can chuyen sang MANUAL\"}");
    return;
  }
  if (cmd == 'P' && floodRaw <= 100) {
    server.send(409, "application/json",
                "{\"ok\":false,\"error\":\"Muc nuoc qua thap\"}");
    return;
  }
  Serial2.write((uint8_t)cmd);
  logEvent("SYSTEM_CMD", "Da gui lenh " + String(name), "DASHBOARD");
  sendCommandResponse(name);
}

void handleWater(char cmd) {
  if (!authorized()) return;
  // Khong bao "bom da bat": day chi la lenh UART gui UNO.
  sendWaterCommand(cmd);
  sendCommandResponse("water/command_sent");
}

// =============== SETUP ===============
void setup() {
  Serial.begin(115200);
  initJournal();
  logEvent("ESP_BOOT", "Smart Eco Garden restart", "ESP32");
  Serial2.begin(9600, SERIAL_8N1, UNO_RX_PIN, UNO_TX_PIN);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  pinMode(WATER_LEVEL_PIN, INPUT);
  analogReadResolution(12);
  floodRaw = readFloodMedian();

  ring.begin();
  ring.setBrightness(5);  // Giu do sang thap nhu ban da test
  ring.clear();
  ring.show();

  doorServo.setPeriodHertz(50);
  doorServo.attach(SERVO_PIN, 500, 2400);
  doorServo.write(CLOSE_ANGLE);
  doorOpen = false;
  doorOpenTime = millis();

  WiFi.mode(WIFI_AP);
  bool apOK = WiFi.softAP(WIFI_NAME, WIFI_PASS);

  // 192.168.4.1 chi la may API. Khong host Dashboard HTML.
  server.on("/", HTTP_GET, []() {
    if (!authorized()) return;
    server.send(200, "application/json; charset=utf-8",
      "{\"system\":\"Smart Eco Garden\",\"role\":\"API_ONLY\",\"dashboard\":\"Laptop_IP:3000\"}");
  });
  server.on("/health", HTTP_GET, []() {
    if (!authorized()) return;
    server.send(200, "application/json; charset=utf-8",
      String("{\"ok\":true,\"unoConnected\":") + (unoConnected()?"true":"false") +
      ",\"eventQueue\":" + String(eventCount) + "}");
  });
  server.on("/data", HTTP_GET, handleData);
  server.on("/events", HTTP_GET, handleEvents);
  server.on("/events/ack", HTTP_POST, handleEventsAck);
  // Chi POST moi thay doi thiet bi, tranh link GET vo tinh kich hoat.
  server.on("/door/open", HTTP_POST, handleDoorOpen);
  server.on("/door/close", HTTP_POST, handleDoorClose);
  server.on("/water/auto", HTTP_POST, []() { handleWater('A'); });
  server.on("/water/manual", HTTP_POST, []() { handleWater('M'); });
  server.on("/water/on", HTTP_POST, []() { handleWater('W'); });
  server.on("/water/off", HTTP_POST, []() { handleWater('S'); });
  // New independent controls. Only POST commands; device remains in charge of safety.
  server.on("/roof/auto", HTTP_POST, []() { handleSubsystemCommand('R', "roof/auto"); });
  server.on("/roof/manual", HTTP_POST, []() { handleSubsystemCommand('H', "roof/manual"); });
  server.on("/roof/open", HTTP_POST, []() { handleSubsystemCommand('O', "roof/open"); });
  server.on("/roof/close", HTTP_POST, []() { handleSubsystemCommand('C', "roof/close"); });
  server.on("/flood/auto", HTTP_POST, []() { handleSubsystemCommand('B', "flood/auto"); });
  server.on("/flood/manual", HTTP_POST, []() { handleSubsystemCommand('N', "flood/manual"); });
  server.on("/flood/on", HTTP_POST, []() { handleSubsystemCommand('P', "flood/on"); });
  server.on("/flood/off", HTTP_POST, []() { handleSubsystemCommand('Q', "flood/off"); });
  server.onNotFound([]() {
    if (!authorized()) return;
    server.send(404, "text/plain", "404 Not Found");
  });
  server.begin();
  updateLights();

  Serial.println("====================================");
  Serial.println("ESP32 SMART ECO GARDEN - API ONLY");
  Serial.print("WiFi AP: "); Serial.println(WIFI_NAME);
  Serial.print("AP STATUS: "); Serial.println(apOK ? "OK" : "ERROR");
  Serial.print("API: http://"); Serial.println(WiFi.softAPIP());
  Serial.println("GITHUB DASHBOARD: http://IP_LAPTOP:3000");
  Serial.println("UART UNO: RX32 TX33 9600");
  Serial.println("WATER LEVEL: GPIO34 3V3");
  Serial.println("ROOF SERVO: UNO D4, RAIN: UNO A2");
  Serial.println("UART DATA: 8 old + 5 extended fields");
  Serial.println("====================================");
}

// =============== LOOP ===============
void loop() {
  server.handleClient();
  receiveUno();
  checkKeypad();

  bool unoNow = unoConnected();
  if (unoNow != unoWasOnline) {
    if (!unoNow && unoWasOnline)
      logEvent("UNO_OFFLINE", "Khong nhan DATA trong 8 giay", "UART");
    if (unoNow && hasUnoData && unoWasOnline == false)
      logEvent("UNO_ONLINE", "UART da ket noi", "UART");
    unoWasOnline = unoNow;
  }

  // Lay millis SAU cac ham co the mo cua.
  // Tranh loi tran so khien servo dong ngay sau khi vua mo.
  unsigned long nowMs = millis();

  if (doorOpen && (nowMs - doorOpenTime >= AUTO_CLOSE_MS)) {
    closeDoor("AUTO 5 GIAY");
    updateLights();
  }

  if (nowMs - lastDistanceRead >= 250UL) {
    lastDistanceRead = nowMs;
    distanceCm = readDistance();
    updateLights();
    checkParkingEvent();
  }

  if (nowMs - lastFloodSend >= 1000UL) {
    lastFloodSend = nowMs;
    sendFloodToUno();
  }
}
