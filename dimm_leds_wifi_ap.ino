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

#pragma pack(push, 1)
struct Settings {
  // WiFi settings
  char ssid[32] = "LED_Controller";
  char password[32] = "12345678";
  
  // Light settings
  uint8_t brightness = 100;
  CRGB color = CRGB::White;
  
  // LED brightness adjustments (0-100%, default 100%)
  uint8_t ledAdjustments[NUM_LEDS] = {100};
  
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
  if(needsSave) saveSettings();
  
  // Fade in on start
  startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.fadeInTime);
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
    } 
    else if(currentDimState != (settings.triggers.dimPinLevel == LOW) && dimmingActive) {
      dimmingActive = false;
      startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.normalTime);
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
      } else {
        powerOn = true;
        startFade(dimmingActive ? 
                 map(settings.brightness, 0, 100, 0, 255) * settings.triggers.dimFactor : 
                 map(settings.brightness, 0, 100, 0, 255), 
                 settings.timing.fadeInTime);
      }
    }
    lastOffState = currentOffState;
    delay(50);
  }
  
  updateFade();

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
            }
        } else {
            if(currentValue >= settings.triggers.dimThreshold + settings.triggers.dimHysteresis) {
                analogDimState = false;
                dimmingActive = false;
                startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.normalTime);
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
            }
        } else {
            if(currentValue <= settings.triggers.dimThreshold - settings.triggers.dimHysteresis) {
                analogDimState = false;
                dimmingActive = false;
                startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.normalTime);
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
    } else {
      dimmingActive = false;
      startFade(map(settings.brightness, 0, 100, 0, 255), settings.timing.normalTime);
    }
  }
  
  // Проверка состояния пина выключения
  if(currentOffState == (settings.triggers.offPinLevel == LOW)) {
    powerOn = false;
    startFade(0, settings.timing.fadeOutTime);
  } else {
    powerOn = true;
    startFade(dimmingActive ? 
             map(settings.brightness, 0, 100, 0, 255) * settings.triggers.dimFactor : 
             map(settings.brightness, 0, 100, 0, 255), 
             settings.timing.fadeInTime);
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
  strcpy(settings.ssid, "LED_Controller");
  strcpy(settings.password, "12345678");
  settings.brightness = 100;
  settings.color = CRGB::White;
  
  // Initialize all LED adjustments to 100%
  for(int i = 0; i < NUM_LEDS; i++) {
    settings.ledAdjustments[i] = 100;
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
      body {font-family: Arial; margin: 0 auto; max-width: 800px; padding: 20px;}
      .tab {overflow: hidden; border: 1px solid #ccc; background-color: #f1f1f1;}
      .tab button {background-color: inherit; float: left; border: none; outline: none; cursor: pointer; padding: 14px 16px; transition: 0.3s;}
      .tab button:hover {background-color: #ddd;}
      .tab button.active {background-color: #ccc;}
      .tabcontent {display: none; padding: 20px; border: 1px solid #ccc; border-top: none;}
      .slider-container {margin: 15px 0;}
      .color-container {display: flex; flex-direction: column; gap: 10px; margin: 15px 0;}
      .color-preview {width: 100%; height: 60px; border: 1px solid #ccc; border-radius: 5px;}
      .notification {
        position: fixed;
        top: 20px;
        left: 50%;
        transform: translateX(-50%);
        background: rgba(0,0,0,0.7);
        color: white;
        padding: 10px 20px;
        border-radius: 5px;
        display: none;
        z-index: 1000;
      }
      .pin-status {display: flex; justify-content: space-between; margin: 10px 0;}
      .pin-value {font-weight: bold;}
      .admin-panel {border-top: 1px solid #ccc; margin-top: 20px; padding-top: 20px;}
      .led-adjust-container {margin: 15px 0;}
      .led-select {width: 100%; padding: 8px; margin-bottom: 10px;}
      .led-adjust-slider {width: 100%;}
      .led-adjust-value {text-align: center; font-weight: bold;}
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
      <button class="tablinks" onclick="openTab(event, 'admin')">Admin</button>
    </div>
    
    <div id="control" class="tabcontent" style="display: block;">
      <div class="slider-container">
        <label for="brightness">Brightness: <span id="brightnessValue">100</span>%</label>
        <input type="range" id="brightness" min="0" max="100" value="100" class="slider">
      </div>
      
      <div class="color-container">
        <label>Color (Pin D0 io-16):</label>
        <div class="color-preview" id="colorPreview"></div>
        <div>
          <label for="red">Red:</label>
          <input type="range" id="red" min="0" max="255" value="255">
        </div>
        <div>
          <label for="green">Green:</label>
          <input type="range" id="green" min="0" max="255" value="255">
        </div>
        <div>
          <label for="blue">Blue:</label>
          <input type="range" id="blue" min="0" max="255" value="255">
        </div>
      </div>
      
      <button onclick="applySettings()">Apply</button>
    </div>
    
    <div id="settings" class="tabcontent">
      <div class="slider-container">
        <label for="fadeInTime">Fade In Time: <span id="fadeInTimeValue">3500</span>ms</label>
        <input type="range" id="fadeInTime" min="100" max="10000" step="100" value="3500" class="slider">
      </div>
      
      <div class="slider-container">
        <label for="fadeOutTime">Fade Out Time: <span id="fadeOutTimeValue">3000</span>ms</label>
        <input type="range" id="fadeOutTime" min="100" max="10000" step="100" value="3000" class="slider">
      </div>
      
      <div class="slider-container">
        <label for="dimTime">Dim Time: <span id="dimTimeValue">2000</span>ms</label>
        <input type="range" id="dimTime" min="100" max="10000" step="100" value="2000" class="slider">
      </div>
      
      <div class="slider-container">
        <label for="normalTime">Normal Change Time: <span id="normalTimeValue">500</span>ms</label>
        <input type="range" id="normalTime" min="100" max="10000" step="100" value="500" class="slider">
      </div>
      
      <button onclick="saveSettings()">Save Settings</button>
    </div>
    
    <div id="triggers" class="tabcontent">
      <h3>Dim Trigger</h3>
      <div>
        <label for="dimMode">Trigger Mode:</label>
        <select id="dimMode">
          <option value="0">Disabled</option>
          <option value="1">Digital (Pin D5 io-14)</option>
          <option value="2">Analog (Pin A0 ADC)</option>
        </select>
      </div>
      
      <div id="digitalDimSettings">
        <div>
          <label for="dimPinLevel">Trigger Level:</label>
          <select id="dimPinLevel">
            <option value="0">LOW (0V)</option>
            <option value="1">HIGH (3.3V)</option>
          </select>
        </div>
        
        <div class="pin-status">
          <span>Current Pin State:</span>
          <span class="pin-value" id="dimPinValue">0</span>
        </div>
      </div>
      
      <div id="analogDimSettings">
      <div>
        <label for="analogDimLevel">Trigger Level:</label>
        <select id="analogDimLevel">
            <option value="0">LOW (below threshold)</option>
            <option value="1">HIGH (above threshold)</option>
        </select>
    </div>
        <div class="slider-container">
          <label for="dimThreshold">Threshold: <span id="dimThresholdValue">512</span></label>
          <input type="range" id="dimThreshold" min="0" max="1023" value="512" class="slider">
        </div>
        
        <div class="slider-container">
          <label for="dimHysteresis">Hysteresis: <span id="dimHysteresisValue">50</span></label>
          <input type="range" id="dimHysteresis" min="1" max="200" value="50" class="slider">
        </div>
        
        <div class="pin-status">
          <span>Current Sensor Value:</span>
          <span class="pin-value" id="analogValue">0</span>
        </div>
      </div>
      
      <div class="slider-container">
        <label for="dimFactor">Dim Factor: <span id="dimFactorValue">40</span>%</label>
        <input type="range" id="dimFactor" min="1" max="99" value="40" class="slider">
      </div>
      
      <h3>Off Trigger (Pin D6 io-12)</h3>
      <div>
        <label for="offPinLevel">Trigger Level:</label>
        <select id="offPinLevel">
          <option value="0">LOW (0V)</option>
          <option value="1">HIGH (3.3V)</option>
        </select>
      </div>
      
      <div class="slider-container">
        <label for="offThreshold">Threshold: <span id="offThresholdValue">512</span></label>
        <input type="range" id="offThreshold" min="0" max="1023" value="512" class="slider">
      </div>
      
      <div class="pin-status">
        <span>Current Pin State:</span>
        <span class="pin-value" id="offPinValue">0</span>
      </div>
      
      <button onclick="saveTriggers()">Save Triggers</button>
    </div>
    
    <div id="ledadjust" class="tabcontent">
      <h2>LED Brightness Adjustment</h2>
      <p>Adjust brightness for individual LEDs (100% = full brightness)</p>
      
      <div class="led-adjust-container">
        <select id="ledSelect" class="led-select" onchange="loadLEDAdjustment()">
          <option value="-1">Select LED</option>
        </select>
        
        <div class="slider-container">
          <label>Brightness: <span id="ledAdjustValue">100</span>%</label>
          <input type="range" id="ledAdjust" min="1" max="100" value="100" class="led-adjust-slider">
        </div>
        
        <button onclick="saveLEDAdjustment()">Save Adjustment</button>
      </div>
    </div>
    
    <div id="admin" class="tabcontent">
      <h2>Administration</h2>
      
      <div class="admin-panel">
        <h3>WiFi Settings</h3>
        <div>
          <label for="ap_ssid">Access Point SSID:</label>
          <input type="text" id="ap_ssid" value="LED_Controller">
        </div>
        
        <div>
          <label for="ap_password">Access Point Password:</label>
          <input type="password" id="ap_password" value="12345678">
        </div>
        
        <button onclick="saveWiFiSettings()">Save WiFi Settings</button>
      </div>
      
      <div class="admin-panel">
        <h3>System Reset</h3>
        <p>This will reset all settings to factory defaults</p>
        <button onclick="factoryReset()" style="background-color: #f44336;">Factory Reset</button>
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
        const r = document.getElementById('red').value;
        const g = document.getElementById('green').value;
        const b = document.getElementById('blue').value;
        document.getElementById('colorPreview').style.backgroundColor = `rgb(${r}, ${g}, ${b})`;
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
              document.getElementById('ledAdjustValue').textContent = data.adjustment;
            });
        }
      }
      
      // Save LED adjustment
      function saveLEDAdjustment() {
        const ledIndex = document.getElementById('ledSelect').value;
        if(ledIndex >= 0) {
          const adjustment = document.getElementById('ledAdjust').value;
          
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
                brightness: document.getElementById('brightness').value,
                color: {
                  r: document.getElementById('red').value,
                  g: document.getElementById('green').value,
                  b: document.getElementById('blue').value
                }
              })
            });
          });
        }
      }
      
      // Инициализация интерфейса
      document.addEventListener('DOMContentLoaded', function() {
        fetch('/settings')
          .then(response => response.json())
          .then(data => {
            // Основные настройки
            document.getElementById('brightness').value = data.brightness;
            document.getElementById('brightnessValue').textContent = data.brightness;
            
            // Цвет
            document.getElementById('red').value = data.color.r;
            document.getElementById('green').value = data.color.g;
            document.getElementById('blue').value = data.color.b;
            updateColorPreview();
            
            // Временные параметры
            document.getElementById('fadeInTime').value = data.timing.fadeInTime;
            document.getElementById('fadeInTimeValue').textContent = data.timing.fadeInTime;
            document.getElementById('fadeOutTime').value = data.timing.fadeOutTime;
            document.getElementById('fadeOutTimeValue').textContent = data.timing.fadeOutTime;
            document.getElementById('dimTime').value = data.timing.dimTime;
            document.getElementById('dimTimeValue').textContent = data.timing.dimTime;
            document.getElementById('normalTime').value = data.timing.normalTime;
            document.getElementById('normalTimeValue').textContent = data.timing.normalTime;
            
            // Триггеры
            document.getElementById('dimMode').value = data.triggers.dimMode;
            document.getElementById('dimPinLevel').value = data.triggers.dimPinLevel;
            document.getElementById('dimThreshold').value = data.triggers.dimThreshold;
            document.getElementById('dimThresholdValue').textContent = data.triggers.dimThreshold;
            document.getElementById('dimHysteresis').value = data.triggers.dimHysteresis;
            document.getElementById('dimHysteresisValue').textContent = data.triggers.dimHysteresis;
            document.getElementById('dimFactor').value = Math.round(data.triggers.dimFactor * 100);
            document.getElementById('dimFactorValue').textContent = Math.round(data.triggers.dimFactor * 100);
            
            document.getElementById('offPinLevel').value = data.triggers.offPinLevel;
            document.getElementById('offThreshold').value = data.triggers.offThreshold;
            document.getElementById('offThresholdValue').textContent = data.triggers.offThreshold;
            
            // WiFi
            document.getElementById('ap_ssid').value = data.ssid;
            document.getElementById('ap_password').value = data.password;
            
            // Показываем/скрываем соответствующие настройки
            toggleDimSettings();
          });
          
        // Обработчики слайдеров
        document.getElementById('brightness').addEventListener('input', function() {
          document.getElementById('brightnessValue').textContent = this.value;
        });
        
        document.getElementById('red').addEventListener('input', updateColorPreview);
        document.getElementById('green').addEventListener('input', updateColorPreview);
        document.getElementById('blue').addEventListener('input', updateColorPreview);
        
        document.getElementById('fadeInTime').addEventListener('input', function() {
          document.getElementById('fadeInTimeValue').textContent = this.value;
        });
        
        document.getElementById('fadeOutTime').addEventListener('input', function() {
          document.getElementById('fadeOutTimeValue').textContent = this.value;
        });
        
        document.getElementById('dimTime').addEventListener('input', function() {
          document.getElementById('dimTimeValue').textContent = this.value;
        });
        
        document.getElementById('normalTime').addEventListener('input', function() {
          document.getElementById('normalTimeValue').textContent = this.value;
        });
        
        document.getElementById('dimThreshold').addEventListener('input', function() {
          document.getElementById('dimThresholdValue').textContent = this.value;
        });
        
        document.getElementById('dimHysteresis').addEventListener('input', function() {
          document.getElementById('dimHysteresisValue').textContent = this.value;
        });
        
        document.getElementById('dimFactor').addEventListener('input', function() {
          document.getElementById('dimFactorValue').textContent = this.value;
        });
        
        document.getElementById('offThreshold').addEventListener('input', function() {
          document.getElementById('offThresholdValue').textContent = this.value;
        });
        
        // Обработчик изменения режима триггера
        document.getElementById('dimMode').addEventListener('change', toggleDimSettings);
        
        // LED adjustment slider
        document.getElementById('ledAdjust').addEventListener('input', function() {
          document.getElementById('ledAdjustValue').textContent = this.value;
        });
        
        // Initialize LED select
        initLEDSelect();
        
        // Обновление статуса пинов каждую секунду
        setInterval(updatePinStatus, 1000);
      });
      
      function applySettings() {
        const brightness = document.getElementById('brightness').value;
        const red = document.getElementById('red').value;
        const green = document.getElementById('green').value;
        const blue = document.getElementById('blue').value;
        
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
        const fadeInTime = document.getElementById('fadeInTime').value;
        const fadeOutTime = document.getElementById('fadeOutTime').value;
        const dimTime = document.getElementById('dimTime').value;
        const normalTime = document.getElementById('normalTime').value;
        
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
        const dimThreshold = document.getElementById('dimThreshold').value;
        const dimHysteresis = document.getElementById('dimHysteresis').value;
        const dimFactor = document.getElementById('dimFactor').value / 100;
        const offPinLevel = document.getElementById('offPinLevel').value;
        const offThreshold = document.getElementById('offThreshold').value;
        
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

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}