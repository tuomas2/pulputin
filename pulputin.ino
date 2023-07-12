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

static const int WATER_LEVEL_PIN_GROUND = 31;
static const int WATER_LEVEL_PIN = 33;

static const int IN_MOISTURE1_PIN = A0;

static const int OUT_PUMP_PIN = 2; // PWM possible

int moisture1Percent = 0;
bool waterLevel = false;
bool maxWaterLevel = false; 

// How many ml water has been pumped each hour
// Latest is first item.
// Once an hour last item is removed and each item is moved one forward.
uint16_t pumpStatistics[24]; 
uint16_t pumpedTotal = 0;

// EEPROM addresses
static const int EEPROM_PUMP_STATISTICS = 0;
static const int EEPROM_CONFIGURED = 48;
static const int EEPROM_PUMP_TOTAL = 49;
static const int EEPROM_LAST = 50;

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

unsigned long minutesAgo(unsigned long timestamp) { return (timeNow - timestamp) / 1000 / 60; }

static const int PUMP_WATER_SPEED = 133;  // Pump speed, ml per 100 seconds

// Convert millilitres to milliseconds and vice versa
unsigned long mlToMs(unsigned long millilitres) { return 100000 * millilitres / PUMP_WATER_SPEED; }
unsigned long msToMl(unsigned long milliseconds) { return milliseconds * PUMP_WATER_SPEED / 100000; }

static const unsigned long ONE_HOUR = 3600000;
static const unsigned long ONE_MINUTE = ONE_HOUR/60;

static const uint16_t CONTAINER_SIZE = 25000;  // Water container size in (ml)

static const int PUMP_PORTION = 100;       // Amount of water pumped at once (ml)
static const unsigned long PERIOD_TIME = 30*ONE_MINUTE; // Adjusted water amount is PUMP_PORTION / PERIOD_TIME.


static const unsigned long PUMP_TIME = mlToMs(PUMP_PORTION);
static const unsigned long IDLE_TIME = PERIOD_TIME - PUMP_TIME;

static const unsigned long WET_TIME = ONE_HOUR * 1;
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
  pinMode(OUT_PUMP_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(WATER_LEVEL_PIN_GROUND, OUTPUT);
  digitalWrite(WATER_LEVEL_PIN_GROUND, LOW);
  pinMode(WATER_LEVEL_PIN, INPUT_PULLUP);
  
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
  pumpedTotal = eeprom_read_word(EEPROM_PUMP_TOTAL);
}

void savePumpStatistics() {
  for (int i = 0; i < 24; i++) {
    eeprom_write_word(EEPROM_PUMP_STATISTICS + i*2, pumpStatistics[i]);
  }
  eeprom_write_word(EEPROM_PUMP_TOTAL, pumpedTotal);
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

static const int LCD_BUF_SIZE = 18;
char lcdBuf1[LCD_BUF_SIZE];
char lcdBuf2[LCD_BUF_SIZE];

void updateLcd() {
  bool showForceStop = !digitalRead(BUTTON4_PIN);
  bool showResetContainer = !digitalRead(BUTTON6_PIN);

  bool showTimes = !digitalRead(BUTTON1_PIN);
  bool showContainer = !digitalRead(BUTTON5_PIN);
  unsigned long leftWater = (CONTAINER_SIZE - pumpedTotal)/1000;

  if(showContainer) {
     snprintf(lcdBuf1, LCD_BUF_SIZE, "Pumped: %lu l        ", (unsigned long)pumpedTotal/1000);
     snprintf(lcdBuf2, LCD_BUF_SIZE, "Left: %lu l        ", leftWater);
  }
  else if (showForceStop) {
    snprintf(lcdBuf1, LCD_BUF_SIZE, "Force stopping                 ");
    snprintf(lcdBuf2, LCD_BUF_SIZE, "for 3 hours                    ");
  }
  else if (showResetContainer) {
    snprintf(lcdBuf1, LCD_BUF_SIZE, "Container                    ");
    snprintf(lcdBuf2, LCD_BUF_SIZE, "filled                       ");
  }
  else if (showTimes) {  
    if(wasForceStopped) {
      snprintf(lcdBuf1, LCD_BUF_SIZE, "FStop %3lumin ago        ", minutesAgo(forceStopStartedMs));
    }
    else {
       snprintf(lcdBuf1, LCD_BUF_SIZE, "Was not FStopped     ");
    }
    if(wasWet) {
      snprintf(lcdBuf2, LCD_BUF_SIZE, "Wet %3lu min ago           ", minutesAgo(lastWetMs));
    } else {
      snprintf(lcdBuf2, LCD_BUF_SIZE, "Was not wet yet     ");
    }
  } else {
    unsigned long total = 0;
    
    for (int i = 0; i < 24; i++) {
      total += pumpStatistics[i];
    }
    snprintf(lcdBuf1, LCD_BUF_SIZE, "%3lu dl/d %3lu min           ", total/100, minutesAgo(pumpStartedMs));
    snprintf(lcdBuf2, LCD_BUF_SIZE, "%s%s %3d%% %s%s             ", waterLevel ? "W" : "-", maxWaterLevel ? "W": "-", moisture1Percent, cantStart() ? "STOP" : "", leftWater < 5 ? "!!" : "");
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
     savePumpStatistics();
  }
  resetContainerPressed = containerBtn;

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
  waterLevel = digitalRead(WATER_LEVEL_PIN);
}

void startPump() {
  pumpRunning = true;
  pumpStartedMs = timeNow;
  digitalWrite(OUT_PUMP_PIN, HIGH);
  digitalWrite(LED_BUILTIN, HIGH);
  resetMaxWaterLevel();
}

void stopPump() {
  pumpRunning = false;
  digitalWrite(OUT_PUMP_PIN, LOW);
  digitalWrite(LED_BUILTIN, LOW);
  uint16_t pumped = msToMl(timeNow - pumpStartedMs);
  pumpStatistics[0] += pumped; 
  pumpedTotal += pumped;
  savePumpStatistics();
  idleStartedMs = timeNow;
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
bool cantStart() { return wetRecently() || forceStoppedRecently(); }

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