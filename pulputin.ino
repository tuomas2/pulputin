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

static const int OUT_PUMP_PIN = LED_BUILTIN; // 2; // PWM possible

int lastHourStarted = 0;

static const int EEPROM_PUMP_STATISTICS=0;
static const int PUMP_PORTION = 10; // ml
static const int PUMP_WATER_SPEED = 133; // ml per 100 seconds

unsigned long timeNow = 0;

const int MAX_DRY_MOISTURE = 1; // percent

// During idle time
int minMoisture = 101;
int maxMoisture = -1;


uint8_t pumpStatistics[24]; // How many ml water has been pumped each hour
// Latest is first item. 
// Once an hour last item is removed and each item is moved one forward.

LiquidCrystal_I2C lcd(0x3F,16,2);

char lcdBuf[16];

static const int EEPROM_CONFIGURED = 24;
static const int EEPROM_LAST = 24;

static const int MILLILITRES_PER_MINUTE = 80;

static const byte IS_CONFIGURED = 0b10101010;


int moisture1Percent = 0;
int moisture2Percent = 0;

bool button1Pressed = false;
bool button2Pressed = false;

unsigned long pumpStartedMs = 0;
unsigned long idleStartedMs = 0;
bool pumpRunning = false;


// Convert millilitres to milliseconds
unsigned long mlToMs(int millilitres) {
  return 100000 * millilitres / PUMP_WATER_SPEED;
}

const unsigned long PUMP_TIME = mlToMs(PUMP_PORTION);
const unsigned long IDLE_TIME = PUMP_TIME * 10;


void setup()
{
  initializePins();
  initializeStatistics();
  lcd.init();
  readInput();
  updateLcd();
  startPump();
}

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
}

void initializeStatistics() {
  if(eeprom_read_byte(EEPROM_CONFIGURED) != IS_CONFIGURED) {
    resetEEPROM();
  }
  for(int i = 0; i< 24; i++) {
    pumpStatistics[i] = eeprom_read_byte(EEPROM_PUMP_STATISTICS + i);
  }
}

void savePumpStatistics() {
  for(int i = 0; i< 24; i++) {
    eeprom_write_byte(EEPROM_PUMP_STATISTICS + i, pumpStatistics[i]);
  }
}

void hourPassed() {
  for(int i = 23; i>0; i--) {
    pumpStatistics[i] = pumpStatistics[i-1];
  }
  pumpStatistics[0] = 0;
  savePumpStatistics();
}

void resetEEPROM() {
  for(int i = 0; i < EEPROM_LAST; i++) {
    eeprom_write_byte(i, 0);
  }
  eeprom_write_byte(EEPROM_CONFIGURED, IS_CONFIGURED);
}

void updateLcd() {
  int total = 0;
  for(int i = 0; i< 24; i++) {
    total += pumpStatistics[i];
  }
  snprintf(lcdBuf, 16, "%d ml/d", total);
  lcd.setCursor(0,0);
  lcd.print(lcdBuf);
  lcd.setCursor(0,1);
  if(button1Pressed) {
    snprintf(lcdBuf, 16, "m1: %d%% m2: %d%%            ", moisture1Percent, moisture2Percent);
  } else {
    snprintf(lcdBuf, 16, "max moist %d%%               ", maxMoisture);
  }
  lcd.print(lcdBuf);
}

void readInput() {
  bool button1Pressed = !digitalRead(BUTTON1_PIN);
  bool b2 = !digitalRead(BUTTON2_PIN);
  if(b2 != button2Pressed) {
    resetEEPROM();
    initializeStatistics();
  }
  button2Pressed = b2;

  int moist1 = analogRead(IN_MOISTURE1_PIN);
  int moist2 = analogRead(IN_MOISTURE2_PIN);

  moisture1Percent = 100-(int)((float)moist1/1023.*100);
  moisture2Percent = 100-(int)((float)moist2/1023.*100);
}

void startPump() {
  pumpRunning = true;
  pumpStartedMs = timeNow;
  idleStartedMs = 0;
  digitalWrite(OUT_PUMP_PIN, HIGH);
  resetMoisture();
}

void stopPump() {
  pumpRunning = false;
  digitalWrite(OUT_PUMP_PIN, LOW);
  pumpStatistics[0] += PUMP_PORTION;
  savePumpStatistics();
  pumpStartedMs = 0;
  idleStartedMs = timeNow;
}


void updateMoisture() {
  if(moisture1Percent < minMoisture) {
    minMoisture = moisture1Percent;
  }
  if(moisture1Percent > maxMoisture) {
    maxMoisture = moisture1Percent;
  }
}

void resetMoisture() {
  minMoisture = 101;
  maxMoisture = -1;
  updateMoisture();
}

bool stopPumpTimePassed() {
  return timeNow - pumpStartedMs > PUMP_TIME; 
}

bool idleTimePassed() {
  return timeNow - idleStartedMs > IDLE_TIME;
}

void manageWaterPump() {
  updateMoisture();
  
  if (pumpRunning) {
    if (stopPumpTimePassed()) {
      stopPump();
    }
  } else {
    if (idleTimePassed() && maxMoisture < MAX_DRY_MOISTURE) {
      startPump();
    }
  }
}

void loop() {
  timeNow = millis();
  readInput();
  manageWaterPump();
  updateLcd();
}