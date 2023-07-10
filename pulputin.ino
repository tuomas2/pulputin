// Copyright (C) 2023 Tuomas Airaksinen
// License: GPL. See GPL.txt for more info

#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

static const int BUTTON1_PIN = 39;
static const int BUTTON2_PIN = 41;
static const int BUTTON3_PIN = 43;
static const int BUTTON4_PIN = 45;
static const int BUTTON5_PIN = 47;
static const int BUTTON6_PIN = 49;
static const int BUTTON7_PIN = 51;
static const int BUTTON8_PIN = 53;

static const int IN_MOISTURE1_PIN = A0;
static const int IN_MOISTURE2_PIN = A1;

static const int OUT_PUMP_PIN = 2; // PWM possible

static const int PUMP_PORTION = 10;       // Amount of water pumped at once (ml)
static const int PUMP_WATER_SPEED = 133;  // Pump speed, ml per 100 seconds

static const int MAX_DRY_MOISTURE = 5;  // percent
static const int MIN_WET_MOISTURE = 5;  // percent

int moisture1Percent = 0;
int moisture2Percent = 0;
int maxMoisture = 0; // Max value during whole idle time

// How many ml water has been pumped each hour
// Latest is first item.
// Once an hour last item is removed and each item is moved one forward.
uint16_t pumpStatistics[24]; 

// EEPROM addresses
static const int EEPROM_PUMP_STATISTICS = 0;
static const int EEPROM_CONFIGURED = 48;
static const int EEPROM_LAST = 48;

static const byte EEPROM_CHECKVALUE = 0b10101010;

// Times, in millisecond (since starting device)
unsigned long timeNow = 0;
unsigned long lastHourStarted = 0;
unsigned long pumpStartedMs = 0;
unsigned long idleStartedMs = 0;
unsigned long lastWetMs = 0;
unsigned long forceStopStartedMs = 0;

bool wasForceStopped = false;
bool wasWet = false;
bool pumpRunning = false;

// Convert millilitres to milliseconds
unsigned long mlToMs(int millilitres) { return 100000 * millilitres / PUMP_WATER_SPEED; }

static const unsigned long ONE_HOUR = 3600000;
static const unsigned long PUMP_TIME = mlToMs(PUMP_PORTION);
static const unsigned long IDLE_TIME = PUMP_TIME * 10;
static const unsigned long WET_TIME = ONE_HOUR;
static const unsigned long FORCE_STOP_TIME = ONE_HOUR*3;

LiquidCrystal_I2C lcd(0x3F, 16, 2);

void initializePins() {
  pinMode(BUTTON1_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);
  pinMode(BUTTON3_PIN, INPUT_PULLUP);
  pinMode(BUTTON4_PIN, INPUT_PULLUP);
  pinMode(BUTTON5_PIN, INPUT_PULLUP);
  pinMode(BUTTON6_PIN, INPUT_PULLUP);
  pinMode(BUTTON7_PIN, INPUT_PULLUP);
  pinMode(BUTTON8_PIN, INPUT_PULLUP);
  pinMode(IN_MOISTURE1_PIN, INPUT);
  pinMode(IN_MOISTURE2_PIN, INPUT);
  pinMode(OUT_PUMP_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  digitalWrite(OUT_PUMP_PIN, LOW);
}

void initializeStatistics() {
  if (eeprom_read_byte(EEPROM_CONFIGURED) != EEPROM_CHECKVALUE) {
    resetEEPROM();
  }
  for (int i = 0; i < 24; i++) {
    pumpStatistics[i] = eeprom_read_word(EEPROM_PUMP_STATISTICS + i*2);
  }
}

void savePumpStatistics() {
  for (int i = 0; i < 24; i++) {
    eeprom_write_word(EEPROM_PUMP_STATISTICS + i*2, pumpStatistics[i]);
  }
}

void hourPassed() {
  for (int i = 23; i > 0; i--) {
    pumpStatistics[i] = pumpStatistics[i - 1];
  }
  pumpStatistics[0] = 0;
  savePumpStatistics();
}

void resetEEPROM() {
  for (int i = 0; i < EEPROM_LAST; i++) {
    eeprom_write_byte(i, 0);
  }
  eeprom_write_byte(EEPROM_CONFIGURED, EEPROM_CHECKVALUE);
}

void updateLcd() {
  static int SIZE = 20;
  char lcdBuf1[SIZE];
  char lcdBuf2[SIZE];

  bool showForceStop = !digitalRead(BUTTON4_PIN);
  bool showTimes = !digitalRead(BUTTON1_PIN);
  bool button1Pressed = !digitalRead(BUTTON2_PIN);

  int pumpLastStartedAgo = (timeNow - pumpStartedMs) / 1000 / 60;
  
  if (showForceStop) {
    snprintf(lcdBuf1, SIZE, "Force stopping                 ");
    snprintf(lcdBuf2, SIZE, "for 3 hours                    ");
  }
  else if (showTimes) {  
    snprintf(lcdBuf1, SIZE, "Pump %d min ago            ", pumpLastStartedAgo);
    if(wasWet) {
      int wetLastAgo = (timeNow - lastWetMs) / 1000 / 60;
      snprintf(lcdBuf2, SIZE, "Wet %d min ago           ", wetLastAgo);
    } else {
      snprintf(lcdBuf2, SIZE, "Was not wet yet     ");
    }
  } else {
    int total = 0;
    for (int i = 0; i < 24; i++) {
      total += pumpStatistics[i];
    }
    snprintf(lcdBuf1, SIZE, "%d dl/d %d min           ", total/100, pumpLastStartedAgo);
    if (button1Pressed) {
      snprintf(lcdBuf2, SIZE, "m1: %d%% m2: %d%%            ", moisture1Percent, moisture2Percent);
    } else {
      snprintf(lcdBuf2, SIZE, "%3d%% %3d%%              ", maxMoisture, moisture2Percent);
    }
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

void readInput() {
  bool resetBtn = !digitalRead(BUTTON8_PIN);
  if (resetBtn != resetButtonPressed && resetBtn) {
    resetEEPROM();
    initializeStatistics();
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

  moisture1Percent = 100 - (int)((float)analogRead(IN_MOISTURE1_PIN)/ 1023. * 100);
  moisture2Percent = 100 - (int)((float)analogRead(IN_MOISTURE2_PIN) / 1023. * 100);
}

void startPump() {
  pumpRunning = true;
  pumpStartedMs = timeNow;
  digitalWrite(OUT_PUMP_PIN, HIGH);
  digitalWrite(LED_BUILTIN, HIGH);
  resetMaxMoisture();
}

void stopPump() {
  pumpRunning = false;
  digitalWrite(OUT_PUMP_PIN, LOW);
  digitalWrite(LED_BUILTIN, LOW);
  pumpStatistics[0] += PUMP_PORTION;
  savePumpStatistics();
  idleStartedMs = timeNow;
}


void updateMaxMoisture() {
  if (moisture1Percent > maxMoisture) {
    maxMoisture = moisture1Percent;
  }
}

void resetMaxMoisture() {
  maxMoisture = moisture1Percent;
}

bool stopPumpTimePassed() { return timeNow - pumpStartedMs > PUMP_TIME;}
bool idleTimePassed() { return timeNow - idleStartedMs > IDLE_TIME; }
bool wetRecently() { return wasWet && (timeNow - lastWetMs < WET_TIME); }
bool forceStoppedRecently() { return wasForceStopped && (timeNow - forceStopStartedMs < FORCE_STOP_TIME); }
bool cantStart() { return wetRecently() || forceStoppedRecently(); }

void manageWaterPump() {
  updateMaxMoisture();

  if (maxMoisture > MIN_WET_MOISTURE) {
    lastWetMs = timeNow;
    wasWet = true;
  }

  if (pumpRunning) {
    if (stopPumpTimePassed() || cantStart()) {
      stopPump();
    }
  } else if (idleTimePassed()) {
    if ((maxMoisture < MAX_DRY_MOISTURE) && !cantStart()) {
      startPump();
    } else {
      resetMaxMoisture();
      idleStartedMs = timeNow;
    }
  }
}

void setup() {
  initializePins();
  initializeStatistics();
  lcd.init();
  readInput();
  startPump();
}

void loop() {
  timeNow = millis();
  if (timeNow - lastHourStarted > ONE_HOUR) {
    hourPassed();
    lastHourStarted = timeNow;
  }
  readInput();
  manageWaterPump();
  updateLcd();
}