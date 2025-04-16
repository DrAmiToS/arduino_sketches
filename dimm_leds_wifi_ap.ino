#include <FastLED.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>

#define LED_PIN     16
#define NUM_LEDS    60
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB
#define DIM_PIN     14
#define OFF_PIN     12
#define ANALOG_PIN  A0  // Аналоговый датчик освещения
#define ARROW_PINS  4   // Количество стрелок

// Пины для стрелок (D1-D4)
const uint8_t arrowPins[ARROW_PINS] = {5, 4, 0, 2};

#pragma pack(push, 1)
struct Settings {
  // WiFi settings
  char ssid[32] = "Led_GX90";
  char password[32] = "dsaEW775688";
  
  // Light settings
  uint8_t brightness = 100;
  CRGB color = CRGB::White;
  
  // LED brightness adjustments (0-100%, default 100%)
  uint8_t ledAdjustments[NUM_LEDS] = {100};
  
  // Настройки стрелок
  struct {
    uint8_t brightness = 100; // Общая яркость стрелок (0-100%)
    uint8_t individualBrightness[ARROW_PINS] = {100}; // Яркость отдельных стрелок
    uint8_t dimFactor = 40; // Коэффициент приглушения (0-100%)
  } arrows;
  
  // Triggers
  struct {
    uint8_t dimMode = 0; // 0 - disabled, 1 - digital, 2 - analog
    uint8_t dimPinLevel = LOW;
    uint16_t dimThreshold = 512;
    uint16_t dimHysteresis = 50; // Гистерезис для аналогового датчика
    float dimFactor = 0.4f;
    
    uint8_t analogDimLevel = LOW;
    uint8_t offPinLevel = LOW;
    uint16_t offThreshold = 512;
  } triggers;
  
  // Timing
  struct {
    uint16_t fadeInTime = 3500;
    uint16_t fadeOutTime = 3000;
    uint16_t dimTime = 2000;
    uint16_t normalTime = 500;
  } timing;
  
  uint16_t crc = 0;
};
#pragma pack(pop)

Settings settings;
CRGB leds[NUM_LEDS];

// System state
int currentBrightness = 0;
int targetBrightness = 0;
unsigned long fadeStartTime = 0;
int startBrightness = 0;
bool isFading = false;
bool dimmingActive = false;
bool powerOn = true;
int lastAnalogValue = 0;
bool analogDimState = false;

// Состояние стрелок
int arrowBrightness[ARROW_PINS] = {0};
int arrowTargetBrightness[ARROW_PINS] = {0};
int arrowStartBrightness[ARROW_PINS] = {0};
unsigned long arrowFadeStartTime[ARROW_PINS] = {0};
bool arrowIsFading[ARROW_PINS] = {false};

ESP8266WebServer server(80);

// Function prototypes
uint16_t calculateCRC(const Settings& data);
void resetSettings();
void loadSettings();
void saveSettings();
void updateLEDs(bool forceUpdate = false);
void startFade(int newBrightness, unsigned long fadeDuration);
void updateFade();
void checkTriggerPins();
void checkAnalogSensor();
void startArrowFade(uint8_t arrowIndex, int newBrightness, unsigned long fadeDuration);
void updateArrowFades();
void setArrowPWM(uint8_t arrowIndex, uint8_t brightness);

void setup() {
  Serial.begin(115200);
  EEPROM.begin(sizeof(Settings) + 10);
  
  loadSettings();
  
  // Setup WiFi AP
  WiFi.softAP(settings.ssid, settings.password);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
  Serial.print("send 'reset eeprom' to reset eeprom");

  // Setup OTA
  ArduinoOTA.begin();
  
  // Initialize LEDs
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.clear();
  FastLED.show();

  // Setup pins
  pinMode(DIM_PIN, INPUT_PULLUP);
  pinMode(OFF_PIN, INPUT_PULLUP);
  pinMode(ANALOG_PIN, INPUT);
  
  // Настройка пинов для стрелок
  for (int i = 0; i < ARROW_PINS; i++) {
    pinMode(arrowPins[i], OUTPUT);
    analogWrite(arrowPins[i], 0); // Изначально выключены
  }

  // Setup web server
  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_GET, handleGetSettings);
  server.on("/settings", HTTP_POST, handlePostSettings);
  server.on("/control", HTTP_POST, handleControl);
  server.on("/reset", HTTP_POST, handleReset);
  server.on("/pinstatus", HTTP_GET, handlePinStatus);
  server.on("/admin", HTTP_GET, handleAdmin);
  server.on("/admin/reset", HTTP_POST, handleAdminReset);
  server.on("/ledadjust", HTTP_GET, handleGetLEDAdjust);
  server.on("/ledadjust", HTTP_POST, handlePostLEDAdjust);
  server.on("/arrowsettings", HTTP_GET, handleGetArrowSettings);
  server.on("/arrowsettings", HTTP_POST, handlePostArrowSettings);
  server.onNotFound(handleNotFound);
  
  server.begin();
  
  // Initialize all LED adjustments to 100% if not set
  bool needsSave = false;
  for(int i = 0; i < NUM_LEDS; i++) {
    if(settings.ledAdjustments[i] == 0) {
      settings.ledAdjustments[i] = 100;
      needsSave = true;
    }
  }
  
  // Initialize arrow brightness adjustments if not set
  for(int i = 0; i < ARROW_PINS; i++) {
    if(settings.arrows.individualBrightness[i] == 0) {
      settings.arrows.individualBrightness[i] = 100;
      needsSave = true;
    }
  }
  
  if(needsSave) saveSettings();
  
  // Fade in on start
  startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.fadeInTime);
  
  // Запускаем плавное включение стрелок
  for(int i = 0; i < ARROW_PINS; i++) {
    startArrowFade(i, map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100, 0, 100, 0, 255), 
                  settings.timing.fadeInTime);
  }
  
  // Проверяем состояние пинов при запуске
  checkTriggerPins();
  // Читаем начальное значение с аналогового датчика
  lastAnalogValue = analogRead(ANALOG_PIN);
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  
  static bool lastDimState = digitalRead(DIM_PIN);
  static bool lastOffState = digitalRead(OFF_PIN);
  
  // Read digital values for triggers
  bool currentDimState = digitalRead(DIM_PIN);
  bool currentOffState = digitalRead(OFF_PIN);
  
  // Handle dimming (only if digital mode is selected)
  if(settings.triggers.dimMode == 1 && currentDimState != lastDimState && powerOn) {
    if(currentDimState == (settings.triggers.dimPinLevel == LOW) && !dimmingActive) {
      dimmingActive = true;
      startFade(map(settings.brightness, 0, 100, 0, 255) * settings.triggers.dimFactor, 
                settings.timing.dimTime);
      // Плавное затемнение стрелок
      for(int i = 0; i < ARROW_PINS; i++) {
        startArrowFade(i, map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100 * settings.arrows.dimFactor / 100, 
                            0, 100, 0, 255), settings.timing.dimTime);
      }
    } 
    else if(currentDimState != (settings.triggers.dimPinLevel == LOW) && dimmingActive) {
      dimmingActive = false;
      startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.normalTime);
      // Плавное восстановление яркости стрелок
      for(int i = 0; i < ARROW_PINS; i++) {
        startArrowFade(i, map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100, 
                            0, 100, 0, 255), settings.timing.normalTime);
      }
    }
    lastDimState = currentDimState;
    delay(50);
  }
  
  // Handle analog sensor
  if(settings.triggers.dimMode == 2) {
    checkAnalogSensor();
  }
  
  // Handle power
  if(currentOffState != lastOffState) {
    if(currentOffState == (settings.triggers.offPinLevel == LOW)) {
      if(powerOn) {
        powerOn = false;
        startFade(0, settings.timing.fadeOutTime);
        // Плавное выключение стрелок
        for(int i = 0; i < ARROW_PINS; i++) {
          startArrowFade(i, 0, settings.timing.fadeOutTime);
        }
      } else {
        powerOn = true;
        startFade(dimmingActive ? 
                 map(settings.brightness, 0, 100, 0, 255) * settings.triggers.dimFactor : 
                 map(settings.brightness, 0, 100, 0, 255), 
                 settings.timing.fadeInTime);
        // Плавное включение стрелок
        for(int i = 0; i < ARROW_PINS; i++) {
          startArrowFade(i, dimmingActive ? 
                        map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100 * settings.arrows.dimFactor / 100, 
                            0, 100, 0, 255) :
                        map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100, 
                            0, 100, 0, 255), 
                        settings.timing.fadeInTime);
        }
      }
    }
    lastOffState = currentOffState;
    delay(50);
  }
  
  updateFade();
  updateArrowFades();

  // Handle console commands
  if(Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if(cmd == "reset eeprom") {
      Serial.println("Resetting EEPROM to defaults...");
      resetSettings();
      Serial.println("Done. Restart to apply changes.");
    }
  }
}

// ========== LED Control Functions ==========
void startFade(int newBrightness, unsigned long fadeDuration) {
  startBrightness = currentBrightness;
  targetBrightness = newBrightness;
  fadeStartTime = millis();
  isFading = true;
}

void updateFade() {
  if(!isFading) return;
  
  unsigned long progress = millis() - fadeStartTime;
  unsigned long fadeTime = settings.timing.normalTime;
  
  if(targetBrightness == 0) {
    fadeTime = settings.timing.fadeOutTime;
  } else if(startBrightness == 0 && targetBrightness > 0) {
    fadeTime = settings.timing.fadeInTime;
  } else if(dimmingActive && targetBrightness < startBrightness) {
    fadeTime = settings.timing.dimTime;
  }
  
  if(progress >= fadeTime) {
    currentBrightness = targetBrightness;
    isFading = false;
  } else {
    float ratio = (float)progress / fadeTime;
    ratio = ratio * ratio * (3.0 - 2.0 * ratio);
    currentBrightness = startBrightness + (targetBrightness - startBrightness) * ratio;
  }
  
  updateLEDs(true); // Всегда обновляем с forceUpdate
}

void checkAnalogSensor() {
    static unsigned long lastReadTime = 0;
    const unsigned long readInterval = 100; // Чтение каждые 100 мс
    
    if(millis() - lastReadTime < readInterval) return;
    lastReadTime = millis();

    int currentValue = analogRead(ANALOG_PIN);
    
    // Защита от некорректных значений
    if(currentValue < 0 || currentValue > 1023) {
        Serial.println("Invalid analog value!");
        return;
    }

    if(settings.triggers.analogDimLevel == LOW) {
        // Режим LOW - срабатывание при значении НИЖЕ порога
        if(!analogDimState) {
            if(currentValue <= settings.triggers.dimThreshold - settings.triggers.dimHysteresis) {
                analogDimState = true;
                dimmingActive = true;
                startFade(map(settings.brightness, 0, 100, 0, 255) * settings.triggers.dimFactor, 
                          settings.timing.dimTime);
                // Плавное затемнение стрелок
                for(int i = 0; i < ARROW_PINS; i++) {
                  startArrowFade(i, map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100 * settings.arrows.dimFactor / 100, 
                                      0, 100, 0, 255), settings.timing.dimTime);
                }
            }
        } else {
            if(currentValue >= settings.triggers.dimThreshold + settings.triggers.dimHysteresis) {
                analogDimState = false;
                dimmingActive = false;
                startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.normalTime);
                // Плавное восстановление яркости стрелок
                for(int i = 0; i < ARROW_PINS; i++) {
                  startArrowFade(i, map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100, 
                                      0, 100, 0, 255), settings.timing.normalTime);
                }
            }
        }
    } else {
        // Режим HIGH - срабатывание при значении ВЫШЕ порога
        if(!analogDimState) {
            if(currentValue >= settings.triggers.dimThreshold + settings.triggers.dimHysteresis) {
                analogDimState = true;
                dimmingActive = true;
                startFade(map(settings.brightness, 0, 100, 0, 255) * settings.triggers.dimFactor, 
                          settings.timing.dimTime);
                // Плавное затемнение стрелок
                for(int i = 0; i < ARROW_PINS; i++) {
                  startArrowFade(i, map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100 * settings.arrows.dimFactor / 100, 
                                      0, 100, 0, 255), settings.timing.dimTime);
                }
            }
        } else {
            if(currentValue <= settings.triggers.dimThreshold - settings.triggers.dimHysteresis) {
                analogDimState = false;
                dimmingActive = false;
                startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.normalTime);
                // Плавное восстановление яркости стрелок
                for(int i = 0; i < ARROW_PINS; i++) {
                  startArrowFade(i, map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100, 
                                      0, 100, 0, 255), settings.timing.normalTime);
                }
            }
        }
    }
    
    lastAnalogValue = currentValue;
}

void checkTriggerPins() {
  bool currentDimState = digitalRead(DIM_PIN);
  bool currentOffState = digitalRead(OFF_PIN);
  
  // Проверка состояния пина затемнения (только для цифрового режима)
  if(settings.triggers.dimMode == 1) {
    if(currentDimState == (settings.triggers.dimPinLevel == LOW)) {
      dimmingActive = true;
      startFade(map(settings.brightness, 0, 100, 0, 255) * settings.triggers.dimFactor, 
                settings.timing.dimTime);
      // Плавное затемнение стрелок
      for(int i = 0; i < ARROW_PINS; i++) {
        startArrowFade(i, map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100 * settings.arrows.dimFactor / 100, 
                            0, 100, 0, 255), settings.timing.dimTime);
      }
    } else {
      dimmingActive = false;
      startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.normalTime);
      // Плавное восстановление яркости стрелок
      for(int i = 0; i < ARROW_PINS; i++) {
        startArrowFade(i, map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100, 
                        0, 100, 0, 255), settings.timing.normalTime);
      }
    }
  }
  
  // Проверка состояния пина выключения
  if(currentOffState == (settings.triggers.offPinLevel == LOW)) {
    powerOn = false;
    startFade(0, settings.timing.fadeOutTime);
    // Плавное выключение стрелок
    for(int i = 0; i < ARROW_PINS; i++) {
      startArrowFade(i, 0, settings.timing.fadeOutTime);
    }
  } else {
    powerOn = true;
    startFade(dimmingActive ? 
             map(settings.brightness, 0, 100, 0, 255) * settings.triggers.dimFactor : 
             map(settings.brightness, 0, 100, 0, 255), 
             settings.timing.fadeInTime);
    // Плавное включение стрелок
    for(int i = 0; i < ARROW_PINS; i++) {
      startArrowFade(i, dimmingActive ? 
                    map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100 * settings.arrows.dimFactor / 100, 
                        0, 100, 0, 255) :
                    map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100, 
                        0, 100, 0, 255), 
                    settings.timing.fadeInTime);
    }
  }
}

void updateLEDs(bool forceUpdate) {
  for(int i = 0; i < NUM_LEDS; i++) {
    // Применяем индивидуальную яркость
    uint8_t adjustedBrightness = currentBrightness * settings.ledAdjustments[i] / 100;
    leds[i] = settings.color;
    leds[i].fadeToBlackBy(255 - adjustedBrightness);
  }
  FastLED.show();
}

// ========== Arrow Control Functions ==========
void startArrowFade(uint8_t arrowIndex, int newBrightness, unsigned long fadeDuration) {
  if(arrowIndex >= ARROW_PINS) return;
  
  arrowStartBrightness[arrowIndex] = arrowBrightness[arrowIndex];
  arrowTargetBrightness[arrowIndex] = newBrightness;
  arrowFadeStartTime[arrowIndex] = millis();
  arrowIsFading[arrowIndex] = true;
  
  // Если продолжительность 0 - мгновенное изменение
  if(fadeDuration == 0) {
    arrowBrightness[arrowIndex] = newBrightness;
    arrowIsFading[arrowIndex] = false;
    setArrowPWM(arrowIndex, arrowBrightness[arrowIndex]);
  }
}

void updateArrowFades() {
  for(int i = 0; i < ARROW_PINS; i++) {
    if(!arrowIsFading[i]) continue;
    
    unsigned long progress = millis() - arrowFadeStartTime[i];
    unsigned long fadeTime = settings.timing.normalTime;
    
    if(arrowTargetBrightness[i] == 0) {
      fadeTime = settings.timing.fadeOutTime;
    } else if(arrowStartBrightness[i] == 0 && arrowTargetBrightness[i] > 0) {
      fadeTime = settings.timing.fadeInTime;
    } else if(dimmingActive && arrowTargetBrightness[i] < arrowStartBrightness[i]) {
      fadeTime = settings.timing.dimTime;
    }
    
    if(progress >= fadeTime) {
      arrowBrightness[i] = arrowTargetBrightness[i];
      arrowIsFading[i] = false;
    } else {
      float ratio = (float)progress / fadeTime;
      ratio = ratio * ratio * (3.0 - 2.0 * ratio);
      arrowBrightness[i] = arrowStartBrightness[i] + (arrowTargetBrightness[i] - arrowStartBrightness[i]) * ratio;
    }
    
    setArrowPWM(i, arrowBrightness[i]);
  }
}

void setArrowPWM(uint8_t arrowIndex, uint8_t brightness) {
  if(arrowIndex >= ARROW_PINS) return;
  
  // Применяем индивидуальную яркость стрелки
  uint8_t adjustedBrightness = brightness * settings.arrows.individualBrightness[arrowIndex] / 100;
  
  // ESP8266 использует 10-битное ШИМ (0-1023), но analogWrite принимает 0-255
  // Поэтому нам нужно масштабировать значение
  uint8_t pwmValue = map(adjustedBrightness, 0, 255, 0, 1023) / 4;
  analogWrite(arrowPins[arrowIndex], pwmValue);
}

// ========== EEPROM Functions ==========
uint16_t calculateCRC(const Settings& data) {
  const uint8_t* p = (const uint8_t*)&data;
  uint16_t crc = 0xFFFF;
  for(size_t i = 0; i < sizeof(Settings) - sizeof(data.crc); i++) {
    crc ^= (uint16_t)p[i] << 8;
    for(uint8_t j = 0; j < 8; j++) {
      if(crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else crc <<= 1;
    }
  }
  return crc;
}

void loadSettings() {
  EEPROM.get(0, settings);
  
  // Verify CRC
  if(calculateCRC(settings) != settings.crc) {
    Serial.println("EEPROM CRC mismatch, resetting to defaults");
    resetSettings();
  }
}

void saveSettings() {
  settings.crc = calculateCRC(settings);
  EEPROM.put(0, settings);
  if(!EEPROM.commit()) {
    Serial.println("EEPROM commit failed!");
  }
}

void resetSettings() {
  memset(&settings, 0, sizeof(Settings));
  
  // Default values
  strcpy(settings.ssid, "Led_GX90");
  strcpy(settings.password, "dsaEW775688");
  settings.brightness = 100;
  settings.color = CRGB::White;
  
  // Initialize all LED adjustments to 100%
  for(int i = 0; i < NUM_LEDS; i++) {
    settings.ledAdjustments[i] = 100;
  }
  
  // Initialize arrow settings
  settings.arrows.brightness = 100;
  settings.arrows.dimFactor = 40;
  for(int i = 0; i < ARROW_PINS; i++) {
    settings.arrows.individualBrightness[i] = 100;
  }
  
  settings.triggers.dimMode = 0; // По умолчанию выключено
  settings.triggers.dimPinLevel = LOW;
  settings.triggers.analogDimLevel = LOW;
  settings.triggers.dimThreshold = 512;
  settings.triggers.dimHysteresis = 50;
  settings.triggers.dimFactor = 0.4f;
  settings.triggers.offPinLevel = LOW;
  settings.triggers.offThreshold = 512;
  
  settings.timing.fadeInTime = 3500;
  settings.timing.fadeOutTime = 3000;
  settings.timing.dimTime = 2000;
  settings.timing.normalTime = 500;
  
  saveSettings();
}

// ========== Web Server Handlers ==========

const char MAIN_page[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>LED Controller</title>
  <style>

    :root {
      --primary-color: #3498db;
      --danger-color: #e74c3c;
      --text-color: #333;
      --border-color: #ccc;
      --bg-color: #f9f9f9;
    }

    * {
      box-sizing: border-box;
      margin: 0;
      padding: 0;
    }

    body {
      font-family: system-ui, -apple-system, sans-serif;
      margin: 0 auto;
      max-width: 1200px;
      padding: 1rem;
      color: var(--text-color);
      line-height: 1.6;
      background: #fafafa;
    }
    
    h1, h2, h3 {
      color: #2c3e50;
      margin: 1rem 0;
    }

    .tab {
      display: flex;
      flex-wrap: wrap;
      gap: 0.25rem;
      border: 1px solid var(--border-color);
      background-color: var(--bg-color);
      border-radius: 0.5rem 0.5rem 0 0;
      padding: 0.5rem;
    }
    
    .tab button {
      color: var(--text-color);
      background-color: transparent;
      border: none;
      outline: none;
      cursor: pointer;
      padding: 0.75rem 1rem;
      transition: all 0.3s ease;
      border-radius: 0.25rem;
      flex: 1;
      min-width: 120px;
      font-size: 0.9rem;
    }
    
    .tab button:hover {
      background-color: rgba(0,0,0,0.05);
    }
    
    .tab button.active {
      background-color: var(--primary-color);
      color: white;
    }
    
    .tabcontent {
      display: none;
      padding: 1.25rem;
      border: 1px solid var(--border-color);
      border-top: none;
      border-radius: 0 0 0.5rem 0.5rem;
      background-color: white;
      box-shadow: 0 2px 8px rgba(0,0,0,0.05);
    }
    
    .control-group {
      margin-bottom: 1.5rem;
      padding: 1.25rem;
      background-color: var(--bg-color);
      border-radius: 0.5rem;
      border: 1px solid var(--border-color);
    }
    
    .control-row {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      margin-bottom: 1rem;
    }
    
    .control-label {
      min-width: 140px;
      font-weight: 500;
    }
    
    .control-input {
      flex: 1;
      min-width: 200px;
      display: flex;
      align-items: center;
      gap: 0.75rem;
    }
    
    .number-input {
      width: 80px;
      padding: 0.5rem;
      border: 1px solid var(--border-color);
      border-radius: 0.25rem;
      text-align: center;
    }
    
    .unit {
      color: #666;
      min-width: 50px;
    }
    
    .slider {
      flex: 1;
      height: 8px;
      background: #ddd;
      border-radius: 1rem;
      outline: none;
      -webkit-appearance: none;
    }
    
    .slider::-webkit-slider-thumb {
      -webkit-appearance: none;
      width: 18px;
      height: 18px;
      border-radius: 50%;
      background: var(--primary-color);
      cursor: pointer;
      border: 2px solid white;
      box-shadow: 0 1px 3px rgba(0,0,0,0.2);
    }
    
    .color-preview {
      width: 100%;
      height: 60px;
      border: 1px solid var(--border-color);
      border-radius: 0.5rem;
      margin: 0.75rem 0;
    }
    
    .notification {
      position: fixed;
      top: 1.25rem;
      left: 50%;
      transform: translateX(-50%);
      background: rgba(0,0,0,0.85);
      color: white;
      padding: 0.75rem 1.5rem;
      border-radius: 0.5rem;
      display: none;
      z-index: 1000;
      box-shadow: 0 4px 12px rgba(0,0,0,0.15);
    }
    
    .pin-status {
      display: flex;
      justify-content: space-between;
      margin: 0.75rem 0;
      padding: 0.75rem;
      background-color: var(--bg-color);
      border-radius: 0.25rem;
      border: 1px solid var(--border-color);
    }
    
    .pin-value {
      font-weight: 600;
      color: var(--primary-color);
    }
    
    select, input[type="text"], input[type="password"] {
      width: 100%;
      padding: 0.75rem;
      margin: 0.5rem 0 1rem;
      border: 1px solid var(--border-color);
      border-radius: 0.25rem;
      font-size: 0.9rem;
    }
    
    button {
      background-color: var(--primary-color);
      color: white;
      border: none;
      padding: 0.75rem 1.25rem;
      border-radius: 0.25rem;
      cursor: pointer;
      font-size: 0.9rem;
      transition: all 0.3s ease;
      font-weight: 500;
    }
    
    button:hover {
      filter: brightness(110%);
      transform: translateY(-1px);
    }
    
    button.danger {
      background-color: var(--danger-color);
    }
    
    @media (max-width: 768px) {
      body {
        padding: 0.75rem;
      }
      
      .tab {
        flex-direction: column;
      }
      
      .tab button {
        width: 100%;
      }
      
      .control-row {
        flex-direction: column;
        align-items: stretch;
      }
      
      .control-label {
        margin-bottom: 0.5rem;
      }
      
      .control-input {
        flex-wrap: wrap;
      }
      
      .number-input {
        width: 100%;
        margin-bottom: 0.5rem;
      }
      
      .unit {
        width: auto;
        margin-left: auto;
      }
    }
  
  </style>
</head>
<body>
  <div id="notification" class="notification">Сохранено</div>
  
  <h1>LED Controller</h1>
  
  <div class="tab">
    <button class="tablinks active" onclick="openTab(event, 'control')">Control</button>
    <button class="tablinks" onclick="openTab(event, 'settings')">Settings</button>
    <button class="tablinks" onclick="openTab(event, 'triggers')">Triggers</button>
    <button class="tablinks" onclick="openTab(event, 'ledadjust')">LED Adjust</button>
    <button class="tablinks" onclick="openTab(event, 'arrowsettings')">Arrow Settings</button>
    <button class="tablinks" onclick="openTab(event, 'admin')">Admin</button>
  </div>
  
  <div id="control" class="tabcontent" style="display: block;">
    <div class="control-group">
      <h3>Brightness Control</h3>
      <div class="control-row">
        <div class="control-label">Brightness:</div>
        <div class="control-input">
          <input type="number" id="brightnessInput" min="0" max="100" value="100" class="number-input">
          <span class="unit">%</span>
          <input type="range" id="brightness" min="0" max="100" value="100" class="slider">
        </div>
      </div>
    </div>
    
    <div class="control-group">
      <h3>Color Control</h3>
      <div class="color-preview" id="colorPreview"></div>
      
      <div class="control-row">
        <div class="control-label">Red:</div>
        <div class="control-input">
          <input type="number" id="redInput" min="0" max="255" value="255" class="number-input">
          <span class="unit">0-255</span>
          <input type="range" id="red" min="0" max="255" value="255" class="slider">
        </div>
      </div>
      
      <div class="control-row">
        <div class="control-label">Green:</div>
        <div class="control-input">
          <input type="number" id="greenInput" min="0" max="255" value="255" class="number-input">
          <span class="unit">0-255</span>
          <input type="range" id="green" min="0" max="255" value="255" class="slider">
        </div>
      </div>
      
      <div class="control-row">
        <div class="control-label">Blue:</div>
        <div class="control-input">
          <input type="number" id="blueInput" min="0" max="255" value="255" class="number-input">
          <span class="unit">0-255</span>
          <input type="range" id="blue" min="0" max="255" value="255" class="slider">
        </div>
      </div>
    </div>
    
    <button onclick="applySettings()">Apply Settings</button>
  </div>
  
  <div id="settings" class="tabcontent">
    <div class="control-group">
      <h3>Timing Settings</h3>
      
      <div class="control-row">
        <div class="control-label">Fade In Time:</div>
        <div class="control-input">
          <input type="number" id="fadeInTimeInput" min="100" max="10000" step="100" value="3500" class="number-input">
          <span class="unit">ms</span>
          <input type="range" id="fadeInTime" min="100" max="10000" step="100" value="3500" class="slider">
        </div>
      </div>
      
      <div class="control-row">
        <div class="control-label">Fade Out Time:</div>
        <div class="control-input">
          <input type="number" id="fadeOutTimeInput" min="100" max="10000" step="100" value="3000" class="number-input">
          <span class="unit">ms</span>
          <input type="range" id="fadeOutTime" min="100" max="10000" step="100" value="3000" class="slider">
        </div>
      </div>
      
      <div class="control-row">
        <div class="control-label">Dim Time:</div>
        <div class="control-input">
          <input type="number" id="dimTimeInput" min="100" max="10000" step="100" value="2000" class="number-input">
          <span class="unit">ms</span>
          <input type="range" id="dimTime" min="100" max="10000" step="100" value="2000" class="slider">
        </div>
      </div>
      
      <div class="control-row">
        <div class="control-label">Normal Change Time:</div>
        <div class="control-input">
          <input type="number" id="normalTimeInput" min="100" max="10000" step="100" value="500" class="number-input">
          <span class="unit">ms</span>
          <input type="range" id="normalTime" min="100" max="10000" step="100" value="500" class="slider">
        </div>
      </div>
    </div>
    
    <button onclick="saveSettings()">Save Timing Settings</button>
  </div>
  
  <div id="triggers" class="tabcontent">
    <div class="control-group">
      <h3>Dim Trigger</h3>
      
      <div class="control-row">
        <div class="control-label">Trigger Mode:</div>
        <div class="control-input">
          <select id="dimMode" style="width: 100%;">
            <option value="0">Disabled</option>
            <option value="1">Digital (Pin D5 io-14)</option>
            <option value="2">Analog (Pin A0 ADC)</option>
          </select>
        </div>
      </div>
      
      <div id="digitalDimSettings">
        <div class="control-row">
          <div class="control-label">Trigger Level:</div>
          <div class="control-input">
            <select id="dimPinLevel" style="width: 100%;">
              <option value="0">LOW (0V)</option>
              <option value="1">HIGH (3.3V)</option>
            </select>
          </div>
        </div>
        
        <div class="pin-status">
          <span>Current Pin State:</span>
          <span class="pin-value" id="dimPinValue">0</span>
        </div>
      </div>
      
      <div id="analogDimSettings">
        <div class="control-row">
          <div class="control-label">Trigger Level:</div>
          <div class="control-input">
            <select id="analogDimLevel" style="width: 100%;">
              <option value="0">LOW (below threshold)</option>
              <option value="1">HIGH (above threshold)</option>
            </select>
          </div>
        </div>
        
        <div class="control-row">
          <div class="control-label">Threshold:</div>
          <div class="control-input">
            <input type="number" id="dimThresholdInput" min="0" max="1023" value="512" class="number-input">
            <span class="unit">0-1023</span>
            <input type="range" id="dimThreshold" min="0" max="1023" value="512" class="slider">
          </div>
        </div>
        
        <div class="control-row">
          <div class="control-label">Hysteresis:</div>
          <div class="control-input">
            <input type="number" id="dimHysteresisInput" min="1" max="200" value="50" class="number-input">
            <span class="unit">1-200</span>
            <input type="range" id="dimHysteresis" min="1" max="200" value="50" class="slider">
          </div>
        </div>
        
        <div class="pin-status">
          <span>Current Sensor Value:</span>
          <span class="pin-value" id="analogValue">0</span>
        </div>
      </div>
      
      <div class="control-row">
        <div class="control-label">Dim Factor:</div>
        <div class="control-input">
          <input type="number" id="dimFactorInput" min="1" max="99" value="40" class="number-input">
          <span class="unit">%</span>
          <input type="range" id="dimFactor" min="1" max="99" value="40" class="slider">
        </div>
      </div>
    </div>
    
    <div class="control-group">
      <h3>Off Trigger (Pin D6 io-12)</h3>
      
      <div class="control-row">
        <div class="control-label">Trigger Level:</div>
        <div class="control-input">
          <select id="offPinLevel" style="width: 100%;">
            <option value="0">LOW (0V)</option>
            <option value="1">HIGH (3.3V)</option>
          </select>
        </div>
      </div>
      
      <div class="control-row">
        <div class="control-label">Threshold:</div>
        <div class="control-input">
          <input type="number" id="offThresholdInput" min="0" max="1023" value="512" class="number-input">
          <span class="unit">0-1023</span>
          <input type="range" id="offThreshold" min="0" max="1023" value="512" class="slider">
        </div>
      </div>
      
      <div class="pin-status">
        <span>Current Pin State:</span>
        <span class="pin-value" id="offPinValue">0</span>
      </div>
    </div>
    
    <button onclick="saveTriggers()">Save Trigger Settings</button>
  </div>
  
  <div id="ledadjust" class="tabcontent">
    <h2>LED Brightness Adjustment</h2>
    <p>Adjust brightness for individual LEDs (100% = full brightness)</p>
    
    <div class="control-group">
      <select id="ledSelect" class="led-select" onchange="loadLEDAdjustment()">
        <option value="-1">Select LED</option>
      </select>
      
      <div class="control-row">
        <div class="control-label">Brightness:</div>
        <div class="control-input">
          <input type="number" id="ledAdjustInput" min="1" max="100" value="100" class="number-input">
          <span class="unit">%</span>
          <input type="range" id="ledAdjust" min="1" max="100" value="100" class="slider">
        </div>
      </div>
      
      <button onclick="saveLEDAdjustment()">Save LED Adjustment</button>
    </div>
  </div>
  
  <div id="arrowsettings" class="tabcontent">
    <h2>Arrow Light Settings</h2>
    
    <div class="control-group">
      <h3>General Settings</h3>
      
      <div class="control-row">
        <div class="control-label">Arrow Brightness:</div>
        <div class="control-input">
          <input type="number" id="arrowBrightnessInput" min="0" max="100" value="100" class="number-input">
          <span class="unit">%</span>
          <input type="range" id="arrowBrightness" min="0" max="100" value="100" class="slider">
        </div>
      </div>
      
      <div class="control-row">
        <div class="control-label">Arrow Dim Factor:</div>
        <div class="control-input">
          <input type="number" id="arrowDimFactorInput" min="1" max="99" value="40" class="number-input">
          <span class="unit">%</span>
          <input type="range" id="arrowDimFactor" min="1" max="99" value="40" class="slider">
        </div>
      </div>
    </div>
    
    <div class="control-group">
      <h3>Individual Arrow Adjustment</h3>
      <p>Adjust brightness for each arrow (100% = full brightness)</p>
      
      <select id="arrowSelect" class="arrow-select" onchange="loadArrowAdjustment()">
        <option value="-1">Select Arrow</option>
        <option value="0">Arrow 1 (D1)</option>
        <option value="1">Arrow 2 (D2)</option>
        <option value="2">Arrow 3 (D3)</option>
        <option value="3">Arrow 4 (D4)</option>
      </select>
      
      <div class="control-row">
        <div class="control-label">Brightness:</div>
        <div class="control-input">
          <input type="number" id="arrowAdjustInput" min="1" max="100" value="100" class="number-input">
          <span class="unit">%</span>
          <input type="range" id="arrowAdjust" min="1" max="100" value="100" class="slider">
        </div>
      </div>
      
      <button onclick="saveArrowAdjustment()">Save Arrow Adjustment</button>
    </div>
    
    <button onclick="saveArrowSettings()">Save All Arrow Settings</button>
  </div>
  
  <div id="admin" class="tabcontent">
    <h2>Administration</h2>
    
    <div class="control-group">
      <h3>WiFi Settings</h3>
      <div>
        <label for="ap_ssid" class="control-label">Access Point SSID:</label>
        <input type="text" id="ap_ssid" value="LED_Controller">
      </div>
      
      <div>
        <label for="ap_password" class="control-label">Access Point Password:</label>
        <input type="password" id="ap_password" value="12345678">
      </div>
      
      <button onclick="saveWiFiSettings()">Save WiFi Settings</button>
    </div>
    
    <div class="control-group">
      <h3>System Reset</h3>
      <p>This will reset all settings to factory defaults</p>
      <button onclick="factoryReset()" class="danger">Factory Reset</button>
    </div>
  </div>
  
  <script>
    function showNotification(message = 'Сохранено') {
      const notif = document.getElementById('notification');
      notif.textContent = message;
      notif.style.display = 'block';
      setTimeout(() => { notif.style.display = 'none'; }, 1000);
    }
    
    function openTab(evt, tabName) {
      var tabcontent = document.getElementsByClassName("tabcontent");
      for (var i = 0; i < tabcontent.length; i++) {
        tabcontent[i].style.display = "none";
      }
      
      var tablinks = document.getElementsByClassName("tablinks");
      for (var i = 0; i < tablinks.length; i++) {
        tablinks[i].className = tablinks[i].className.replace(" active", "");
      }
      
      document.getElementById(tabName).style.display = "block";
      evt.currentTarget.className += " active";
    }
    
    function updateColorPreview() {
  const red = document.getElementById('redInput').value;
  const green = document.getElementById('greenInput').value;
  const blue = document.getElementById('blueInput').value;
  document.getElementById('colorPreview').style.backgroundColor = `rgb(${red}, ${green}, ${blue})`;
}
    
    function updatePinStatus() {
      fetch('/pinstatus')
        .then(response => response.json())
        .then(data => {
          document.getElementById('dimPinValue').textContent = data.dimState;
          document.getElementById('offPinValue').textContent = data.offState;
          document.getElementById('analogValue').textContent = data.analogValue;
        });
    }
    
    function toggleDimSettings() {
      const mode = document.getElementById('dimMode').value;
      document.getElementById('digitalDimSettings').style.display = mode == '1' ? 'block' : 'none';
      document.getElementById('analogDimSettings').style.display = mode == '2' ? 'block' : 'none';
    }
    
    // Initialize LED select dropdown
    function initLEDSelect() {
      const select = document.getElementById('ledSelect');
      for(let i = 0; i < 60; i++) {
        const option = document.createElement('option');
        option.value = i;
        option.textContent = `LED ${i + 1}`;
        select.appendChild(option);
      }
    }
    
    // Load LED adjustment value
    function loadLEDAdjustment() {
      const ledIndex = document.getElementById('ledSelect').value;
      if(ledIndex >= 0) {
        fetch('/ledadjust?led=' + ledIndex)
          .then(response => response.json())
          .then(data => {
            document.getElementById('ledAdjust').value = data.adjustment;
            document.getElementById('ledAdjustInput').value = data.adjustment;
          });
      }
    }
    
    // Save LED adjustment
    function saveLEDAdjustment() {
      const ledIndex = document.getElementById('ledSelect').value;
      if(ledIndex >= 0) {
        const adjustment = document.getElementById('ledAdjustInput').value;
        
        fetch('/ledadjust', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({
            led: parseInt(ledIndex),
            adjustment: parseInt(adjustment)
          })
        }).then(() => {
          showNotification();
          // Force update LEDs after adjustment
          fetch('/control', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({
              brightness: document.getElementById('brightnessInput').value,
              color: {
                r: document.getElementById('redInput').value,
                g: document.getElementById('greenInput').value,
                b: document.getElementById('blueInput').value
              }
            })
          });
        });
      }
    }
    
    // Load arrow settings
    function loadArrowSettings() {
      fetch('/arrowsettings')
        .then(response => response.json())
        .then(data => {
          document.getElementById('arrowBrightness').value = data.brightness;
          document.getElementById('arrowBrightnessInput').value = data.brightness;
          document.getElementById('arrowDimFactor').value = data.dimFactor;
          document.getElementById('arrowDimFactorInput').value = data.dimFactor;
        });
    }
    
    // Load arrow adjustment value
    function loadArrowAdjustment() {
      const arrowIndex = document.getElementById('arrowSelect').value;
      if(arrowIndex >= 0) {
        fetch('/arrowsettings?arrow=' + arrowIndex)
          .then(response => response.json())
          .then(data => {
            document.getElementById('arrowAdjust').value = data.adjustment;
            document.getElementById('arrowAdjustInput').value = data.adjustment;
          });
      }
    }
    
    // Save arrow adjustment
    function saveArrowAdjustment() {
      const arrowIndex = document.getElementById('arrowSelect').value;
      if(arrowIndex >= 0) {
        const adjustment = document.getElementById('arrowAdjustInput').value;
        
        fetch('/arrowsettings', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({
            arrow: parseInt(arrowIndex),
            adjustment: parseInt(adjustment)
          })
        }).then(() => {
          showNotification();
        });
      }
    }
    
    // Save all arrow settings
    function saveArrowSettings() {
      const brightness = document.getElementById('arrowBrightnessInput').value;
      const dimFactor = document.getElementById('arrowDimFactorInput').value;
      
      fetch('/arrowsettings', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          brightness: parseInt(brightness),
          dimFactor: parseInt(dimFactor)
        })
      }).then(() => {
        showNotification();
      });
    }
    
// Setup input-slider synchronization
function setupInputSliderSync(inputId, sliderId, callback) {
  const input = document.getElementById(inputId);
  const slider = document.getElementById(sliderId);
  
  input.addEventListener('input', function() {
    slider.value = this.value;
    if (callback) callback();
  });
  
  slider.addEventListener('input', function() {
    input.value = this.value;
    if (callback) callback();
  });
}
    
    // Инициализация интерфейса
    document.addEventListener('DOMContentLoaded', function() {
  // Setup color controls with updateColorPreview callback
  setupInputSliderSync('redInput', 'red', updateColorPreview);
  setupInputSliderSync('greenInput', 'green', updateColorPreview);
  setupInputSliderSync('blueInput', 'blue', updateColorPreview);
  
      // Setup all input-slider pairs
      setupInputSliderSync('brightnessInput', 'brightness');
      setupInputSliderSync('redInput', 'red');
      setupInputSliderSync('greenInput', 'green');
      setupInputSliderSync('blueInput', 'blue');
      setupInputSliderSync('fadeInTimeInput', 'fadeInTime');
      setupInputSliderSync('fadeOutTimeInput', 'fadeOutTime');
      setupInputSliderSync('dimTimeInput', 'dimTime');
      setupInputSliderSync('normalTimeInput', 'normalTime');
      setupInputSliderSync('dimThresholdInput', 'dimThreshold');
      setupInputSliderSync('dimHysteresisInput', 'dimHysteresis');
      setupInputSliderSync('dimFactorInput', 'dimFactor');
      setupInputSliderSync('offThresholdInput', 'offThreshold');
      setupInputSliderSync('ledAdjustInput', 'ledAdjust');
      setupInputSliderSync('arrowBrightnessInput', 'arrowBrightness');
      setupInputSliderSync('arrowDimFactorInput', 'arrowDimFactor');
      setupInputSliderSync('arrowAdjustInput', 'arrowAdjust');
      
      fetch('/settings')
        .then(response => response.json())
        .then(data => {
          // Основные настройки
          document.getElementById('brightness').value = data.brightness;
          document.getElementById('brightnessInput').value = data.brightness;
          
          // Цвет
          document.getElementById('red').value = data.color.r;
          document.getElementById('redInput').value = data.color.r;
          document.getElementById('green').value = data.color.g;
          document.getElementById('greenInput').value = data.color.g;
          document.getElementById('blue').value = data.color.b;
          document.getElementById('blueInput').value = data.color.b;
          updateColorPreview();
          
          // Временные параметры
          document.getElementById('fadeInTime').value = data.timing.fadeInTime;
          document.getElementById('fadeInTimeInput').value = data.timing.fadeInTime;
          document.getElementById('fadeOutTime').value = data.timing.fadeOutTime;
          document.getElementById('fadeOutTimeInput').value = data.timing.fadeOutTime;
          document.getElementById('dimTime').value = data.timing.dimTime;
          document.getElementById('dimTimeInput').value = data.timing.dimTime;
          document.getElementById('normalTime').value = data.timing.normalTime;
          document.getElementById('normalTimeInput').value = data.timing.normalTime;
          
          // Триггеры
          document.getElementById('dimMode').value = data.triggers.dimMode;
          document.getElementById('dimPinLevel').value = data.triggers.dimPinLevel;
          document.getElementById('dimThreshold').value = data.triggers.dimThreshold;
          document.getElementById('dimThresholdInput').value = data.triggers.dimThreshold;
          document.getElementById('dimHysteresis').value = data.triggers.dimHysteresis;
          document.getElementById('dimHysteresisInput').value = data.triggers.dimHysteresis;
          document.getElementById('dimFactor').value = Math.round(data.triggers.dimFactor * 100);
          document.getElementById('dimFactorInput').value = Math.round(data.triggers.dimFactor * 100);
          
          document.getElementById('offPinLevel').value = data.triggers.offPinLevel;
          document.getElementById('offThreshold').value = data.triggers.offThreshold;
          document.getElementById('offThresholdInput').value = data.triggers.offThreshold;
          
          // WiFi
          document.getElementById('ap_ssid').value = data.ssid;
          document.getElementById('ap_password').value = data.password;
          
          // Показываем/скрываем соответствующие настройки
          toggleDimSettings();
          
          // Загружаем настройки стрелок
          loadArrowSettings();
          
          // Initialize LED select
          initLEDSelect();
          
          // Обновление статуса пинов каждую секунду
          setInterval(updatePinStatus, 1000);
        });
    });
    
    function applySettings() {
      const brightness = document.getElementById('brightnessInput').value;
      const red = document.getElementById('redInput').value;
      const green = document.getElementById('greenInput').value;
      const blue = document.getElementById('blueInput').value;
      
      fetch('/control', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          brightness: parseInt(brightness),
          color: {
            r: parseInt(red),
            g: parseInt(green),
            b: parseInt(blue)
          }
        })
      }).then(() => showNotification());
    }
    
    function saveSettings() {
      const fadeInTime = document.getElementById('fadeInTimeInput').value;
      const fadeOutTime = document.getElementById('fadeOutTimeInput').value;
      const dimTime = document.getElementById('dimTimeInput').value;
      const normalTime = document.getElementById('normalTimeInput').value;
      
      fetch('/settings', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          timing: {
            fadeInTime: parseInt(fadeInTime),
            fadeOutTime: parseInt(fadeOutTime),
            dimTime: parseInt(dimTime),
            normalTime: parseInt(normalTime)
          }
        })
      }).then(() => showNotification());
    }
    
    function saveTriggers() {
      const dimMode = document.getElementById('dimMode').value;
      const dimPinLevel = document.getElementById('dimPinLevel').value;
      const analogDimLevel = document.getElementById('analogDimLevel').value;
      const dimThreshold = document.getElementById('dimThresholdInput').value;
      const dimHysteresis = document.getElementById('dimHysteresisInput').value;
      const dimFactor = document.getElementById('dimFactorInput').value / 100;
      const offPinLevel = document.getElementById('offPinLevel').value;
      const offThreshold = document.getElementById('offThresholdInput').value;
      
      fetch('/settings', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          triggers: {
            dimMode: parseInt(dimMode),
            dimPinLevel: parseInt(dimPinLevel),
            analogDimLevel: parseInt(analogDimLevel),
            dimThreshold: parseInt(dimThreshold),
            dimHysteresis: parseInt(dimHysteresis),
            dimFactor: parseFloat(dimFactor),
            offPinLevel: parseInt(offPinLevel),
            offThreshold: parseInt(offThreshold)
          }
        })
      }).then(() => showNotification());
    }
    
    function saveWiFiSettings() {
      const ssid = document.getElementById('ap_ssid').value;
      const password = document.getElementById('ap_password').value;
      
      fetch('/admin/wifi', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
          ssid: ssid,
          password: password
        })
      }).then(() => showNotification("WiFi settings saved. Restart to apply."));
    }
    
    function factoryReset() {
      if(confirm('Are you sure you want to reset ALL settings to factory defaults?')) {
        fetch('/admin/reset', { method: 'POST' })
          .then(() => {
            showNotification("Factory reset complete. Restarting...");
            setTimeout(() => { location.reload(); }, 2000);
          });
      }
    }
  </script>
</body>
</html>
)=====";
  
void handleRoot() {
  server.send_P(200, "text/html", MAIN_page);
}

void handleGetSettings() {
  DynamicJsonDocument doc(1024);
  
  doc["brightness"] = settings.brightness;
  doc["color"]["r"] = settings.color.r;
  doc["color"]["g"] = settings.color.g;
  doc["color"]["b"] = settings.color.b;
  
  doc["timing"]["fadeInTime"] = settings.timing.fadeInTime;
  doc["timing"]["fadeOutTime"] = settings.timing.fadeOutTime;
  doc["timing"]["dimTime"] = settings.timing.dimTime;
  doc["timing"]["normalTime"] = settings.timing.normalTime;
  
  doc["triggers"]["dimMode"] = settings.triggers.dimMode;
  doc["triggers"]["dimPinLevel"] = settings.triggers.dimPinLevel;
  doc["triggers"]["dimThreshold"] = settings.triggers.dimThreshold;
  doc["triggers"]["dimHysteresis"] = settings.triggers.dimHysteresis;
  doc["triggers"]["dimFactor"] = settings.triggers.dimFactor;
  doc["triggers"]["offPinLevel"] = settings.triggers.offPinLevel;
  doc["triggers"]["offThreshold"] = settings.triggers.offThreshold;
  
  doc["ssid"] = settings.ssid;
  doc["password"] = settings.password;
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handlePostSettings() {
  if(server.hasArg("plain")) {
    DynamicJsonDocument doc(512);
    deserializeJson(doc, server.arg("plain"));
    
    if(doc.containsKey("brightness")) {
      settings.brightness = doc["brightness"];
    }
    
    if(doc.containsKey("color")) {
      settings.color.r = doc["color"]["r"];
      settings.color.g = doc["color"]["g"];
      settings.color.b = doc["color"]["b"];
    }
    
    if(doc.containsKey("timing")) {
      if(doc["timing"].containsKey("fadeInTime")) settings.timing.fadeInTime = doc["timing"]["fadeInTime"];
      if(doc["timing"].containsKey("fadeOutTime")) settings.timing.fadeOutTime = doc["timing"]["fadeOutTime"];
      if(doc["timing"].containsKey("dimTime")) settings.timing.dimTime = doc["timing"]["dimTime"];
      if(doc["timing"].containsKey("normalTime")) settings.timing.normalTime = doc["timing"]["normalTime"];
    }
    
    if(doc.containsKey("triggers")) {
      if(doc["triggers"].containsKey("dimMode")) settings.triggers.dimMode = doc["triggers"]["dimMode"];
      if(doc["triggers"].containsKey("dimPinLevel")) settings.triggers.dimPinLevel = doc["triggers"]["dimPinLevel"];
      if(doc["triggers"].containsKey("analogDimLevel")) settings.triggers.analogDimLevel = doc["triggers"]["analogDimLevel"];
      if(doc["triggers"].containsKey("dimThreshold")) settings.triggers.dimThreshold = doc["triggers"]["dimThreshold"];
      if(doc["triggers"].containsKey("dimHysteresis")) settings.triggers.dimHysteresis = doc["triggers"]["dimHysteresis"];
      if(doc["triggers"].containsKey("dimFactor")) settings.triggers.dimFactor = doc["triggers"]["dimFactor"];
      if(doc["triggers"].containsKey("offPinLevel")) settings.triggers.offPinLevel = doc["triggers"]["offPinLevel"];
      if(doc["triggers"].containsKey("offThreshold")) settings.triggers.offThreshold = doc["triggers"]["offThreshold"];
    }
    
    saveSettings();
    
    checkTriggerPins();
    
    if(powerOn && !dimmingActive) {
      startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.normalTime);
    }
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\"}");
  }
}

void handleControl() {
  if(server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    deserializeJson(doc, server.arg("plain"));
    
    if(doc.containsKey("brightness")) {
      settings.brightness = doc["brightness"];
    }
    
    if(doc.containsKey("color")) {
      settings.color.r = doc["color"]["r"];
      settings.color.g = doc["color"]["g"];
      settings.color.b = doc["color"]["b"];
    }
    
    saveSettings();
    
    if(powerOn) {
      startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.normalTime);
    }
    // Принудительное обновление светодиодов независимо от состояния powerOn
    updateLEDs(true);
    
    server.send(200, "application/json", "{\"status\":\"ok\"}");
  } else {
    server.send(400, "application/json", "{\"status\":\"error\"}");
  }
}

void handleReset() {
  resetSettings();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handlePinStatus() {
  DynamicJsonDocument doc(128);
  doc["dimState"] = digitalRead(DIM_PIN);
  doc["offState"] = digitalRead(OFF_PIN);
  doc["analogValue"] = analogRead(ANALOG_PIN);
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleAdmin() {
  server.sendHeader("Location", "/");
  server.send(302);
}

void handleAdminReset() {
  resetSettings();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleGetLEDAdjust() {
  int ledIndex = server.arg("led").toInt();
  if(ledIndex >= 0 && ledIndex < NUM_LEDS) {
    DynamicJsonDocument doc(128);
    doc["led"] = ledIndex;
    doc["adjustment"] = settings.ledAdjustments[ledIndex];
    
    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
  } else {
    server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid LED index\"}");
  }
}

void handlePostLEDAdjust() {
  if(server.hasArg("plain")) {
    DynamicJsonDocument doc(128);
    deserializeJson(doc, server.arg("plain"));
    
    if(doc.containsKey("led") && doc.containsKey("adjustment")) {
      int ledIndex = doc["led"];
      if(ledIndex >= 0 && ledIndex < NUM_LEDS) {
        settings.ledAdjustments[ledIndex] = doc["adjustment"];
        saveSettings();
        
        // Принудительное обновление всех светодиодов
        updateLEDs(true);
        
        // Проверяем состояние пинов
        checkTriggerPins();
        
        server.send(200, "application/json", "{\"status\":\"ok\"}");
        return;
      }
    }
  }
  server.send(400, "application/json", "{\"status\":\"error\"}");
}

void handleGetArrowSettings() {
  if(server.hasArg("arrow")) {
    int arrowIndex = server.arg("arrow").toInt();
    if(arrowIndex >= 0 && arrowIndex < ARROW_PINS) {
      DynamicJsonDocument doc(128);
      doc["arrow"] = arrowIndex;
      doc["adjustment"] = settings.arrows.individualBrightness[arrowIndex];
      
      String json;
      serializeJson(doc, json);
      server.send(200, "application/json", json);
      return;
    }
  }
  
  // Если нет конкретного запроса стрелки, возвращаем все настройки стрелок
  DynamicJsonDocument doc(256);
  doc["brightness"] = settings.arrows.brightness;
  doc["dimFactor"] = settings.arrows.dimFactor;
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handlePostArrowSettings() {
  if(server.hasArg("plain")) {
    DynamicJsonDocument doc(256);
    deserializeJson(doc, server.arg("plain"));
    
    if(doc.containsKey("brightness") && doc.containsKey("dimFactor")) {
      // Общие настройки стрелок
      settings.arrows.brightness = doc["brightness"];
      settings.arrows.dimFactor = doc["dimFactor"];
      saveSettings();
      
      // Обновляем яркость стрелок, если они включены
      if(powerOn) {
        for(int i = 0; i < ARROW_PINS; i++) {
          startArrowFade(i, dimmingActive ? 
                        map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100 * settings.arrows.dimFactor / 100, 
                            0, 100, 0, 255) :
                        map(settings.arrows.brightness * settings.arrows.individualBrightness[i] / 100, 
                            0, 100, 0, 255), 
                        settings.timing.normalTime);
        }
      }
      
      server.send(200, "application/json", "{\"status\":\"ok\"}");
      return;
    } else if(doc.containsKey("arrow") && doc.containsKey("adjustment")) {
      // Индивидуальная настройка стрелки
      int arrowIndex = doc["arrow"];
      if(arrowIndex >= 0 && arrowIndex < ARROW_PINS) {
        settings.arrows.individualBrightness[arrowIndex] = doc["adjustment"];
        saveSettings();
        
        // Обновляем яркость конкретной стрелки
        if(powerOn) {
          startArrowFade(arrowIndex, dimmingActive ? 
                        map(settings.arrows.brightness * settings.arrows.individualBrightness[arrowIndex] / 100 * settings.arrows.dimFactor / 100, 
                            0, 100, 0, 255) :
                        map(settings.arrows.brightness * settings.arrows.individualBrightness[arrowIndex] / 100, 
                            0, 100, 0, 255), 
                        settings.timing.normalTime);
        }
        
        server.send(200, "application/json", "{\"status\":\"ok\"}");
        return;
      }
    }
  }
  server.send(400, "application/json", "{\"status\":\"error\"}");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}
