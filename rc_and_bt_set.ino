#include <RCSwitch.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <WiFi.h>
#include "time.h"
#include <EEPROM.h>

// --- НАСТРОЙКИ ПАМЯТИ EEPROM ---
#define EEPROM_SIZE 5
#define ADDR_OPEN_HOUR 0
#define ADDR_OPEN_MIN  1
#define ADDR_CLOSE_HOUR 2
#define ADDR_CLOSE_MIN  3
#define ADDR_SCHEDULE_ENABLED 4

// --- НАСТРОЙКИ ПИНОВ ---
const int receiverPin = 15; 
const int motorPin1 = 14; 
const int motorPin2 = 27; 
const int standbyPin = 12;

// --- КОДЫ ПУЛЬТА ---
const unsigned long FORWARD_CODE = 504520; 
const unsigned long BACKWARD_CODE = 504514; 
const unsigned long CONFIG_CODE = 504516;

const int motorRunDuration = 3000;
const char* ssid = "...";        
const char* pass = "yooooooo";    
const char* ntpServer = "pool.ntp.org"; 
const long gmtOffset_sec = 3 * 3600; 
const int daylightOffset_sec = 0;

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"

// --- ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ---
RCSwitch mySwitch = RCSwitch();
bool isBleActive = false;
bool scheduleEnabled = false;
int openHour = -1, openMinute = -1, closeHour = -1, closeMinute = -1;
unsigned long lastRfTime = 0;
const unsigned long debounceDelay = 1000;
BLEServer *pServer = NULL;

// --- АППАРАТНЫЙ ТАЙМЕР ---
hw_timer_t * waitTimer = NULL;
volatile bool waitTimerTriggered = false; 
volatile bool waitDirectionForward = true;

)
void IRAM_ATTR onWaitTimer() {
  waitTimerTriggered = true; 
}

// --- ПРЕДВАРИТЕЛЬНОЕ ОБЪЯВЛЕНИЕ ФУНКЦИЙ ---
void runMotor(bool forward);
void startBluetoothConfig();
void stopBluetoothConfig();

// --- КЛАСС ДЛЯ ОБРАБОТКИ BLE ---
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      Serial.println("\n[BLE] >>> Входящее сообщение от телефона <<<");
      String command = pCharacteristic->getValue();
      command.trim(); command.toUpperCase();
      Serial.print("[BLE] Команда: '"); Serial.print(command); Serial.println("'");

      int space1 = command.indexOf(' ');

      if (space1 > 0) {
        String type = command.substring(0, space1);

       
        if (type == "OPEN" || type == "CLOSE") {
          int colonIndex = command.indexOf(':', space1);
          if (colonIndex > space1) {
            int hour = command.substring(space1 + 1, colonIndex).toInt();
            int minute = command.substring(colonIndex + 1).toInt();   
            
            if (type == "OPEN") {
              openHour = hour; openMinute = minute; scheduleEnabled = true;
              EEPROM.write(ADDR_OPEN_HOUR, openHour);
              EEPROM.write(ADDR_OPEN_MIN, openMinute);
              EEPROM.write(ADDR_SCHEDULE_ENABLED, 1);
              EEPROM.commit();
              Serial.printf("[EEPROM] Сохранено OPEN: %02d:%02d\n", openHour, openMinute);
            } else {
              closeHour = hour; closeMinute = minute; scheduleEnabled = true;
              EEPROM.write(ADDR_CLOSE_HOUR, closeHour);
              EEPROM.write(ADDR_CLOSE_MIN, closeMinute);
              EEPROM.write(ADDR_SCHEDULE_ENABLED, 1);
              EEPROM.commit();
              Serial.printf("[EEPROM] Сохранено CLOSE: %02d:%02d\n", closeHour, closeMinute);
            }
          }
        }
        
       
        else if (type == "WAIT") {
          int space2 = command.indexOf(' ', space1 + 1);
          if (space2 > space1) {
            int waitSeconds = command.substring(space1 + 1, space2).toInt();
            String dirStr = command.substring(space2 + 1);
            dirStr.trim();

            if (waitSeconds > 0 && (dirStr == "F" || dirStr == "B")) {
              bool isForward = (dirStr == "F");
              Serial.printf("[TIMER] Настройка аппаратного таймера на %d секунд...\n", waitSeconds);

              // --- ЖЕЛЕЗНЫЙ СБРОС И ЗАПУСК ТАЙМЕРА ---
              if (waitTimer != NULL) {
                timerEnd(waitTimer);
                waitTimer = NULL;
              }
              
              // Создаем таймер: частота 1 000 000 Гц 
              waitTimer = timerBegin(1000000); 
              // Привязываем функцию-прерывание
              timerAttachInterrupt(waitTimer, &onWaitTimer);
              
              waitDirectionForward = isForward;
              waitTimerTriggered = false; 
              
              
              
              timerAlarm(waitTimer, (uint64_t)waitSeconds * 1000000ULL, false, 0);
              Serial.println("[TIMER] Таймер запущен!");

            }
          }
        }
      }
    }
};
MyCallbacks bleCallbacks;

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { Serial.println("\n[BLE] Устройство подключилось!"); }
    void onDisconnect(BLEServer* pServer) {
      Serial.println("\n[BLE] Устройство отключилось!");
      pServer->getAdvertising()->start();
    }
};

void setup() {
  Serial.begin(115200);
  pinMode(motorPin1, OUTPUT); pinMode(motorPin2, OUTPUT); pinMode(standbyPin, OUTPUT);
  digitalWrite(standbyPin, LOW);

  // Инициализация прерывания для радиоприемника
  mySwitch.enableReceive(digitalPinToInterrupt(receiverPin));

  EEPROM.begin(EEPROM_SIZE);
  if (EEPROM.read(ADDR_SCHEDULE_ENABLED) == 1) {
    scheduleEnabled = true;
    openHour = EEPROM.read(ADDR_OPEN_HOUR);
    openMinute = EEPROM.read(ADDR_OPEN_MIN);
    closeHour = EEPROM.read(ADDR_CLOSE_HOUR);
    closeMinute = EEPROM.read(ADDR_CLOSE_MIN);
  }

  BLEDevice::init("Curtain_Config");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE);
  pRxCharacteristic->setCallbacks(&bleCallbacks);
  pService->start();
  
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  Serial.println("Ready.");
}

void loop() {
  // 1. ОБРАБОТКА АППАРАТНОГО ТАЙМЕРА
  if (waitTimerTriggered) {
    waitTimerTriggered = false; // Сбрасываем флаг прерывания
    Serial.println("\n[TIMER] !!! СРАБОТАЛО ПРЕРЫВАНИЕ ТАЙМЕРА !!! Запускаю мотор.");
    runMotor(waitDirectionForward);
  }

  // 2. ОБРАБОТКА BLE
  if (isBleActive) {
    if (mySwitch.available()) {
      if (millis() - lastRfTime > debounceDelay) {
        unsigned long receivedCode = mySwitch.getReceivedValue();
        lastRfTime = millis();
        if (receivedCode == CONFIG_CODE) stopBluetoothConfig();
      }
      mySwitch.resetAvailable();
    }
    return;
  }

  // 3. ОБРАБОТКА РАДИО ПУЛЬТА
  if (mySwitch.available()) {
    if (millis() - lastRfTime > debounceDelay) {
      unsigned long receivedCode = mySwitch.getReceivedValue();
      lastRfTime = millis();
      
      if (receivedCode == FORWARD_CODE) runMotor(true);
      else if (receivedCode == BACKWARD_CODE) runMotor(false);
      else if (receivedCode == CONFIG_CODE) startBluetoothConfig();
    }
    mySwitch.resetAvailable();
  }

  // 4. ОБРАБОТКА РАСПИСАНИЯ
  if (scheduleEnabled) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      if (timeinfo.tm_hour == openHour && timeinfo.tm_min == openMinute) {
        runMotor(true);
        openHour = -1; 
        EEPROM.write(ADDR_OPEN_HOUR, -1); EEPROM.commit();
      }
      if (timeinfo.tm_hour == closeHour && timeinfo.tm_min == closeMinute) {
        runMotor(false);
        closeHour = -1;
        EEPROM.write(ADDR_CLOSE_HOUR, -1); EEPROM.commit();
      }
    }
  }
}

// --- ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ---
void runMotor(bool forward) {
  digitalWrite(standbyPin, HIGH);
  if (forward) {
    digitalWrite(motorPin1, HIGH); digitalWrite(motorPin2, LOW);
  } else {
    digitalWrite(motorPin1, LOW); digitalWrite(motorPin2, HIGH);
  }
  delay(motorRunDuration);
  digitalWrite(standbyPin, LOW);
  Serial.println("[MOTOR] Остановлен.");
}

void startBluetoothConfig() {
  if (isBleActive) return;
  WiFi.mode(WIFI_OFF); delay(100);
  isBleActive = true;
  pServer->getAdvertising()->start();
  Serial.println("[BLE] Реклама включена.");
}

void stopBluetoothConfig() {
  pServer->getAdvertising()->stop();
  isBleActive = false;
  if (WiFi.status() != WL_CONNECTED) WiFi.begin(ssid, pass);
  Serial.println("[BLE] Реклама выключена.");
}
