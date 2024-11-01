// Copyright (C) 2023 Tuomas Airaksinen
// License: GPL. See GPL.txt for more info

// #define USE_LOWPOWER

#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <RTClib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <avr/wdt.h>
#ifdef USE_LOWPOWER
  #include <LowPower.h>
#endif

static const uint16_t ONE_WIRE_PIN = 30; // Temperature sensor
static const uint16_t OUT_HEATER_PIN = 34;

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

// How many milliseconds has been spent to heat, each hour.
uint32_t heatStatistics[24]; 

// EEPROM addresses
static const uint16_t EEPROM_PUMP_STATISTICS = 0; // 2*24 = 48
static const uint16_t EEPROM_CONFIGURED = 48;
static const uint16_t EEPROM_PUMP_TOTAL = 49;

// NOT NEEDED. ADDRESS CAN BE USED FOR OTHER PURPOSE LATER...
// static const uint16_t EEPROM_LAST_TIME_CLOCK_CORRECTED = 51; // 8

static const uint16_t EEPROM_PUMP_STARTED = 59; // 8
static const uint16_t EEPROM_IDLE_STARTED = 67; // 8
static const uint16_t EEPROM_LAST_WET = 75;
static const uint16_t EEPROM_STATS_CUR_DAY = 83; // 1
static const uint16_t EEPROM_DISPLAY_MODE = 84; // 1
static const uint16_t EEPROM_HEATER_STARTED = 85; // 8
static const uint16_t EEPROM_HEAT_STATISTICS = 93; // 4*24 = 96
static const uint16_t EEPROM_LAST = 188; 

static const byte EEPROM_CHECKVALUE = 0b10101010;

static const uint32_t EPOCH_OFFSET = 1694490000;

// Times, in millisecond (since starting device)
uint64_t epochAtStart = 0;
uint64_t timeNow = 0;
DateTime dateTimeNow;

uint64_t lastTimeClockCorrected = 0;
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

static const uint16_t PUMP_WATER_SPEED = 106;  // Pump speed, ml per 100 seconds

// Convert millilitres to milliseconds and vice versa
uint64_t mlToMs(uint32_t millilitres) { return 100000 * millilitres / PUMP_WATER_SPEED; }
uint32_t msToMl(uint64_t milliseconds) { return milliseconds * PUMP_WATER_SPEED / 100000; }


static const uint32_t ONE_SECOND = 1000;
static const uint32_t ONE_HOUR = 3600000;
static const uint32_t ONE_MINUTE = ONE_HOUR/60;
static const uint32_t FIFTEEN_MINUTES = ONE_MINUTE*15;

static const float TEMP_LIMIT = 5.0;
static const float TEMP_ALARM_LOW = 3.0;

static const uint32_t HEATER_POWER = 50; // Watts
static const uint32_t TARGET_POWER = 5; // Watts
static const uint32_t HEATER_ON_TIME = 5*ONE_SECOND;
static const uint32_t HEATER_IDLE_TIME = HEATER_POWER * (float)HEATER_ON_TIME / TARGET_POWER - HEATER_ON_TIME;  

static const uint8_t DISPLAY_SUMMER = 0;
static const uint8_t DISPLAY_WINTER = 1;
static const uint8_t DISPLAY_INTERVAL = 2;

uint8_t modeNow = DISPLAY_SUMMER;

static const uint16_t CONTAINER_SIZE = 28000;  // Water container size in (ml)

static const uint16_t PUMP_PORTION = 100;       // Amount of water pumped at once (ml)
static const uint32_t PERIOD_TIME = 15*ONE_MINUTE; // Adjusted water amount is PUMP_PORTION / PERIOD_TIME.


static const uint32_t PUMP_TIME = mlToMs(PUMP_PORTION);
static const uint32_t IDLE_TIME = PERIOD_TIME - PUMP_TIME;

static const uint32_t WET_TIME = ONE_HOUR;
static const uint32_t DRY_TOO_LONG_TIME = ONE_HOUR*48;

static const uint32_t FORCE_STOP_TIME = ONE_HOUR;
static const uint32_t MOTION_STOP_TIME = ONE_MINUTE * 15;


float temperature = TEMP_LIMIT + 1; // in celsius 
bool tempSensorFail = false;
bool showBootInfo = true;

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
    heatStatistics[i] = eeprom_read_dword(EEPROM_HEAT_STATISTICS + i*4);
  }

  pumpedTotal = eeprom_read_word(EEPROM_PUMP_TOTAL);

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
    eeprom_update_dword(EEPROM_HEAT_STATISTICS + i*4, heatStatistics[i]);
  }
 
  eeprom_update_word(EEPROM_PUMP_TOTAL, pumpedTotal);
  
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
    heatStatistics[i] = heatStatistics[i - 1];
  }
  pumpStatistics[0] = 0;
  heatStatistics[0] = 0;
}

void resetEEPROM() {
  for (int16_t i = 23; i >= 0; i--) {
    pumpStatistics[i] = 0;
    heatStatistics[i] = 0;
  }

  pumpedTotal = 0;
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

char lcdBuf1a[BUF_SIZE];
char lcdBuf2a[BUF_SIZE];

char floatBuf1[BUF_SIZE];
char floatBuf2[BUF_SIZE];
char floatBuf3[BUF_SIZE];
char timeOrTempBuf[BUF_SIZE];


void updateLcdSummer() {
  dtostrf((float)(pumpStatistics[0]/1000.0), 4, 1, floatBuf1);
  dtostrf((float)(pumpStatistics[1]/1000.0), 4, 1, floatBuf2);
  
  int32_t totalMinutes = minutesAgo(waterLevel ? pumpStartedMs: lastWetMs);
  int32_t hours = totalMinutes/60;
  int32_t minutesLeft = totalMinutes - hours*60;
  uint16_t waterRemainingPercent = ((float)(CONTAINER_SIZE - pumpedTotal - 1) / CONTAINER_SIZE)*100;
  snprintf(lcdBuf1, BUF_SIZE, "%s %s %luh %lum         ", floatBuf1, floatBuf2, hours, minutesLeft);
  snprintf(lcdBuf2, BUF_SIZE, "%2d%% %s%s%s %s           ", 
    waterRemainingPercent,
    waterLevel ? "We" : "Dr",
    motionSns ? "Mo": "  ",
    cantStart() ? "St" : "  ", 
    timeOrTempBuf
  );
}

void updateLcdWinter() {
  int32_t totalMinutes = minutesAgo(heaterStartedMs);
  int32_t hours = totalMinutes/60;
  int32_t minutesLeft = totalMinutes - hours*60;

  dtostrf((float)(heatStatistics[0]/60000.0), 4, 1, floatBuf1); // Show heat on in minutes 
  dtostrf((float)(heatStatistics[1]/60000.0), 4, 1, floatBuf2);
  dtostrf(temperature, 4, 1, floatBuf3);
  snprintf(lcdBuf1, BUF_SIZE, "%s %s %luh %lum         ", floatBuf1, floatBuf2, hours, minutesLeft);    
  snprintf(lcdBuf2, BUF_SIZE, "%sC %s%s %s                    ", 
    floatBuf3, 
    heaterRunning ? "He" : "  ",
    tempSensorFail ? "!!" : "  ",
    timeOrTempBuf
  );
}

void updateLcd() {
  bool showForceStop = !digitalRead(BUTTON4_PIN);
  bool showResetContainer = !digitalRead(BUTTON6_PIN);
  bool backlightBtn = !digitalRead(BUTTON3_PIN);
  bool showTimes = !digitalRead(BUTTON1_PIN);
  bool showContainer = !digitalRead(BUTTON5_PIN);
  float leftWater = (CONTAINER_SIZE - pumpedTotal)/1000.0;
  
  if (timeNow - modeLastChanged > 5000) {
    modeLastChanged = timeNow;
    modeNow = (modeNow + 1)%2;
  }

  if (displayMode == DISPLAY_WINTER || modeNow == 0) {
    snprintf(timeOrTempBuf, BUF_SIZE, "%2u:%02u               ", dateTimeNow.hour(), dateTimeNow.minute());
  } else {
    dtostrf(temperature, 4, 1, floatBuf3);
    snprintf(timeOrTempBuf, BUF_SIZE, "%sC              ", floatBuf3);
  }
  
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
    if(displayMode == DISPLAY_SUMMER) {
      updateLcdSummer();
    } else if(displayMode == DISPLAY_WINTER) {
      updateLcdWinter();
    } else if(displayMode == DISPLAY_INTERVAL) {
      if(modeNow == DISPLAY_SUMMER) updateLcdSummer();
      else updateLcdWinter();
    }
  }
  if (!showBootInfo && strcmp(lcdBuf1a, lcdBuf1) != 0) {
    lcd.setCursor(0, 0);
    lcd.print(lcdBuf1);
    strcpy(lcdBuf1a, lcdBuf1);
  }
  if (strcmp(lcdBuf2a, lcdBuf2) != 0) {
    lcd.setCursor(0, 1);
    lcd.print(lcdBuf2);
    strcpy(lcdBuf2a, lcdBuf2);
  }
}

void printBootInfo() {
  snprintf(timeOrTempBuf, BUF_SIZE, "%u.%u %2u:%02u               ", dateTimeNow.day(), dateTimeNow.month(), dateTimeNow.hour(), dateTimeNow.minute());
  snprintf(lcdBuf1, BUF_SIZE, "BTN1 %s", timeOrTempBuf);  
  lcd.setCursor(0, 0);
  lcd.print(lcdBuf1);
}

uint64_t blinkStoppedMs = 0;
uint64_t blinkStartedMs = 0;
bool blinkNow = false;

void manageBlink() {
  if(!blinkNow && (timeNow - blinkStoppedMs > 30000)) {
    blinkNow = true;
    blinkStartedMs = timeNow;
  }
  if(blinkNow && (timeNow - blinkStartedMs > 50)) {
    blinkNow = false;
    blinkStoppedMs = timeNow;
  }
}

bool isBeeping() {
  bool backlightBtn = !digitalRead(BUTTON3_PIN);
  return backlightBtn || (blinkNow && alarmRunning);
}

void updateBeeper() {
  analogWrite(ALARM_PIN, isBeeping() ? 50 : 0);
}

void manageBuiltinLedBlink() {
  if(blinkNow) {
    digitalWrite(LED_BUILTIN, HIGH);
  } else {
    updateBuiltinLed();
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
  bool button1 = !digitalRead(BUTTON1_PIN);
  if (button1) {
    showBootInfo = false;
  }

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
      if(isWinter()) {
        digitalWrite(OUT_HEATER_PIN, HIGH);
      } else {
        digitalWrite(OUT_PUMP_PIN, HIGH);
      } 
      
      digitalWrite(LED_BUILTIN, HIGH);
    } else {
      digitalWrite(OUT_HEATER_PIN, heaterRunning ? HIGH: LOW);
      digitalWrite(OUT_PUMP_PIN, pumpRunning ? HIGH: LOW);
      updateBuiltinLed();
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
}

void stopHeat() {
  Serial.println("stopHeat");
  heaterRunning = false;
  digitalWrite(OUT_HEATER_PIN, LOW);
  heaterIdleStartedMs = timeNow;
  uint32_t heaterTime = timeNow - heaterStartedMs;
  heatStatistics[0] += heaterTime;
  updateBuiltinLed();
  saveEeprom();
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
  uint32_t pumped = msToMl(timeNow - pumpStartedMs);
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
bool dryTooLong() { return timeNow - lastWetMs > DRY_TOO_LONG_TIME; }

bool forceStoppedRecently() { return wasForceStopped && (timeNow - forceStopStartedMs < FORCE_STOP_TIME); }
bool motionStoppedRecently() { return wasMotionStopped && (timeNow - motionStopStartedMs < MOTION_STOP_TIME); }

bool stopHeaterTimePassed() { return timeNow - heaterStartedMs > HEATER_ON_TIME; }
bool heaterIdleTimePassed() { return timeNow - heaterIdleStartedMs > HEATER_IDLE_TIME; }
bool isTriggerTemp() { return temperature < TEMP_LIMIT; }
bool isAlarmTemp() { return temperature < TEMP_ALARM_LOW; }
bool isOperating() { return pumpRunning || heaterRunning; }
bool isWinter() { return true; }

bool cantStart() { return isWinter() || isTriggerTemp() || wetRecently() || forceStoppedRecently() || motionStoppedRecently(); }

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
  alarmRunning = (!isWinter() && dryTooLong()) || showBootInfo || tempSensorFail || isAlarmTemp() || (!isWinter() && (leftWater < 7.5 && !forceStoppedRecently()));
}

void alarmReason() {
  float leftWater = (CONTAINER_SIZE - pumpedTotal)/1000.0;
  Serial.println(leftWater);
  Serial.println(dryTooLong());
  Serial.println(isWinter());
  Serial.println(tempSensorFail);
}


void printStats() {
  for(uint16_t i = 0; i<24; i++) {
    Serial.println(pumpStatistics[i]);
  }
}
uint32_t counter = 0;
void setup() {
  wdt_enable(WDTO_2S);
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
  printBootInfo();
}

unsigned long millisAdd = 0;
unsigned long myMillis() {
  return millis() + millisAdd;
}

void loop() {
  wdt_reset();
  timeNow = epochAtStart + myMillis();
  dateTimeNow.setunixtime((timeNow / 1000) + EPOCH_OFFSET);

  if(dateTimeNow.day() != statisticsCurrentDay) {
    dayPassed();
    statisticsCurrentDay = dateTimeNow.day();
    saveEeprom();
  }

  // We do not want to fix clock (because it might jump backwards) during operations.
  // It would mess up time based volume etc. calculations
  if (timeNow - lastTimeClockCorrected > FIFTEEN_MINUTES && !isOperating()) {    
    int32_t correction = rtc.now().unixtime() - dateTimeNow.unixtime();
    epochAtStart += correction * 1000;
    timeNow = epochAtStart + myMillis();
    lastTimeClockCorrected = timeNow;
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
    tempLastRead = timeNow;
  }
  readInput();
  manageWaterPump();
  manageHeater();
  manageAlarm();
  updateLcd();
  manageBlink();
  updateBeeper();
  manageBuiltinLedBlink();
  counter++;
  if(false && counter % 100 == 0) {
    Serial.println("speed");
    Serial.println(counter);
    Serial.println(myMillis());
    Serial.println(1000*counter/myMillis());
    Serial.flush();
  }
  #ifdef USE_LOWPOWER
  if(!isBeeping()) {
    LowPower.powerDown(SLEEP_120MS, ADC_OFF, BOD_ON);
    millisAdd += 120;
    // LowPower disables, so let's re-enable.
    wdt_enable(WDTO_2S);
  }
  #endif
}