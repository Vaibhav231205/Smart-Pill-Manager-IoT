#include <Wire.h>
#include <U8g2lib.h>
#include <RTClib.h>
#include <ESP32Servo.h>
#include <WiFi.h>

// ---------------- Blynk Template Info ----------------
#define BLYNK_TEMPLATE_ID "TMPL3QIswxFV9"
#define BLYNK_TEMPLATE_NAME "Smart Pill Management"
#define BLYNK_AUTH_TOKEN "PRhX-H8Zfl_WrJzbcPyJcao_CXZSMmBO"

#include <BlynkSimpleEsp32.h>

// ---------------- WiFi ----------------
char ssid[] = "Vaibhav";
char pass[] = "12345678";
bool wifiConnected = false;

// ---------------- Hardware ----------------
U8G2_SH1106_128X64_NONAME_1_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
RTC_DS3231 rtc;   // DS3231 RTC
Servo pillServo;

#define BUTTON_PIN       0
#define HOUR_BUTTON_PIN  4
#define MIN_BUTTON_PIN   5
#define BUZZER_PIN       15
#define MOTOR_CTRL_PIN   2
#define SERVO_PIN        16

int pillCount = 5;

// ---------------- Multiple Alarms ----------------
struct Alarm { byte hour; byte minute; bool triggeredToday; };
const int MAX_ALARMS = 3;
Alarm alarms[MAX_ALARMS] = {{8,0,false},{14,0,false},{20,0,false}};
int currentAlarmIndex = 0;

// Modes
bool fillMode = false;
bool settingAlarm = false;
bool adjustingHours = true;

// Alert
unsigned long missedDoseTimeout = 60000UL;
bool alertActive = false;
bool doseMissed = false;
unsigned long alertStartTime = 0;

// Blink/buzzer
unsigned long lastBuzzerToggle = 0;
bool buzzerOn = false;
unsigned long lastBlinkToggle = 0;
bool blinkOn = false;

// Servo
bool dispensing = false;
unsigned long dispenseStart = 0;
const unsigned long dispenseDuration = 1000;
const int SERVO_DISPENSE_POS = 90;
const int SERVO_HOME_POS = 0;

// Auto-save for alarm
unsigned long lastAlarmAdjustTime = 0;

// Button struct
struct BtnState {
  uint8_t pin;
  bool lastStable;
  bool lastRaw;
  unsigned long lastDebounceTime;
  unsigned long lastPressedTime;
  bool longEventFired;
};
const unsigned long debounceDelay = 30;
const unsigned long longPressMs = 3000;
BtnState btnMain = { BUTTON_PIN, HIGH, HIGH, 0, 0, false };
BtnState btnHour = { HOUR_BUTTON_PIN, HIGH, HIGH, 0, 0, false };
BtnState btnMin  = { MIN_BUTTON_PIN, HIGH, HIGH, 0, 0, false };

// Daily reset
int lastDay = -1;

// Display throttling
unsigned long lastDisplayUpdate = 0;
const unsigned long displayInterval = 200;

// ---------------- Blynk Handlers ----------------
// Alarms
BLYNK_WRITE(V0) { int h = param.asInt(); if(h>=0 && h<24){ alarms[0].hour=h; alarms[0].triggeredToday=false; } }
BLYNK_WRITE(V1) { int m = param.asInt(); if(m>=0 && m<60){ alarms[0].minute=m; alarms[0].triggeredToday=false; } }
BLYNK_WRITE(V2) { int h = param.asInt(); if(h>=0 && h<24){ alarms[1].hour=h; alarms[1].triggeredToday=false; } }
BLYNK_WRITE(V3) { int m = param.asInt(); if(m>=0 && m<60){ alarms[1].minute=m; alarms[1].triggeredToday=false; } }
BLYNK_WRITE(V4) { int h = param.asInt(); if(h>=0 && h<24){ alarms[2].hour=h; alarms[2].triggeredToday=false; } }
BLYNK_WRITE(V5) { int m = param.asInt(); if(m>=0 && m<60){ alarms[2].minute=m; alarms[2].triggeredToday=false; } }

// Pill count from app
BLYNK_WRITE(V6) {
  int val = param.asInt();
  if(val >= 0) {
    pillCount = val;
    displayMessage("Pills Updated", String(pillCount).c_str(), 600);
  }
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(HOUR_BUTTON_PIN, INPUT_PULLUP);
  pinMode(MIN_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MOTOR_CTRL_PIN, OUTPUT);

  pillServo.attach(SERVO_PIN);
  pillServo.write(SERVO_HOME_POS);

  u8g2.begin();

  if(!rtc.begin()) { 
    displayError("RTC not found!"); 
    while(1); 
  }
  if(rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  lastDay = rtc.now().day();
  displayMessage("Pill Reminder", "Ready", 1000);

  setupWiFi();
}

// ---------------- Loop ----------------
void loop() {
  checkWiFi();
  if(wifiConnected) Blynk.run();

  DateTime now = rtc.now();

  if(now.day() != lastDay){
    for(int i=0;i<MAX_ALARMS;i++) alarms[i].triggeredToday=false;
    lastDay = now.day();
  }

  processButton(&btnMain);
  processButton(&btnHour);
  processButton(&btnMin);

  static bool lastMain=HIGH,lastHour=HIGH,lastMin=HIGH;

  if(lastMain==LOW && btnMain.lastStable==HIGH){
    unsigned long pressDur = millis()-btnMain.lastPressedTime;
    if(settingAlarm){
      adjustingHours = !adjustingHours;
    } else {
      if(pressDur>=2000) fillMode=!fillMode;
      else{
        if(fillMode){ 
          pillCount++; 
          Blynk.virtualWrite(V6, pillCount); 
          displayMessage("Pill Added","",600); 
        } else if(pillCount>0 && !dispensing) startDispense();
        else { displayMessage("No Pills","Refill Needed",1000); doubleBeep(); }
      }
    }
  }
  lastMain = btnMain.lastStable;

  if(btnHour.longEventFired){
    settingAlarm=true; btnHour.longEventFired=false; adjustingHours=true;
    lastAlarmAdjustTime=millis();
    displayMessage("SET ALARM", ("Editing #" + String(currentAlarmIndex+1)).c_str(), 600);
  }

  if(btnMin.longEventFired){
    btnMin.longEventFired=false;
    if(!settingAlarm){ settingAlarm=true; adjustingHours=true; }
    currentAlarmIndex=(currentAlarmIndex+1)%MAX_ALARMS;
    lastAlarmAdjustTime=millis();
    displayMessage("SET ALARM", ("Now #" + String(currentAlarmIndex+1)).c_str(), 500);
  }

  if(settingAlarm){
    bool buttonPressed=false;
    if(lastHour==LOW && btnHour.lastStable==HIGH){ alarms[currentAlarmIndex].hour=(alarms[currentAlarmIndex].hour+1)%24; buttonPressed=true; }
    lastHour=btnHour.lastStable;
    if(lastMin==LOW && btnMin.lastStable==HIGH){ alarms[currentAlarmIndex].minute=(alarms[currentAlarmIndex].minute+1)%60; buttonPressed=true; }
    lastMin=btnMin.lastStable;
    if(buttonPressed) lastAlarmAdjustTime=millis();
    if(millis()-lastAlarmAdjustTime>=3000){
      settingAlarm=false;
      alarms[currentAlarmIndex].triggeredToday=false;
      displayMessage(("Alarm #" + String(currentAlarmIndex+1) + " Saved").c_str(), "", 800);
    }
  }

  // ---------------- Alarm Check ----------------
  if(!fillMode && !settingAlarm){
    for(int i=0;i<MAX_ALARMS;i++){
      if(!alarms[i].triggeredToday && now.hour()==alarms[i].hour && now.minute()==alarms[i].minute && now.second()==0){
        alertActive=true; doseMissed=false; alertStartTime=millis();
        alarms[i].triggeredToday=true;

        if(wifiConnected){
          Blynk.logEvent("pill_reminder", "⏰ It's time to take your pill!");
        }
        break;
      }
    }
  }

  if(alertActive && !doseMissed){
    unsigned long m=millis();
    if(m-lastBuzzerToggle>=500){ lastBuzzerToggle=m; buzzerOn=!buzzerOn; if(buzzerOn) tone(BUZZER_PIN,1500); else noTone(BUZZER_PIN); digitalWrite(MOTOR_CTRL_PIN, buzzerOn?HIGH:LOW); }
    if(m-lastBlinkToggle>=500){ lastBlinkToggle=m; blinkOn=!blinkOn; }
    if(m-alertStartTime>=missedDoseTimeout){
      doseMissed=true; stopAlert(); displayMessage("Missed Dose","",1000);
      if(wifiConnected){ Blynk.logEvent("missed_dose", "⚠️ You missed your pill dose!"); }
      Blynk.virtualWrite(V8, 1);
    }
  }

  if(dispensing && millis()-dispenseStart>=dispenseDuration){
    pillServo.write(SERVO_HOME_POS); dispensing=false;
    if(pillCount>0) pillCount--;
    Blynk.virtualWrite(V6, pillCount);
    displayMessage("Pill Taken","",600);
    if(alertActive) stopAlert();
    if(wifiConnected) Blynk.virtualWrite(V7, 1);
  }

  if(millis()-lastDisplayUpdate>=displayInterval){
    lastDisplayUpdate=millis();
    if(fillMode) displayFillMode();
    else if(settingAlarm) displayAlarmSetting();
    else if(alertActive && !doseMissed) displayAlarmScreen(now, blinkOn);
    else displayStatus(now);
  }

  delay(5);
}

// ---------------- WiFi Functions ----------------
void setupWiFi(){ WiFi.mode(WIFI_STA); WiFi.begin(ssid, pass); Serial.println("Connecting to WiFi..."); }
void checkWiFi(){ if(WiFi.status()==WL_CONNECTED){ if(!wifiConnected){ wifiConnected=true; Serial.println("WiFi Connected!"); Blynk.config(BLYNK_AUTH_TOKEN); Blynk.connect(); } } else wifiConnected=false; }

// ---------------- Helper functions ----------------
void startDispense(){ pillServo.write(SERVO_DISPENSE_POS); dispensing=true; dispenseStart=millis(); }
void stopAlert(){ alertActive=false; doseMissed=false; noTone(BUZZER_PIN); digitalWrite(MOTOR_CTRL_PIN,LOW); buzzerOn=false; blinkOn=false; }
void doubleBeep(){ for(int i=0;i<2;i++){ tone(BUZZER_PIN,1500); delay(200); noTone(BUZZER_PIN); delay(200); } }
void processButton(BtnState* b){
  bool raw=digitalRead(b->pin);
  if(raw!=b->lastRaw){ b->lastDebounceTime=millis(); b->lastRaw=raw; }
  if((millis()-b->lastDebounceTime)>debounceDelay){
    if(raw!=b->lastStable){ b->lastStable=raw; if(raw==LOW){ b->lastPressedTime=millis(); b->longEventFired=false; } }
    else if(b->lastStable==LOW && !b->longEventFired && millis()-b->lastPressedTime>=longPressMs) b->longEventFired=true;
  }
}

// ---------------- Display Functions ----------------
void printTwoDigits(int num){ if(num<10) u8g2.print('0'); u8g2.print(num); }
void displayAlarmScreen(DateTime now,bool blinkState){ u8g2.firstPage(); do{ u8g2.setFont(u8g2_font_6x10_tf); u8g2.setCursor(0,12); u8g2.print("Take Your Pill!"); u8g2.setCursor(0,28); u8g2.print("Pills Left: "); u8g2.print(pillCount); if(blinkState){ u8g2.setCursor(0,44); u8g2.print("ALARM ACTIVE!"); } }while(u8g2.nextPage()); }
void displayFillMode(){ u8g2.firstPage(); do{ u8g2.setFont(u8g2_font_6x10_tf); u8g2.setCursor(0,12); u8g2.print("FILL MODE"); u8g2.setCursor(0,28); u8g2.print("Press Btn to add"); u8g2.setCursor(0,44); u8g2.print("Total: "); u8g2.print(pillCount); }while(u8g2.nextPage()); }
void displayAlarmSetting(){ DateTime now=rtc.now(); u8g2.firstPage(); do{ u8g2.setFont(u8g2_font_6x10_tf); u8g2.setCursor(0,8); u8g2.print("SET ALARM #"); u8g2.print(currentAlarmIndex+1); u8g2.setCursor(0,24); printTwoDigits(alarms[currentAlarmIndex].hour); u8g2.print(":"); printTwoDigits(alarms[currentAlarmIndex].minute); u8g2.setCursor(0,40); if(adjustingHours) u8g2.print("Adjusting HOURS"); else u8g2.print("Adjusting MINUTES"); u8g2.setCursor(0,56); u8g2.print("Now: "); printTwoDigits(now.hour()); u8g2.print(":"); printTwoDigits(now.minute()); u8g2.print(":"); printTwoDigits(now.second()); }while(u8g2.nextPage()); }
void displayStatus(DateTime now){ u8g2.firstPage(); do{ u8g2.setFont(u8g2_font_6x10_tf); u8g2.setCursor(0,10); u8g2.print("Time: "); printTwoDigits(now.hour()); u8g2.print(":"); printTwoDigits(now.minute()); u8g2.print(":"); printTwoDigits(now.second()); u8g2.setCursor(0,26); u8g2.print("Pills: "); u8g2.print(pillCount); int y=42; for(int i=0;i<MAX_ALARMS;i++){ u8g2.setCursor(0,y); u8g2.print("A"); u8g2.print(i+1); u8g2.print(": "); printTwoDigits(alarms[i].hour); u8g2.print(":"); printTwoDigits(alarms[i].minute); y+=12; } if(doseMissed){ u8g2.setCursor(0,60); u8g2.print("MISSED DOSE!"); } }while(u8g2.nextPage()); }
void displayMessage(const char* line1,const char* line2,unsigned long delayMs){ 
  unsigned long start=millis(); 
  while(millis()-start<delayMs){ 
    u8g2.firstPage(); 
    do{ 
      u8g2.setFont(u8g2_font_6x10_tf); 
      u8g2.setCursor(0,20); 
      u8g2.print(line1); 
      u8g2.setCursor(0,40); 
      u8g2.print(line2); 
    }while(u8g2.nextPage()); 
    delay(10); 
  }  
}
void displayError(const char* msg){ 
  u8g2.firstPage(); 
  do{ 
    u8g2.setFont(u8g2_font_6x10_tf); 
    u8g2.setCursor(0,20); 
    u8g2.print("ERROR:"); 
    u8g2.setCursor(0,40); 
    u8g2.print(msg); 
  }while(u8g2.nextPage()); 
}
