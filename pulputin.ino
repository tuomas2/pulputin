// Copyright (C) 2023 Tuomas Airaksinen
// License: GPL. See GPL.txt for more info

#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <RTClib.h>
#include <OneWire.h>
#include <DallasTemperature.h>

static const uint16_t ONE_WIRE_PIN = 30; // Temperature sensor
static const uint16_t OUT_HEATER_PIN = 32;

static const uint16_t BUTTON1_PIN = 39;
static const uint16_t BUTTON2_PIN = 41;
static const uint16_t BUTTON3_PIN = 43;
static const uint16_t BUTTON4_PIN = 45;
static const uint16_t BUTTON5_PIN = 47;
static const uint16_t BUTTON6_PIN = 49;
static const uint16_t BUTTON7_PIN = 51;
static const uint16_t BUTTON8_PIN = 53;

static const uint16_t WATER_LEVEL_PIN = 48;

static const uint16_t IN_MOISTURE1_PIN = A0;

static const uint16_t OUT_PUMP_PIN = 4; // PWM possible
static const uint16_t ALARM_PIN = 3;

static const uint16_t MOTION_PIN = 52;
static const uint16_t MOTION_GROUND_PIN = 50;

uint16_t moisture1Percent = 0;
bool waterLevel = false;
bool maxWaterLevel = false; 
bool motionSns = false;
bool alarmRunning = false;


// How many ml water has been pumped each hour
// Latest is first item.
// Once an hour last item is removed and each item is moved one forward.
uint16_t pumpStatistics[24]; 
uint16_t pumpedTotal = 0;

// EEPROM addresses
static const uint16_t EEPROM_PUMP_STATISTICS = 0;
static const uint16_t EEPROM_CONFIGURED = 48;
static const uint16_t EEPROM_PUMP_TOTAL = 49;
static const uint16_t EEPROM_LAST_HOUR_STARTED = 51; // 8
static const uint16_t EEPROM_PUMP_STARTED = 59; // 8
static const uint16_t EEPROM_IDLE_STARTED = 67; // 8
static const uint16_t EEPROM_LAST_WET = 75;
static const uint16_t EEPROM_STATS_CUR_DAY = 83; // 1
static const uint16_t EEPROM_DISPLAY_MODE = 84; // 1
static const uint16_t EEPROM_HEATER_STARTED = 85; // 8

static const uint16_t EEPROM_LAST = 93;

static const byte EEPROM_CHECKVALUE = 0b10101010;

static const uint32_t EPOCH_OFFSET = 1694490000;

// Times, in millisecond (since starting device)
uint64_t epochAtStart = 0;
uint64_t timeNow = 0;
DateTime dateTimeNow;

uint64_t lastHourStarted = 0;
uint64_t tempLastRead = 0;
uint64_t modeLastChanged = 0;

uint64_t pumpStartedMs = 0;
uint64_t idleStartedMs = 0;
uint64_t lastWetMs = 0;
uint64_t forceStopStartedMs = 0;
uint64_t motionStopStartedMs = 0;

uint8_t statisticsCurrentDay = 0;

uint8_t displayMode = 0; // DISPLAY_*

bool wasMotionStopped = false;
bool wasForceStopped = false;
bool wasWet = false;
bool pumpRunning = false;

uint64_t heaterStartedMs = 0;
uint64_t heaterIdleStartedMs = 0;

bool heaterRunning = false;

uint16_t minutesAgo(uint64_t timestamp) { return (timeNow - timestamp) / 1000 / 60; }

static const uint16_t PUMP_WATER_SPEED = 116;  // Pump speed, ml per 100 seconds

// Convert millilitres to milliseconds and vice versa
uint64_t mlToMs(uint32_t millilitres) { return 100000 * millilitres / PUMP_WATER_SPEED; }
uint32_t msToMl(uint64_t milliseconds) { return milliseconds * PUMP_WATER_SPEED / 100000; }

static const float TEMP_LIMIT = 8.0;
static const float TEMP_ALARM_LOW = 5.0;
static const float TEMP_ALARM_HIGH = 12.0;

static const uint32_t HEATER_POWER = 50; // Watts
static const uint32_t TARGET_POWER = 1; // Watts
static const uint32_t HEATER_ON_TIME = 5; // Seconds
static const uint32_t HEATER_IDLE_TIME = HEATER_POWER * (float)HEATER_ON_TIME / TARGET_POWER - HEATER_ON_TIME;  

static const uint8_t DISPLAY_SUMMER = 0;
static const uint8_t DISPLAY_WINTER = 1;
static const uint8_t DISPLAY_INTERVAL = 2;

uint8_t modeNow = DISPLAY_SUMMER;

static const uint32_t ONE_HOUR = 3600000;
static const uint32_t ONE_MINUTE = ONE_HOUR/60;

static const uint16_t CONTAINER_SIZE = 30000;  // Water container size in (ml)

static const uint16_t PUMP_PORTION = 100;       // Amount of water pumped at once (ml)
static const uint32_t PERIOD_TIME = 15*ONE_MINUTE; // Adjusted water amount is PUMP_PORTION / PERIOD_TIME.


static const uint32_t PUMP_TIME = mlToMs(PUMP_PORTION);
static const uint32_t IDLE_TIME = PERIOD_TIME - PUMP_TIME;

static const uint32_t WET_TIME = ONE_HOUR;
static const uint32_t FORCE_STOP_TIME = ONE_HOUR;
static const uint32_t MOTION_STOP_TIME = ONE_MINUTE * 15;


float temperature = TEMP_LIMIT + 1; // in celsius 
bool tempSensorFail = false;

OneWire oneWire(ONE_WIRE_PIN);
DallasTemperature sensors(&oneWire);

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
  pinMode(OUT_HEATER_PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(ALARM_PIN, OUTPUT);

  pinMode(MOTION_GROUND_PIN, OUTPUT);
  digitalWrite(MOTION_GROUND_PIN, LOW);

  pinMode(WATER_LEVEL_PIN, INPUT_PULLUP);
  
  digitalWrite(LED_BUILTIN, LOW);
  digitalWrite(ALARM_PIN, LOW);
  digitalWrite(OUT_PUMP_PIN, LOW);
  digitalWrite(OUT_HEATER_PIN, LOW);
}

void readEeprom() {
  if (eeprom_read_byte(EEPROM_CONFIGURED) != EEPROM_CHECKVALUE) {
    resetEEPROM();
  }
  for (uint16_t i = 0; i < 24; i++) {
    pumpStatistics[i] = eeprom_read_word(EEPROM_PUMP_STATISTICS + i*2);
  }
  
  pumpedTotal = eeprom_read_word(EEPROM_PUMP_TOTAL);

  eeprom_read_block(&lastHourStarted, EEPROM_LAST_HOUR_STARTED, 8);
  eeprom_read_block(&pumpStartedMs, EEPROM_PUMP_STARTED, 8); 
  eeprom_read_block(&idleStartedMs, EEPROM_IDLE_STARTED, 8); 
  eeprom_read_block(&heaterStartedMs, EEPROM_HEATER_STARTED, 8); 

  eeprom_read_block(&lastWetMs, EEPROM_LAST_WET, 8); 
 
  statisticsCurrentDay = eeprom_read_byte(EEPROM_STATS_CUR_DAY); 
  displayMode = eeprom_read_byte(EEPROM_DISPLAY_MODE);
}

void saveEeprom() {
  for (uint16_t i = 0; i < 24; i++) {
    eeprom_update_word(EEPROM_PUMP_STATISTICS + i*2, pumpStatistics[i]);
  }
  eeprom_update_word(EEPROM_PUMP_TOTAL, pumpedTotal);
  
  eeprom_update_block(&lastHourStarted, EEPROM_LAST_HOUR_STARTED, 8);
  eeprom_update_block(&pumpStartedMs, EEPROM_PUMP_STARTED, 8);
  eeprom_update_block(&idleStartedMs, EEPROM_IDLE_STARTED, 8);
  eeprom_update_block(&heaterStartedMs, EEPROM_HEATER_STARTED, 8);

  eeprom_update_block(&lastWetMs, EEPROM_LAST_WET, 8);

  eeprom_update_byte(EEPROM_STATS_CUR_DAY, statisticsCurrentDay);
  eeprom_update_byte(EEPROM_DISPLAY_MODE, displayMode);
}


void dayPassed() {
  for (int16_t i = 23; i > 0; i--) {
    pumpStatistics[i] = pumpStatistics[i - 1];
  }
  pumpStatistics[0] = 0;
}

void resetEEPROM() {
  for (int16_t i = 23; i >= 0; i--) {
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

static const uint16_t BUF_SIZE = 18;
char lcdBuf1[BUF_SIZE];
char lcdBuf2[BUF_SIZE];
char floatBuf1[BUF_SIZE];
char floatBuf2[BUF_SIZE];

void updateLcdSummer() {
  bool showForceStop = !digitalRead(BUTTON4_PIN);
  bool showResetContainer = !digitalRead(BUTTON6_PIN);
  bool backlightBtn = !digitalRead(BUTTON3_PIN);
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
    snprintf(lcdBuf1, BUF_SIZE, "Wet %u min ago        ", minutesAgo(lastWetMs));
    snprintf(lcdBuf2, BUF_SIZE, "Pumped %u min ago        ", minutesAgo(pumpStartedMs));
  } else {
    //dtostrf(temperature, 4, 1, floatBuf1);

    dtostrf((float)(pumpStatistics[0]/1000.0), 4, 1, floatBuf1);
    dtostrf((float)(pumpStatistics[1]/1000.0), 4, 1, floatBuf2);
    
    int32_t totalMinutes = minutesAgo(waterLevel ? pumpStartedMs: lastWetMs);
    int32_t hours = totalMinutes/60;
    int32_t minutesLeft = totalMinutes - hours*60;
    uint16_t waterRemainingPercent = ((float)(CONTAINER_SIZE - pumpedTotal - 1) / CONTAINER_SIZE)*100;
    snprintf(lcdBuf1, BUF_SIZE, "%s %s %luh %lum         ", floatBuf1, floatBuf2, hours, minutesLeft);
    snprintf(lcdBuf2, BUF_SIZE, "%2d%% %s%s%s %2u:%02u           ", 
      waterRemainingPercent,
      waterLevel ? "We" : "Dr",
      motionSns ? "Mo": "  ",
      cantStart() ? "St" : "  ", 
      dateTimeNow.hour(), dateTimeNow.minute()
    );
  }
}

void updateLcdWinter() {
    dtostrf(temperature, 4, 1, floatBuf1);

    int32_t totalMinutes = minutesAgo(heaterStartedMs);
    int32_t hours = totalMinutes/60;
    int32_t minutesLeft = totalMinutes - hours*60;

    snprintf(lcdBuf1, BUF_SIZE, "%s C %luh %lum         ", floatBuf1, hours, minutesLeft);
    snprintf(lcdBuf2, BUF_SIZE, "%s %s %2u:%02u                    ", 
      heaterRunning ? "He" : "  ",
      tempSensorFail ? "!!" : "  ",
      dateTimeNow.hour(), dateTimeNow.minute()
    );
}

void updateLcd() {
  if(displayMode == DISPLAY_SUMMER) {
    updateLcdSummer();
  } else if(displayMode == DISPLAY_WINTER) {
    updateLcdWinter();
  } else if(displayMode == DISPLAY_INTERVAL) {
    if (timeNow - modeLastChanged > 5000) {
      modeLastChanged = timeNow;
      modeNow = (modeNow + 1)%2;
    }
    if(modeNow == DISPLAY_SUMMER) updateLcdSummer();
    else updateLcdWinter();
  }
  lcd.setCursor(0, 0);
  lcd.print(lcdBuf1);
  lcd.setCursor(0, 1);
  lcd.print(lcdBuf2);
}

void updateBeeper() {
  bool backlightBtn = !digitalRead(BUTTON3_PIN);
  if(backlightBtn || (alarmRunning && timeNow/100 % 100 == 0)) {
    analogWrite(ALARM_PIN, 50);
  } else {
    analogWrite(ALARM_PIN, 0);
  }
}

bool forceStopPressed = false;
bool resetButtonPressed = false;
bool backlightButtonPressed = false;
bool backlightOn = false;
bool resetContainerPressed = false;
bool forceRunPressed = false;
bool modeChangePressed = false;


void readInput() {
  bool modeChangeBtn = !digitalRead(BUTTON2_PIN);
  if (modeChangeBtn != modeChangePressed) {
    if(modeChangeBtn) {
      displayMode = (displayMode + 1) % 3;
      saveEeprom();
      Serial.println(displayMode);
    }
  }
  modeChangePressed = modeChangeBtn;
  bool forceRunBtn = !digitalRead(BUTTON7_PIN);
  if(forceRunBtn != forceRunPressed) {
    if(forceRunBtn) {
      digitalWrite(OUT_PUMP_PIN, HIGH);
      digitalWrite(LED_BUILTIN, HIGH);
    } else {
      digitalWrite(OUT_PUMP_PIN, pumpRunning ? HIGH: LOW);
      digitalWrite(LED_BUILTIN, pumpRunning ? HIGH: LOW);
    }
  }
  forceRunPressed = forceRunBtn;

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

  moisture1Percent = 100 - (uint16_t)((float)analogRead(IN_MOISTURE1_PIN)/ 1023. * 100);
  waterLevel = digitalRead(WATER_LEVEL_PIN);
}

void startHeat() {
  Serial.println("startHeat");
  heaterRunning = true;
  heaterStartedMs = timeNow;
  digitalWrite(OUT_HEATER_PIN, HIGH);
  updateBuiltinLed();
  saveEeprom();
}

void stopHeat() {
  Serial.println("stopHeat");
  heaterRunning = false;
  heaterIdleStartedMs = timeNow;
  updateBuiltinLed();
}

void updateBuiltinLed() {
   digitalWrite(LED_BUILTIN, (heaterRunning || pumpRunning) ? HIGH: LOW);
}

void startPump() {
  pumpRunning = true;
  pumpStartedMs = timeNow;
  digitalWrite(OUT_PUMP_PIN, HIGH);
  updateBuiltinLed();
  resetMaxWaterLevel();
  saveEeprom();
}

void stopPump() {
  pumpRunning = false;
  digitalWrite(OUT_PUMP_PIN, LOW);
  updateBuiltinLed();
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

bool stopHeaterTimePassed() { return timeNow - heaterStartedMs > HEATER_ON_TIME*1000; }
bool heaterIdleTimePassed() { return timeNow - heaterIdleStartedMs > HEATER_IDLE_TIME*1000; }
bool isTriggerTemp() { return temperature < TEMP_LIMIT; }
bool isAlarmTemp() { return temperature < TEMP_ALARM_LOW || temperature > TEMP_ALARM_HIGH; }


bool cantStart() { return isTriggerTemp() || wetRecently() || forceStoppedRecently() || motionStoppedRecently(); }

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

void manageHeater() {
  if (heaterRunning) {
    if(stopHeaterTimePassed()) stopHeat();
  } else if (heaterIdleTimePassed() && isTriggerTemp()) { 
    startHeat();
  }
}

void manageAlarm() {
  float leftWater = (CONTAINER_SIZE - pumpedTotal)/1000.0;
  alarmRunning = tempSensorFail || isAlarmTemp() || (leftWater < 5.0 && !forceStoppedRecently());
}

void printStats() {
  for(uint16_t i = 0; i<24; i++) {
    Serial.println(pumpStatistics[i]);
  }
}

void setup() {
  Serial.begin(9600);
  Wire.begin();
  rtc.begin();
  sensors.begin();

  if (!rtc.isrunning()) {
    Serial.println("RTC is NOT running!");
    rtc.adjust(DateTime(__DATE__, __TIME__));
  }
  //Serial.println(__TIME__);
  //rtc.adjust(DateTime(__DATE__, __TIME__));
  
  dateTimeNow = rtc.now();
  
  dateTimeNow.tostr(lcdBuf1); 
  Serial.println(lcdBuf1);
  Serial.println(dateTimeNow.hour());
  Serial.println(dateTimeNow.minute());
  
  epochAtStart = (uint64_t)(dateTimeNow.unixtime() - EPOCH_OFFSET) * 1000;
  
  initializePins();
  readEeprom();
  printStats();
  lcd.init();
  Serial.println("Heat params in seconds");
  Serial.println(HEATER_ON_TIME);
  Serial.println(HEATER_IDLE_TIME);
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
    int32_t correction = rtc.now().unixtime() - dateTimeNow.unixtime();
    epochAtStart += correction * 1000;
    timeNow = epochAtStart + millis();

    lastHourStarted = timeNow;
    saveEeprom();
  }
  if (timeNow - tempLastRead > 10000) {
    sensors.requestTemperatures();
    float temp = sensors.getTempCByIndex(0);
    if (temp != DEVICE_DISCONNECTED_C) {
      temperature = temp;
      tempSensorFail = false;
    } else {
      temperature = TEMP_LIMIT + 1;
      tempSensorFail = true;
    }
    Serial.println(temperature);
    tempLastRead = timeNow;
  }
  readInput();
  manageWaterPump();
  manageHeater();
  manageAlarm();
  updateLcd();
  updateBeeper();
}