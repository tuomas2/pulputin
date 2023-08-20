// Copyright (C) 2023 Tuomas Airaksinen
// License: GPL. See GPL.txt for more info

#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <RTClib.h>

static const int BUTTON1_PIN = 39;
static const int BUTTON2_PIN = 41;
static const int BUTTON3_PIN = 43;
static const int BUTTON4_PIN = 45;
static const int BUTTON5_PIN = 47;
static const int BUTTON6_PIN = 49;
static const int BUTTON7_PIN = 51;
static const int BUTTON8_PIN = 53;

static const int WATER_LEVEL_PIN_GROUND = 31;
static const int WATER_LEVEL_PIN = 33;

static const int IN_MOISTURE1_PIN = A0;

static const int OUT_PUMP_PIN = 2; // PWM possible
static const int ALARM_PIN = 3;

static const int MOTION_PIN = 52;
static const int MOTION_GROUND_PIN = 50;


int moisture1Percent = 0;
bool waterLevel = false;
bool maxWaterLevel = false; 
bool motionSns = false;


// How many ml water has been pumped each hour
// Latest is first item.
// Once an hour last item is removed and each item is moved one forward.
uint16_t pumpStatistics[24]; 
uint16_t pumpedTotal = 0;

// EEPROM addresses
static const int EEPROM_PUMP_STATISTICS = 0;
static const int EEPROM_CONFIGURED = 48;
static const int EEPROM_PUMP_TOTAL = 49;
static const int EEPROM_LAST_HOUR_STARTED = 51;
static const int EEPROM_PUMP_STARTED = 55;
static const int EEPROM_IDLE_STARTED = 59;
static const int EEPROM_LAST_WET = 63;
static const int EEPROM_UNUSED1 = 67;
static const int EEPROM_STATS_CUR_DAY = 72;
static const int EEPROM_LAST = 72;

static const byte EEPROM_CHECKVALUE = 0b10101010;

static const unsigned long EPOCH_OFFSET = 1690180000;

// Times, in millisecond (since starting device)
unsigned long epochAtStart = 0;
unsigned long timeNow = 0;
DateTime dateTimeNow;

unsigned long lastHourStarted = 0;
unsigned long pumpStartedMs = 0;
unsigned long idleStartedMs = 0;
unsigned long lastWetMs = 0;
unsigned long forceStopStartedMs = 0;
unsigned long motionStopStartedMs = 0;

uint8_t statisticsCurrentDay = 0;

bool wasMotionStopped = false;
bool wasForceStopped = false;
bool wasWet = false;
bool pumpRunning = false;

unsigned long minutesAgo(unsigned long timestamp) { return (timeNow - timestamp) / 1000 / 60; }

static const int PUMP_WATER_SPEED = 133;  // Pump speed, ml per 100 seconds

// Convert millilitres to milliseconds and vice versa
unsigned long mlToMs(unsigned long millilitres) { return 100000 * millilitres / PUMP_WATER_SPEED; }
unsigned long msToMl(unsigned long milliseconds) { return milliseconds * PUMP_WATER_SPEED / 100000; }

static const unsigned long ONE_HOUR = 3600000;
static const unsigned long ONE_MINUTE = ONE_HOUR/60;

static const uint16_t CONTAINER_SIZE = 28000;  // Water container size in (ml)

static const int PUMP_PORTION = 100;       // Amount of water pumped at once (ml)
static const unsigned long PERIOD_TIME = 15*ONE_MINUTE; // Adjusted water amount is PUMP_PORTION / PERIOD_TIME.


static const unsigned long PUMP_TIME = mlToMs(PUMP_PORTION);
static const unsigned long IDLE_TIME = PERIOD_TIME - PUMP_TIME;

static const unsigned long WET_TIME = ONE_HOUR;
static const unsigned long FORCE_STOP_TIME = ONE_HOUR;
static const unsigned long MOTION_STOP_TIME = ONE_MINUTE * 15;


LiquidCrystal_I2C lcd(0x3F, 16, 2);
DS3231 rtc;

void initializePins() {
  pinMode(BUTTON1_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);
  pinMode(BUTTON3_PIN, INPUT_PULLUP);
  pinMode(BUTTON4_PIN, INPUT_PULLUP);
  pinMode(BUTTON5_PIN, INPUT_PULLUP);
  pinMode(BUTTON6_PIN, INPUT_PULLUP);
  pinMode(BUTTON7_PIN, INPUT_PULLUP);
  pinMode(BUTTON8_PIN, INPUT_PULLUP);
  pinMode(MOTION_PIN, INPUT);

  pinMode(IN_MOISTURE1_PIN, INPUT);
  pinMode(OUT_PUMP_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(ALARM_PIN, OUTPUT);

  pinMode(WATER_LEVEL_PIN_GROUND, OUTPUT);
  digitalWrite(WATER_LEVEL_PIN_GROUND, LOW);
  pinMode(MOTION_GROUND_PIN, OUTPUT);
  digitalWrite(MOTION_GROUND_PIN, LOW);

  pinMode(WATER_LEVEL_PIN, INPUT_PULLUP);
  
  digitalWrite(LED_BUILTIN, LOW);
  digitalWrite(ALARM_PIN, LOW);
  digitalWrite(OUT_PUMP_PIN, LOW);
}

void readEeprom() {
  if (eeprom_read_byte(EEPROM_CONFIGURED) != EEPROM_CHECKVALUE) {
    resetEEPROM();
  }
  for (int i = 0; i < 24; i++) {
    pumpStatistics[i] = eeprom_read_word(EEPROM_PUMP_STATISTICS + i*2);
  }
  
  pumpedTotal = eeprom_read_word(EEPROM_PUMP_TOTAL);
  lastHourStarted = eeprom_read_dword(EEPROM_LAST_HOUR_STARTED);
  pumpStartedMs = eeprom_read_dword(EEPROM_PUMP_STARTED);
  idleStartedMs = eeprom_read_dword(EEPROM_IDLE_STARTED);
  lastWetMs = eeprom_read_dword(EEPROM_LAST_WET);
  statisticsCurrentDay = eeprom_read_byte(EEPROM_STATS_CUR_DAY); 
}

void saveEeprom() {
  for (int i = 0; i < 24; i++) {
    eeprom_update_word(EEPROM_PUMP_STATISTICS + i*2, pumpStatistics[i]);
  }
  eeprom_update_word(EEPROM_PUMP_TOTAL, pumpedTotal);
  eeprom_update_dword(EEPROM_LAST_HOUR_STARTED, lastHourStarted);
  eeprom_update_dword(EEPROM_PUMP_STARTED, pumpStartedMs);
  eeprom_update_dword(EEPROM_IDLE_STARTED, idleStartedMs);
  eeprom_update_dword(EEPROM_LAST_WET, lastWetMs);
  eeprom_update_byte(EEPROM_STATS_CUR_DAY, statisticsCurrentDay);
}


void dayPassed() {
  for (int i = 23; i > 0; i--) {
    pumpStatistics[i] = pumpStatistics[i - 1];
  }
  pumpStatistics[0] = 0;
}

void resetEEPROM() {
  for (int i = 23; i >= 0; i--) {
    pumpStatistics[i] = 0;
  }
  pumpedTotal = 0;
  lastHourStarted = timeNow;
  pumpStartedMs = timeNow;
  idleStartedMs = timeNow;
  lastWetMs = 0;
  forceStopStartedMs = 0;
  statisticsCurrentDay = dateTimeNow.day();
  eeprom_update_byte(EEPROM_CONFIGURED, EEPROM_CHECKVALUE);
  saveEeprom();
  //readEeprom();
}

static const int BUF_SIZE = 18;
char lcdBuf1[BUF_SIZE];
char lcdBuf2[BUF_SIZE];
char floatBuf1[BUF_SIZE];
char floatBuf2[BUF_SIZE];

void updateLcd() {
  bool showForceStop = !digitalRead(BUTTON4_PIN);
  bool showResetContainer = !digitalRead(BUTTON6_PIN);

  bool showTimes = !digitalRead(BUTTON1_PIN);
  bool showContainer = !digitalRead(BUTTON5_PIN);
  float leftWater = (CONTAINER_SIZE - pumpedTotal)/1000.0;

  if(showContainer) {
    float pumpedTotalLitres = pumpedTotal / 1000.0;
  
    dtostrf(pumpedTotalLitres, 0, 2, floatBuf1);
    snprintf(lcdBuf1, BUF_SIZE, "Pumped: %s l        ", floatBuf1);
    dtostrf(leftWater, 0, 2, floatBuf1);
    snprintf(lcdBuf2, BUF_SIZE, "Left: %s l        ", floatBuf1);
  }
  else if (showForceStop) {
    snprintf(lcdBuf1, BUF_SIZE, "Force stopping                 ");
    snprintf(lcdBuf2, BUF_SIZE, "for 1 hour                     ");
  }
  else if (showResetContainer) {
    snprintf(lcdBuf1, BUF_SIZE, "Container                    ");
    snprintf(lcdBuf2, BUF_SIZE, "filled                       ");
  }
  else if (showTimes) {  
    if(wasForceStopped) {
      snprintf(lcdBuf1, BUF_SIZE, "FStop %3lu min ago        ", minutesAgo(forceStopStartedMs));
    }
    else {
       snprintf(lcdBuf1, BUF_SIZE, "Was not FStopped     ");
    }
    if(wasMotionStopped) {
      snprintf(lcdBuf2, BUF_SIZE, "MStop %3lu min ago           ", minutesAgo(motionStopStartedMs));
    } else {
      snprintf(lcdBuf2, BUF_SIZE, "Was not MStopped     ");
    }
  } else {
    dtostrf((float)(pumpStatistics[0]/1000.0), 4, 1, floatBuf1);
    dtostrf((float)(pumpStatistics[1]/1000.0), 4, 1, floatBuf2);
    
    long totalMinutes = minutesAgo(pumpStartedMs);
    long hours = totalMinutes/60;
    long minutesLeft = totalMinutes - hours*60;
    int waterRemainingPercent = ((float)(CONTAINER_SIZE - pumpedTotal) / CONTAINER_SIZE)*100;
    snprintf(lcdBuf1, BUF_SIZE, "%s %s %luh %lum         ", floatBuf1, floatBuf2, hours, minutesLeft);
    snprintf(lcdBuf2, BUF_SIZE, "%2d%% %s%s%s %2d:%2d           ", 
      waterRemainingPercent,
      waterLevel ? "We" : "Dr",
      cantStart() ? "St" : "  ", 
      motionSns ? "Mo": "  ",
      dateTimeNow.hour(),
      dateTimeNow.minute()
);
  }

  if(leftWater < 5.0 && timeNow/100 % 100 == 0) {
    analogWrite(ALARM_PIN, 50);
  } else {
    analogWrite(ALARM_PIN, 0);
  }
  
  lcd.setCursor(0, 0);
  lcd.print(lcdBuf1);
  lcd.setCursor(0, 1);
  lcd.print(lcdBuf2);
}

bool forceStopPressed = false;
bool resetButtonPressed = false;
bool backlightButtonPressed = false;
bool backlightOn = false;
bool resetContainerPressed = false;

void readInput() {
  bool containerBtn = !digitalRead(BUTTON6_PIN);

  if(containerBtn != resetContainerPressed && containerBtn) {
     pumpedTotal = 0;
     saveEeprom();
  }
  resetContainerPressed = containerBtn;

  bool resetBtn = !digitalRead(BUTTON8_PIN);
  if (resetBtn != resetButtonPressed && resetBtn) {
    resetEEPROM();
    readEeprom();
  }
  resetButtonPressed = resetBtn;
  bool backlightBtn = !digitalRead(BUTTON3_PIN);
  if (backlightBtn != backlightButtonPressed && backlightBtn) {
    backlightOn = !backlightOn;
    if(backlightOn) {
      lcd.backlight(); 
    } else {
      lcd.noBacklight();
    }
  }
  backlightButtonPressed = backlightBtn;

  bool forceBtn = !digitalRead(BUTTON4_PIN);
  if (forceBtn != forceStopPressed && forceBtn) {
    forceStopStartedMs = timeNow;
    wasForceStopped = true;
  }
  forceStopPressed = forceBtn;
  motionSns = digitalRead(MOTION_PIN);
  if(motionSns) {
    motionStopStartedMs = timeNow;
    wasMotionStopped = true;
  }

  moisture1Percent = 100 - (int)((float)analogRead(IN_MOISTURE1_PIN)/ 1023. * 100);
  waterLevel = digitalRead(WATER_LEVEL_PIN);
}

void startPump() {
  pumpRunning = true;
  pumpStartedMs = timeNow;
  digitalWrite(OUT_PUMP_PIN, HIGH);
  digitalWrite(LED_BUILTIN, HIGH);
  resetMaxWaterLevel();
  saveEeprom();
}

void stopPump() {
  pumpRunning = false;
  digitalWrite(OUT_PUMP_PIN, LOW);
  digitalWrite(LED_BUILTIN, LOW);
  uint16_t pumped = msToMl(timeNow - pumpStartedMs);
  pumpStatistics[0] += pumped; 
  pumpedTotal += pumped;
  idleStartedMs = timeNow;
  saveEeprom();
}


void updateMaxWaterLevel() {
  if (waterLevel) {
    maxWaterLevel = true;
  }
}

void resetMaxWaterLevel() {
  maxWaterLevel = waterLevel;
}

bool stopPumpTimePassed() { return timeNow - pumpStartedMs > PUMP_TIME;}
bool idleTimePassed() { return timeNow - idleStartedMs > IDLE_TIME; }
bool wetRecently() { return wasWet && (timeNow - lastWetMs < WET_TIME); }
bool forceStoppedRecently() { return wasForceStopped && (timeNow - forceStopStartedMs < FORCE_STOP_TIME); }
bool motionStoppedRecently() { return wasMotionStopped && (timeNow - motionStopStartedMs < MOTION_STOP_TIME); }

bool cantStart() { return wetRecently() || forceStoppedRecently() || motionStoppedRecently(); }

void manageWaterPump() {
  updateMaxWaterLevel();

  if (maxWaterLevel) {
    lastWetMs = timeNow;
    wasWet = true;
  }

  if (pumpRunning) {
    if (stopPumpTimePassed() || cantStart()) {
      stopPump();
    }
  } else if (idleTimePassed()) {
    if (!maxWaterLevel && !cantStart()) {
      startPump();
    } else {
      resetMaxWaterLevel();
      idleStartedMs = timeNow;
    }
  }
}

void printStats() {
  for(int i = 0; i<24; i++) {
    Serial.println(pumpStatistics[i]);
  }
}

void setup() {
  Serial.begin(9600);
  Wire.begin();
  rtc.begin();

  if (!rtc.isrunning()) {
    Serial.println("RTC is NOT running!");
    rtc.adjust(DateTime(__DATE__, __TIME__));
  }
  //rtc.adjust(DateTime(__DATE__, __TIME__));
  
  dateTimeNow = rtc.now(); 
  epochAtStart = (dateTimeNow.unixtime() - EPOCH_OFFSET) * 1000;
  
  initializePins();
  readEeprom();
  printStats();
  lcd.init();
}

void loop() {
  timeNow = epochAtStart + millis();
  dateTimeNow.setunixtime((timeNow / 1000) + EPOCH_OFFSET);

  if(dateTimeNow.day() != statisticsCurrentDay) {
    dayPassed();
    statisticsCurrentDay = dateTimeNow.day();
    saveEeprom();
  }

  if (timeNow - lastHourStarted > ONE_HOUR && !pumpRunning) {    
    long correction = rtc.now().unixtime() - dateTimeNow.unixtime();
    epochAtStart += correction * 1000;
    timeNow = epochAtStart + millis();

    lastHourStarted = timeNow;
    saveEeprom();
  }
  readInput();
  manageWaterPump();
  updateLcd();
}