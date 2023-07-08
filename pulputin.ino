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

static const int IN_MOISTURE_PIN = 0; // Analog 0
static const int IN_WATER_THROUGH_PIN = 22;

static const int OUT_PUMP_PIN = 3; // PWM possible

int lastHourStarted = 0;

static const int EEPROM_PUMP_STATISTICS=0;

uint8_t pumpStatistics[24]; // How many ml water has been pumped each hour
// Latest is first item. 
// Once an hour last item is removed and each item is moved one forward.

LiquidCrystal_I2C lcd(0x3F,16,2);

char lcdBuf[16];

static const int EEPROM_CONFIGURED = 24;
static const int EEPROM_LAST = 24;

static const int MILLILITRES_PER_MINUTE = 80;

static const byte IS_CONFIGURED = 0b10101010;

void setup()
{
  initializeStatistics();
  lcd.init();
  updateLcd();
  pinMode(BUTTON1_PIN, INPUT_PULLUP);
  pinMode(BUTTON2_PIN, INPUT_PULLUP);
  pinMode(BUTTON3_PIN, INPUT_PULLUP);
  pinMode(BUTTON4_PIN, INPUT_PULLUP);
  pinMode(BUTTON5_PIN, INPUT_PULLUP);
  pinMode(BUTTON6_PIN, INPUT_PULLUP);
  pinMode(BUTTON7_PIN, INPUT_PULLUP);
  pinMode(BUTTON8_PIN, INPUT_PULLUP);

  pinMode(IN_MOISTURE_PIN, INPUT);
  pinMode(IN_WATER_THROUGH_PIN, INPUT_PULLUP);
  pinMode(OUT_PUMP_PIN, OUTPUT);
  digitalWrite(OUT_PUMP_PIN, LOW);
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
  snprintf(lcdBuf, 16, "%d ml/24h", total);
  
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print(lcdBuf);
  lcd.setCursor(0,1);
  snprintf(lcdBuf, 16, "That's all");

  lcd.print(lcdBuf);
}


void loop()
{
}