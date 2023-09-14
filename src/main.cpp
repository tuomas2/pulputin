// Copyright (C) 2023 Tuomas Airaksinen
// License: GPL. See GPL.txt for more info

#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>
#include <RTClib.h>


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

static const uint16_t OUT_PUMP_PIN = 2; // PWM possible
static const uint16_t ALARM_PIN = 3;

static const uint16_t MOTION_PIN = 52;
static const uint16_t MOTION_GROUND_PIN = 50;

bool waterLevel = false;
bool maxWaterLevel = false;
bool motionSns = false;

// How many ml water has been pumped each hour
// Latest is first item.
// Once an hour last item is removed and each item is moved one forward.
uint16_t pumpStatistics[24];
uint16_t pumpedTotal = 0;

// EEPROM addresses
static const uint16_t EEPROM_PUMP_STATISTICS = 0;
static uint8_t* EEPROM_CONFIGURED = reinterpret_cast<uint8_t *>(48);
static uint16_t* EEPROM_PUMP_TOTAL = reinterpret_cast<uint16_t *>(49);
static const void* EEPROM_LAST_HOUR_STARTED = reinterpret_cast<const void *>(51);
static const void* EEPROM_PUMP_STARTED = reinterpret_cast<const void *>(59);
static const void* EEPROM_IDLE_STARTED = reinterpret_cast<const void *>(67);
static const void* EEPROM_LAST_WET = reinterpret_cast<const void *>(75);
static uint8_t* EEPROM_STATS_CUR_DAY = reinterpret_cast<uint8_t *>(83);
static const uint16_t* EEPROM_LAST = reinterpret_cast<const uint16_t *>(83);

static const uint8_t EEPROM_CHECKVALUE = 0b10101010;

static const uint32_t EPOCH_OFFSET = 1694490000;

// Times, in millisecond (since starting device)
uint64_t epochAtStart = 0;
uint64_t timeNow = 0;
DateTime dateTimeNow;

uint64_t lastHourStarted = 0;
uint64_t pumpStartedMs = 0;
uint64_t idleStartedMs = 0;
uint64_t lastWetMs = 0;
uint64_t forceStopStartedMs = 0;
uint64_t motionStopStartedMs = 0;

uint8_t statisticsCurrentDay = 0;

bool wasMotionStopped = false;
bool wasForceStopped = false;
bool wasWet = false;
bool pumpRunning = false;

uint16_t minutesAgo(uint64_t timestamp) { return (timeNow - timestamp) / 1000 / 60; }

static const uint16_t PUMP_WATER_SPEED = 116;  // Pump speed, ml per 100 seconds

// Convert millilitres to milliseconds and vice versa
uint64_t mlToMs(uint32_t millilitres) { return 100000 * millilitres / PUMP_WATER_SPEED; }
uint32_t msToMl(uint64_t milliseconds) { return milliseconds * PUMP_WATER_SPEED / 100000; }

static const uint32_t ONE_HOUR = 3600000;
static const uint32_t ONE_MINUTE = ONE_HOUR/60;

static const uint16_t CONTAINER_SIZE = 28000;  // Water container size in (ml)

static const uint16_t PUMP_PORTION = 100;       // Amount of water pumped at once (ml)
static const uint32_t PERIOD_TIME = 15*ONE_MINUTE; // Adjusted water amount is PUMP_PORTION / PERIOD_TIME.


static const uint32_t PUMP_TIME = mlToMs(PUMP_PORTION);
static const uint32_t IDLE_TIME = PERIOD_TIME - PUMP_TIME;

static const uint32_t WET_TIME = ONE_HOUR;
static const uint32_t FORCE_STOP_TIME = ONE_HOUR;
static const uint32_t MOTION_STOP_TIME = ONE_MINUTE * 15;

LiquidCrystal_I2C lcd(0x3F, 16, 2);
DS3231 rtc;

void resetMaxWaterLevel() {
    maxWaterLevel = waterLevel;
}

bool wetRecently() { return wasWet && (timeNow - lastWetMs < WET_TIME); }
bool forceStoppedRecently() { return wasForceStopped && (timeNow - forceStopStartedMs < FORCE_STOP_TIME); }
bool motionStoppedRecently() { return wasMotionStopped && (timeNow - motionStopStartedMs < MOTION_STOP_TIME); }
bool cantStart() { return wetRecently() || forceStoppedRecently() || motionStoppedRecently(); }

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

    pinMode(MOTION_GROUND_PIN, OUTPUT);
    digitalWrite(MOTION_GROUND_PIN, LOW);

    pinMode(WATER_LEVEL_PIN, INPUT_PULLUP);

    digitalWrite(LED_BUILTIN, LOW);
    digitalWrite(ALARM_PIN, LOW);
    digitalWrite(OUT_PUMP_PIN, LOW);
}

void saveEeprom() {
    for (uint16_t i = 0; i < 24; i++) {
        eeprom_update_word(reinterpret_cast<uint16_t *>(EEPROM_PUMP_STATISTICS + i * 2), pumpStatistics[i]);
    }
    eeprom_update_word(EEPROM_PUMP_TOTAL, pumpedTotal);

    eeprom_update_block(&lastHourStarted, &EEPROM_LAST_HOUR_STARTED, 8);
    eeprom_update_block(&pumpStartedMs, &EEPROM_PUMP_STARTED, 8);
    eeprom_update_block(&idleStartedMs, &EEPROM_IDLE_STARTED, 8);
    eeprom_update_block(&lastWetMs, &EEPROM_LAST_WET, 8);

    eeprom_update_byte(EEPROM_STATS_CUR_DAY, statisticsCurrentDay);
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
}

void readEeprom() {
    if (eeprom_read_byte(EEPROM_CONFIGURED) != EEPROM_CHECKVALUE) {
        resetEEPROM();
    }
    for (uint16_t i = 0; i < 24; i++) {
        pumpStatistics[i] = eeprom_read_word(reinterpret_cast<const uint16_t *>(EEPROM_PUMP_STATISTICS + i * 2));
    }

    pumpedTotal = eeprom_read_word(EEPROM_PUMP_TOTAL);

    eeprom_read_block(&lastHourStarted, EEPROM_LAST_HOUR_STARTED, 8);
    eeprom_read_block(&pumpStartedMs, EEPROM_PUMP_STARTED, 8);
    eeprom_read_block(&idleStartedMs, EEPROM_IDLE_STARTED, 8);
    eeprom_read_block(&lastWetMs, EEPROM_LAST_WET, 8);

    statisticsCurrentDay = eeprom_read_byte(EEPROM_STATS_CUR_DAY);
}

void dayPassed() {
    for (int16_t i = 23; i > 0; i--) {
        pumpStatistics[i] = pumpStatistics[i - 1];
    }
    pumpStatistics[0] = 0;
}

static const uint16_t BUF_SIZE = 18;
char lcdBuf1[BUF_SIZE];
char lcdBuf2[BUF_SIZE];
char floatBuf1[BUF_SIZE];
char floatBuf2[BUF_SIZE];

void updateLcd() {
    bool showForceStop = !digitalRead(BUTTON4_PIN);
    bool showResetContainer = !digitalRead(BUTTON6_PIN);
    bool backlightBtn = !digitalRead(BUTTON3_PIN);
    bool showTimes = !digitalRead(BUTTON1_PIN);
    bool showContainer = !digitalRead(BUTTON5_PIN);
    double leftWater = (CONTAINER_SIZE - pumpedTotal)/1000.0;

    if(showContainer) {
        double pumpedTotalLitres = pumpedTotal / 1000.0;

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
        dtostrf((float)(pumpStatistics[0]/1000.0), 4, 1, floatBuf1);
        dtostrf((float)(pumpStatistics[1]/1000.0), 4, 1, floatBuf2);

        int32_t totalMinutes = minutesAgo(waterLevel ? pumpStartedMs: lastWetMs);
        int32_t hours = totalMinutes/60;
        int32_t minutesLeft = totalMinutes - hours*60;
        uint16_t waterRemainingPercent = (uint16_t)((double)(CONTAINER_SIZE - pumpedTotal - 1) / CONTAINER_SIZE)*100;
        snprintf(lcdBuf1, BUF_SIZE, "%s %s %luh %lum         ", floatBuf1, floatBuf2, hours, minutesLeft);
        snprintf(lcdBuf2, BUF_SIZE, "%2d%% %s%s%s %2u:%02u           ",
                 waterRemainingPercent,
                 waterLevel ? "We" : "Dr",
                 motionSns ? "Mo": "  ",
                 cantStart() ? "St" : "  ",
                 dateTimeNow.hour(), dateTimeNow.minute()
        );
    }

    if(backlightBtn || (leftWater < 3.0 && timeNow/100 % 100 == 0 && !forceStoppedRecently())) {
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

bool stopPumpTimePassed() { return timeNow - pumpStartedMs > PUMP_TIME;}
bool idleTimePassed() { return timeNow - idleStartedMs > IDLE_TIME; }

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
    for(unsigned int pumpStatistic : pumpStatistics) {
        Serial.println(pumpStatistic);
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
        int32_t correction = (int32_t)(rtc.now().unixtime() - dateTimeNow.unixtime());
        epochAtStart += correction * 1000;
        timeNow = epochAtStart + millis();

        lastHourStarted = timeNow;
        saveEeprom();
    }
    readInput();
    manageWaterPump();
    updateLcd();
}