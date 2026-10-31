/*
 * SMART ECO GARDEN - ARDUINO UNO R3 | BAN NOP BAO CAO
 * Water pump D3, flood pump D5: ACTIVE HIGH (tested)
 * Roof SG90: D4; rain sensor A2
 * Red D6, green D8, yellow D9
 * ESP32 UART: UNO RX D11 <- ESP32 TX33
 *             UNO TX D10 -> 5V/3.3V divider -> ESP32 RX32
 * IMPORTANT: adapter pumps/roof separate from USB; COMMON GND.
 */
#include <Wire.h>
#include <DHT.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <SoftwareSerial.h>
#include <Servo.h>
#include <stdlib.h>
#include <string.h>

#define DHT_PIN 7
#define SOIL_PIN A0
#define LIGHT_PIN A1
#define RAIN_PIN A2
#define VIB_PIN 2
#define RELAY_WATER_PIN 3
#define ROOF_SERVO_PIN 4
#define RELAY_FLOOD_PIN 5
#define LED_RED 6
#define LED_GREEN 8
#define LED_YELLOW 9
#define RELAY_ON HIGH
#define RELAY_OFF LOW

SoftwareSerial espLink(11, 10); // RX, TX; 9600 baud
DHT dht(DHT_PIN, DHT11);
RTC_DS1307 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo roofServo;

const int SOIL_DRY = 850, SOIL_WET = 650, RAIN_DRY = 900;
const int FLOOD_HIGH = 700, FLOOD_LOW = 100;
const unsigned long MAX_WATER_MS = 5000UL;
const unsigned long MAX_FLOOD_MS = 30000UL;
const unsigned long FLOOD_STABLE_MS = 3000UL;
const unsigned long FLOOD_LINK_TIMEOUT_MS = 4000UL;

const int ROOF_RETRACT = 0, ROOF_COVER = 180;
const unsigned long RAIN_CONFIRM_MS = 2000UL;
const unsigned long DRY_CONFIRM_MS = 10000UL;
const unsigned long ROOF_STEP_MS = 20UL;
int roofAngle = ROOF_RETRACT;
int roofTarget = ROOF_RETRACT;
bool roofCoverRequested = false;
// true = auto from rain sensor, false = manual from Dashboard
bool roofAutoMode = true;
bool floodAutoMode = true;
bool floodManualReady = true; // Manual pump: OFF must precede another ON
bool rainSampleWet = false;
unsigned long rainStateSince = 0, lastRoofStep = 0;

float temperature = NAN, humidity = NAN;
int soil = 0, lightValue = 0, rainValue = 0, vibration = 0;
bool vibrationDetected = false;
bool autoMode = true, autoArmed = false, watering = false;
unsigned long waterStart = 0;

int floodRaw = 0;
bool floodHasData = false, floodPumping = false, floodArmed = true, floodAlarm = false;
unsigned long lastFloodPacket = 0, floodStart = 0;
bool floodHighCandidate = false, floodLowCandidate = false;
unsigned long floodHighSince = 0, floodLowSince = 0;
bool receivingFlood = false;
char floodDigits[6];
byte floodDigitCount = 0;

bool rtcOK = false;
unsigned long lastRead = 0, lastLCD = 0, lastPage = 0;
byte page = 0;

void updateLEDs() {
  bool linkOK = floodHasData &&
    millis() - lastFloodPacket <= FLOOD_LINK_TIMEOUT_MS;
  digitalWrite(LED_RED, linkOK && floodAlarm ? HIGH : LOW);
  digitalWrite(LED_GREEN, linkOK && !floodAlarm ? HIGH : LOW);
  digitalWrite(LED_YELLOW, watering ? HIGH : LOW);
}

void stopWatering() {
  digitalWrite(RELAY_WATER_PIN, RELAY_OFF);
  if (watering) Serial.println(F("[BOM1] TAT"));
  watering = false;
  updateLEDs();
}

void startWatering() {
  if (watering || rainValue < RAIN_DRY) return;
  digitalWrite(RELAY_WATER_PIN, RELAY_ON);
  watering = true;
  waterStart = millis();
  updateLEDs();
  Serial.println(F("[BOM1] BAT"));
}

void stopFloodPump() {
  digitalWrite(RELAY_FLOOD_PIN, RELAY_OFF);
  if (floodPumping) Serial.println(F("[BOM2] TAT"));
  floodPumping = false;
}

void startFloodPump() {
  if (floodPumping || !floodArmed) return;
  floodArmed = false;
  digitalWrite(RELAY_FLOOD_PIN, RELAY_ON);
  floodStart = millis();
  floodPumping = true;
  Serial.println(F("[BOM2] BAT"));
}

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (!isnan(t)) temperature = t;
  if (!isnan(h)) humidity = h;
  soil = analogRead(SOIL_PIN);
  lightValue = analogRead(LIGHT_PIN);
  rainValue = analogRead(RAIN_PIN);
  vibration = digitalRead(VIB_PIN);
}

void controlRoofWeather() {
  if (!roofAutoMode) return;
  unsigned long nowMs = millis();
  bool wet = rainValue < RAIN_DRY;
  if (wet != rainSampleWet) {
    rainSampleWet = wet;
    rainStateSince = nowMs;
    Serial.println(wet ? F("[MAI] XAC NHAN MUA") : F("[MAI] XAC NHAN KHO"));
  }
  if (wet && !roofCoverRequested &&
      nowMs - rainStateSince >= RAIN_CONFIRM_MS) {
    roofCoverRequested = true;
    roofTarget = ROOF_COVER;
    Serial.println(F("[MAI] MUA -> CHE 180"));
  }
  if (!wet && roofCoverRequested &&
      nowMs - rainStateSince >= DRY_CONFIRM_MS) {
    roofCoverRequested = false;
    roofTarget = ROOF_RETRACT;
    Serial.println(F("[MAI] KHO -> THU 0"));
  }
}

void updateRoofServo() {
  unsigned long nowMs = millis();
  if (nowMs - lastRoofStep < ROOF_STEP_MS) return;
  lastRoofStep = nowMs;
  if (roofAngle < roofTarget) ++roofAngle;
  else if (roofAngle > roofTarget) --roofAngle;
  else return;
  roofServo.write(roofAngle);
}

void setRoofMode(bool automatic) {
  roofAutoMode = automatic;
  if (automatic) {
    // Evaluate weather again after changing from manual.
    rainSampleWet = rainValue < RAIN_DRY;
    rainStateSince = millis();
    roofCoverRequested = roofTarget == ROOF_COVER;
  }
  Serial.println(automatic ? F("[MAI] AUTO") : F("[MAI] MANUAL"));
}

void setFloodMode(bool automatic) {
  stopFloodPump(); // Changing mode never starts the pump.
  floodAutoMode = automatic;
  floodManualReady = true;
  floodHighCandidate = false;
  floodLowCandidate = false;
  // AUTO rearms after the water level is confirmed low.
  floodArmed = true;
  // If the water is already high, AUTO starts after a new 3s confirmation.
  Serial.println(automatic ? F("[NGAP] AUTO") : F("[NGAP] MANUAL"));
}

void manualFloodOn() {
  if (floodAutoMode || !floodManualReady || floodPumping) return;
  // No dry running and no pumping with a lost water sensor link.
  if (!floodHasData || millis() - lastFloodPacket > FLOOD_LINK_TIMEOUT_MS ||
      floodRaw <= FLOOD_LOW) {
    Serial.println(F("[NGAP] CHAN LENH: KHONG DU NUOC / MAT CAM BIEN"));
    return;
  }
  floodManualReady = false;
  // startFloodPump() uses floodArmed in AUTO; drive relay safely here.
  digitalWrite(RELAY_FLOOD_PIN, RELAY_ON);
  floodPumping = true;
  floodStart = millis();
  Serial.println(F("[NGAP] BOM MANUAL BAT"));
}

void controlWatering() {
  if (rainValue < RAIN_DRY) stopWatering();
  if (!autoMode) return;
  if (soil <= SOIL_WET) {
    stopWatering();
    autoArmed = true;
  }
  if (autoArmed && !watering && soil >= SOIL_DRY && rainValue >= RAIN_DRY) {
    startWatering();
    autoArmed = false;
  }
}

void acceptFloodRaw(int raw) {
  if (raw < 0 || raw > 4095) return;
  unsigned long nowMs = millis();
  floodRaw = raw;
  floodHasData = true;
  lastFloodPacket = nowMs;
  if (raw >= FLOOD_HIGH) {
    floodAlarm = true;
    floodLowCandidate = false;
    if (!floodHighCandidate) {
      floodHighCandidate = true;
      floodHighSince = nowMs;
    }
    if (floodAutoMode && floodArmed && !floodPumping &&
        nowMs - floodHighSince >= FLOOD_STABLE_MS) startFloodPump();
  } else if (raw <= FLOOD_LOW) {
    // Manual pump also stops immediately at low level to avoid dry running.
    if (!floodAutoMode && floodPumping) {stopFloodPump();floodManualReady=false;}
    floodHighCandidate = false;
    if (!floodLowCandidate) {
      floodLowCandidate = true;
      floodLowSince = nowMs;
    }
    if (nowMs - floodLowSince >= FLOOD_STABLE_MS) {
      if (floodAutoMode) stopFloodPump();
      floodArmed = true;
      floodAlarm = false;
    }
  } else {
    // Intermediate water level: keep pump and alarm state.
    floodHighCandidate = false;
    floodLowCandidate = false;
  }
  updateLEDs();
}

void controlFloodSafety() {
  unsigned long nowMs = millis();
  if (!floodHasData || nowMs - lastFloodPacket > FLOOD_LINK_TIMEOUT_MS) {
    if (floodPumping) stopFloodPump();
    floodManualReady = false;
    floodHighCandidate = false;
    floodLowCandidate = false;
    updateLEDs();
    return;
  }
  if (floodPumping && nowMs - floodStart >= MAX_FLOOD_MS) {
    stopFloodPump();
    floodManualReady = false;
    Serial.println(F("[BOM2] NGAT AN TOAN 30S"));
    // Only re-arm after raw <= 100 stable 3 seconds.
  }
}

void receiveESP32() {
  while (espLink.available()) {
    char c = (char)espLink.read();
    if (receivingFlood) {
      if (c >= '0' && c <= '9') {
        if (floodDigitCount < sizeof(floodDigits) - 1) {
          floodDigits[floodDigitCount++] = c;
        } else {
          receivingFlood = false;
          floodDigitCount = 0;
        }
      } else if (c == '\n') {
        if (floodDigitCount > 0) {
          floodDigits[floodDigitCount] = '\0';
          acceptFloodRaw(atoi(floodDigits));
        }
        receivingFlood = false;
        floodDigitCount = 0;
      } else if (c != '\r') {
        receivingFlood = false;
        floodDigitCount = 0;
      }
      continue;
    }
    switch (c) {
      case 'F': receivingFlood = true; floodDigitCount = 0; break;
      case 'T': Serial.println(F("[UART] TEST")); break;
      case 'A':
        stopWatering();
        autoMode = true;
        autoArmed = soil <= SOIL_WET;
        Serial.println(F("[TUOI] AUTO"));
        break;
      case 'M':
        stopWatering();
        autoMode = false;
        Serial.println(F("[TUOI] MANUAL"));
        break;
      case 'W':
        if (!autoMode && rainValue >= RAIN_DRY) startWatering();
        else Serial.println(F("[TUOI] LENH BI CHAN"));
        break;
      case 'S':
        stopWatering();
        autoArmed = false;
        break;
      // Roof commands: auto/manual/open/close
      case 'R': setRoofMode(true); break;
      case 'H': setRoofMode(false); break;
      case 'O':
        if (!roofAutoMode) {roofTarget=ROOF_COVER;roofCoverRequested=true;}
        break;
      case 'C':
        if (!roofAutoMode) {roofTarget=ROOF_RETRACT;roofCoverRequested=false;}
        break;
      // Flood pump commands: auto/manual/on/off
      case 'B': setFloodMode(true); break;
      case 'N': setFloodMode(false); break;
      case 'P': manualFloodOn(); break;
      case 'Q':
        if (!floodAutoMode) {stopFloodPump();floodManualReady=true;}
        break;
    }
  }
}

// Legacy first 13 fields are unchanged; append roof/flood modes.
// DATA,T,H,SOIL,LIGHT,RAIN,VIB,PUMP1,AUTO,ROOF_ANGLE,
//      ROOF_TARGET,ROOF_COVER,PUMP2,FLOOD_ALARM,ROOF_AUTO,FLOOD_AUTO\n
void sendSensors() {
  espLink.print(F("DATA,"));
  if (isnan(temperature)) espLink.print(F("nan"));
  else espLink.print(temperature, 1);
  espLink.print(',');
  if (isnan(humidity)) espLink.print(F("nan"));
  else espLink.print(humidity, 1);
  espLink.print(','); espLink.print(soil);
  espLink.print(','); espLink.print(lightValue);
  espLink.print(','); espLink.print(rainValue);
  espLink.print(','); espLink.print(vibrationDetected ? 1 : vibration);
  espLink.print(','); espLink.print(watering ? 1 : 0);
  espLink.print(','); espLink.print(autoMode ? 1 : 0);
  espLink.print(','); espLink.print(roofAngle);
  espLink.print(','); espLink.print(roofTarget);
  espLink.print(','); espLink.print(roofCoverRequested ? 1 : 0);
  espLink.print(','); espLink.print(floodPumping ? 1 : 0);
  espLink.print(','); espLink.print(floodAlarm ? 1 : 0);
  espLink.print(','); espLink.print(roofAutoMode ? 1 : 0);
  espLink.print(','); espLink.println(floodAutoMode ? 1 : 0);
}

void showLine(byte row, const char *s) {
  lcd.setCursor(0, row);
  lcd.print(F("                "));
  lcd.setCursor(0, row);
  lcd.print(s);
}

void displayPage() {
  char l1[17], l2[17];
  l1[0] = '\0'; l2[0] = '\0';
  switch (page) {
    case 0:
      if (rtcOK) {
        DateTime n = rtc.now();
        snprintf(l1, sizeof(l1), "Ngay %02d/%02d/%04d", n.day(), n.month(), n.year());
        snprintf(l2, sizeof(l2), "Gio %02d:%02d:%02d", n.hour(), n.minute(), n.second());
      } else { strcpy(l1, "RTC ERROR"); strcpy(l2, "KIEM TRA I2C"); }
      break;
    case 1:
      if (isnan(temperature) || isnan(humidity)) {
        strcpy(l1, "DHT11 ERROR"); strcpy(l2, "KIEM TRA D7");
      } else {
        snprintf(l1, sizeof(l1), "Nhiet do: %d C", (int)temperature);
        snprintf(l2, sizeof(l2), "Do am KK: %d%%", (int)humidity);
      }
      break;
    case 2: strcpy(l1, "DO AM DAT"); snprintf(l2, sizeof(l2), "ADC: %d", soil); break;
    case 3: strcpy(l1, "ANH SANG"); snprintf(l2, sizeof(l2), "ADC: %d", lightValue); break;
    case 4: strcpy(l1, "CAM BIEN MUA"); snprintf(l2, sizeof(l2), "ADC: %d", rainValue); break;
    case 5:
      strcpy(l1, "CAM BIEN RUNG");
      snprintf(l2, sizeof(l2), "DO:%d RUNG:%s", vibration, vibrationDetected ? "CO" : "KHONG");
      break;
    case 6:
      strcpy(l1, "TUOI CAY");
      strcpy(l2, watering ? "RELAY 1: BAT" : (rainValue < RAIN_DRY ? "CO MUA - TAT" : "RELAY 1: TAT"));
      break;
    case 7: strcpy(l1, "CHE DO TUOI"); strcpy(l2, autoMode ? "TU DONG" : "THU CONG"); break;
    case 8:
      strcpy(l1, "MUC NUOC");
      if (floodHasData && millis() - lastFloodPacket <= FLOOD_LINK_TIMEOUT_MS)
        snprintf(l2, sizeof(l2), "RAW: %d", floodRaw);
      else strcpy(l2, "CHO ESP32...");
      break;
    case 9:
      strcpy(l1, "CHONG NGAP");
      snprintf(l2, sizeof(l2), "%s %s", floodAutoMode ? "AUTO" : "MANUAL",
               floodPumping ? "BAT" : "TAT");
      break;
    case 10:
      strcpy(l1, "MAI CHE NHA KINH");
      if (roofAngle != roofTarget) snprintf(l2, sizeof(l2), "QUAY %s: %d",
        roofAutoMode ? "AUTO" : "TAY", roofAngle);
      else snprintf(l2, sizeof(l2), "GOC LENH: %d", roofAngle);
      break;
  }
  showLine(0, l1);
  showLine(1, l2);
}

void setup() {
  // Set relay pins OFF before changing to outputs (ACTIVE HIGH boards).
  digitalWrite(RELAY_WATER_PIN, RELAY_OFF);
  digitalWrite(RELAY_FLOOD_PIN, RELAY_OFF);
  pinMode(RELAY_WATER_PIN, OUTPUT);
  pinMode(RELAY_FLOOD_PIN, OUTPUT);
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_YELLOW, LOW);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(VIB_PIN, INPUT);

  Serial.begin(9600);
  espLink.begin(9600);
  Wire.begin();
  dht.begin();
  lcd.init();
  lcd.backlight();
  lcd.clear();
  showLine(0, "SMART ECO GARDEN");
  showLine(1, "DANG KHOI DONG");
  rtcOK = rtc.begin();
  readSensors();

  // Servo has no true feedback sensor. roofAngle is the commanded angle.
  roofAngle = ROOF_RETRACT;
  roofTarget = ROOF_RETRACT;
  roofCoverRequested = false;
  roofAutoMode = true;
  floodAutoMode = true;
  floodManualReady = true;
  roofServo.attach(ROOF_SERVO_PIN);
  roofServo.write(roofAngle);
  rainSampleWet = rainValue < RAIN_DRY;
  rainStateSince = millis();

  stopWatering();
  stopFloodPump();
  autoMode = true;
  autoArmed = false;
  floodHasData = false;
  floodArmed = true;
  floodAlarm = false;
  floodHighCandidate = false;
  floodLowCandidate = false;
  updateLEDs();
  lastRead = lastLCD = lastPage = lastRoofStep = millis();

  Serial.println(F("SMART ECO GARDEN - UNO R3"));
  Serial.println(F("UART 9600 / ROOF D4 / PUMP D3,D5"));
  Serial.println(F("RAIN A2 / ROOF 0-180 / LED D6,D8,D9"));
}

void loop() {
  receiveESP32();
  unsigned long nowMs = millis();
  updateRoofServo();

  vibration = digitalRead(VIB_PIN);
  if (vibration == HIGH) vibrationDetected = true;
  if (watering && nowMs - waterStart >= MAX_WATER_MS) stopWatering();
  controlFloodSafety();
  updateLEDs();

  if (nowMs - lastRead >= 2000UL) {
    lastRead = nowMs;
    readSensors();
    controlRoofWeather();
    controlWatering();
    sendSensors();
    Serial.print(F("[GARDEN] DAT=")); Serial.print(soil);
    Serial.print(F(" MUA=")); Serial.print(rainValue);
    Serial.print(F(" MAI=")); Serial.print(roofAngle);
    Serial.print('/'); Serial.print(roofTarget);
    Serial.print(F(" BOM1=")); Serial.print(watering ? F("ON") : F("OFF"));
    Serial.print(F(" BOM2=")); Serial.println(floodPumping ? F("ON") : F("OFF"));
    vibrationDetected = false;
  }
  if (nowMs - lastPage >= 3000UL) {
    lastPage = nowMs;
    page = (page + 1) % 11;
  }
  if (nowMs - lastLCD >= 1000UL) {
    lastLCD = nowMs;
    displayPage();
  }
}
