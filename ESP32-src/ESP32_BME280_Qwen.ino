/*
 * ESP32 + BME280 — Климатическая станция с веб-дашбордом
 * 
 * Архитектура:
 *   - ESPAsyncWebServer (mathieucarbou fork) + AsyncTCP
 *   - ArduinoJson v7
 *   - Adafruit_BME280 (I2C)
 *   - Кольцевой буфер 2880 точек (48ч @ 1/мин)
 *   - WebSocket live-обновление (5с)
 *   - Captive portal в AP-режиме (DNSServer)
 *   - WiFi state machine с фоновым переподключением
 * 
 * Исправлены баги из предыдущих версий:
 *   - ArduinoJson v7 API (не v6)
 *   - Корректный порядок аргументов setSampling()
 *   - WIFI_AP_STA при фоновом retry (AP не умирает)
 *   - interval_sec учитывает step даунсемплинга
 *   - Chart.js через <script src>, не await import()
 *   - NaN-защита + авто-реинициализация BME280
 *   - ws.onEvent() + ws.cleanupClients()
 *   - WiFi.setSleep(false)
 *   - Никаких delay()
 *   By: Qwen3.8-Max-Preview
 *   Bug fix: Claude Sonnet 5 Medium (01.08.2026 20:24)
 *   От: 01.08.2026 20:24
 *   https://github.com/FireTIA/esp32-bme280-weather-station
 *   MIT License
 */

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <DNSServer.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include "index_html.h"

// ============================================================
// КОНСТАНТЫ (редактировать здесь)
// ============================================================

// --- Wi-Fi STA ---
#define WIFI_SSID         "<заполните> <имя Wifi сети к которой подключиться>"  
#define WIFI_PASS         "<заполните> <пароль Wifi сети к которой подключиться>"
#define WIFI_BSSID_STR    "<заполните> <MAC адрес Wifi сети к которой подключиться>"     // MAC роутера (или "00:00:00:00:00:00" для игнора) // 
#define WIFI_CHANNEL      0                       // 0 = авто || Если у вас фиксированный канал то установите тут значение по желанию, если плавающий то оставьте значение '0'.

// --- Wi-Fi AP (Fallback) ---
#define AP_SSID           "<заполните> <имя Wifi сети раздаваемого>"
#define AP_PASS           "<заполните> <пароль Wifi сети раздаваемого>"

// --- BME280 I2C ---
#define I2C_SDA_PIN       19      // УКАЖИТЕ SDA ПИН ГДЕ ОБИТАЕТ BME280
#define I2C_SCL_PIN       22      // УКАЖИТЕ SCL ПИН ГДЕ ОБИТАЕТ BME280
#define BME_ADDR_PRIMARY  0x76
#define BME_ADDR_SECONDARY 0x77

// --- Кольцевой буфер ---
#define HISTORY_SIZE      2880   // 48ч × 60мин
#define HISTORY_INTERVAL  60     // секунд между точками
#define MAX_JSON_POINTS   288    // максимум точек в /api/history (даунсемплинг)

// --- Часовой пояс ---
#define NTP_UTC_OFFSET_SEC  (3 * 3600)   // UTC+3 (Москва)

// --- Таймеры ---
#define STA_TIMEOUT_MS        60000   // 60с на подключение к STA
#define AP_RETRY_INTERVAL_MS  60000   // каждые 60с пробуем STA из AP
#define AP_RETRY_TIMEOUT_MS   15000   // 15с на попытку, потом назад в AP
#define SENSOR_60S_MS         60000   // интервал записи в историю
#define SENSOR_5S_MS          5000    // интервал WebSocket-обновления
#define BME_RETRY_MS          30000   // повторная инициализация BME при отвале

// ============================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ============================================================

// --- WiFi State Machine ---
enum WiFiState {
  WIFI_STA_CONNECTING,
  WIFI_STA_CONNECTED,
  WIFI_AP_ONLY,
  WIFI_AP_STA_RECONNECTING
};
WiFiState wifiState = WIFI_STA_CONNECTING;

// --- BME280 ---
Adafruit_BME280 bme;
bool bmeOK = false;
unsigned long lastBmeRetry = 0;

struct SensorData {
  float t;
  float h;
  float p;
};
SensorData currentData = {0.0f, 0.0f, 0.0f};

// --- Кольцевой буфер ---
SensorData history[HISTORY_SIZE];
uint16_t histHead = 0;
uint16_t histCount = 0;

// --- NTP / Время ---
bool ntpSynced = false;
unsigned long ntpBaseOffset = 0;  // epoch - (millis()/1000) в момент синхронизации

// --- Таймеры (неблокирующие) ---
unsigned long last60sRead = 0;
unsigned long last5sRead = 0;
unsigned long wifiStateTimer = 0;

// --- Сеть ---
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dnsServer;
bool dnsActive = false;

// ============================================================
// HTML DASHBOARD (PROGMEM)
// ============================================================


// ============================================================
// ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ============================================================

static bool parseBSSID(const char* str, uint8_t mac[6]) {
  if (strlen(str) < 17) return false;
  unsigned int v[6];
  if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0],&v[1],&v[2],&v[3],&v[4],&v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
  // Проверка "все нули" = игнорировать
  bool allZero = true;
  for (int i = 0; i < 6; i++) if (mac[i] != 0) { allZero = false; break; }
  return !allZero;
}

static int rssiToPercent(int32_t rssi) {
  if (rssi <= -100) return 0;
  if (rssi >= -50) return 100;
  return 2 * (rssi + 100);
}

static unsigned long getEpochNow() {
  if (ntpSynced) {
    return ntpBaseOffset + (millis() / 1000);
  }
  return millis() / 1000;  // uptime как fallback
}

// ============================================================
// BME280
// ============================================================

void initBME() {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);  // I2C 400 kHz

  if (bme.begin(BME_ADDR_PRIMARY, &Wire)) {
    Serial.println(F("[BME] Найден на 0x76"));
  } else if (bme.begin(BME_ADDR_SECONDARY, &Wire)) {
    Serial.println(F("[BME] Найден на 0x77"));
  } else {
    Serial.println(F("[BME] ОШИБКА: датчик не обнаружен!"));
    bmeOK = false;
    return;
  }

  bmeOK = true;
  // Порядок аргументов: mode, tempOS, pressOS, humOS, filter, standby
  bme.setSampling(
    Adafruit_BME280::MODE_NORMAL,
    Adafruit_BME280::SAMPLING_X2,   // Temperature
    Adafruit_BME280::SAMPLING_X2,   // Pressure
    Adafruit_BME280::SAMPLING_X2,   // Humidity
    Adafruit_BME280::FILTER_X16,    // Noise filter
    Adafruit_BME280::STANDBY_MS_1000
  );
  Serial.println(F("[BME] Инициализирован (Normal, x2, Filter x16)"));
}

void readBME() {
  // Авто-реинициализация при отвале (не чаще раза в 30с)
  if (!bmeOK) {
    if (millis() - lastBmeRetry >= BME_RETRY_MS) {
      lastBmeRetry = millis();
      Serial.println(F("[BME] Попытка реинициализации..."));
      initBME();
    }
    if (!bmeOK) return;
  }

  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0f;  // Pa → hPa

  // NaN / sanity check
  if (isnan(t) || isnan(h) || isnan(p) || p < 300.0f || p > 1100.0f) {
    Serial.println(F("[BME] Ошибка чтения (NaN). Датчик отвалился."));
    bmeOK = false;
    lastBmeRetry = millis();
    return;
  }

  currentData.t = t;
  currentData.h = h;
  currentData.p = p;
}

void addHistoryPoint() {
  history[histHead] = currentData;
  histHead = (histHead + 1) % HISTORY_SIZE;
  if (histCount < HISTORY_SIZE) histCount++;
}

// ============================================================
// WI-FI STATE MACHINE
// ============================================================

void wifiStartSTA() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  uint8_t bssid[6];
  if (parseBSSID(WIFI_BSSID_STR, bssid)) {
    WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL, bssid);
    Serial.printf("[WiFi] STA: %s (BSSID: %s)\n", WIFI_SSID, WIFI_BSSID_STR);
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[WiFi] STA: %s\n", WIFI_SSID);
  }
  wifiStateTimer = millis();
  wifiState = WIFI_STA_CONNECTING;
}

void wifiStartAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(AP_SSID, AP_PASS);
  // Captive portal: DNS redirect всех доменов на ESP32
  dnsServer.start(53, "*", IPAddress(192,168,4,1));
  dnsActive = true;
  wifiState = WIFI_AP_ONLY;
  wifiStateTimer = millis();
  Serial.printf("[WiFi] AP запущен: %s (192.168.4.1)\n", AP_SSID);
}

void wifiStopAP() {
  if (dnsActive) {
    dnsServer.stop();
    dnsActive = false;
  }
  WiFi.softAPdisconnect(true);
}

void handleWiFi() {
  switch (wifiState) {

    case WIFI_STA_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        wifiState = WIFI_STA_CONNECTED;
        Serial.printf("[WiFi] STA подключен! IP: %s\n", WiFi.localIP().toString().c_str());
        // NTP
        configTime(NTP_UTC_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov");
      } else if (millis() - wifiStateTimer > STA_TIMEOUT_MS) {
        Serial.println(F("[WiFi] Таймаут STA (60с). Переход в AP."));
        wifiStartAP();
      }
      break;

    case WIFI_STA_CONNECTED:
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[WiFi] Потеряно STA-соединение!"));
        wifiStateTimer = millis();
        wifiState = WIFI_STA_CONNECTING;
        WiFi.disconnect(false);
        WiFi.reconnect();
      } else if (!ntpSynced) {
        time_t now;
        time(&now);
        if (now > 1700000000) {  // > Nov 2023 = валидный NTP
          ntpBaseOffset = (unsigned long)now - (millis() / 1000);
          ntpSynced = true;
          Serial.printf("[NTP] Синхронизирован. Epoch: %lu\n", (unsigned long)now);
        }
      }
      break;

    case WIFI_AP_ONLY:
      if (millis() - wifiStateTimer > AP_RETRY_INTERVAL_MS) {
        Serial.println(F("[WiFi] Фоновая попытка STA (AP_STA)..."));
        WiFi.mode(WIFI_AP_STA);
        WiFi.setSleep(false);
        uint8_t bssid[6];
        if (parseBSSID(WIFI_BSSID_STR, bssid)) {
          WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL, bssid);
        } else {
          WiFi.begin(WIFI_SSID, WIFI_PASS);
        }
        wifiState = WIFI_AP_STA_RECONNECTING;
        wifiStateTimer = millis();
      }
      break;

    case WIFI_AP_STA_RECONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] STA восстановлен! IP: %s\n", WiFi.localIP().toString().c_str());
        wifiStopAP();
        WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);
        wifiState = WIFI_STA_CONNECTED;
        ntpSynced = false;  // Перезапросим NTP
        configTime(NTP_UTC_OFFSET_SEC, 0, "pool.ntp.org", "time.nist.gov");
      } else if (millis() - wifiStateTimer > AP_RETRY_TIMEOUT_MS) {
        Serial.println(F("[WiFi] Не удалось. Возврат в AP."));
        WiFi.disconnect(false);
        WiFi.mode(WIFI_AP);
        wifiState = WIFI_AP_ONLY;
        wifiStateTimer = millis();
      }
      break;
  }
}

// ============================================================
// JSON HELPERS
// ============================================================

void fillStatusJSON(JsonDocument& doc) {
  if (bmeOK) {
    doc["temp"]  = serialized(String(currentData.t, 2));
    doc["hum"]   = serialized(String(currentData.h, 2));
    doc["press"] = serialized(String(currentData.p, 2));
  } else {
    doc["temp"]  = nullptr;
    doc["hum"]   = nullptr;
    doc["press"] = nullptr;
  }

  bool staActive = (WiFi.status() == WL_CONNECTED);
  doc["wifi_ssid"]       = staActive ? WiFi.SSID() : String(AP_SSID);
  doc["wifi_rssi_dbm"]   = staActive ? WiFi.RSSI() : 0;
  doc["wifi_signal_pct"] = staActive ? rssiToPercent(WiFi.RSSI()) : 100;
  doc["wifi_channel"]    = staActive ? WiFi.channel() : 0;
  doc["uptime_sec"]      = (uint32_t)(millis() / 1000);
  // ФИКС: явный флаг вместо того, чтобы фронтенд угадывал статус по RSSI==0,
  // что ложно срабатывает при реальном RSSI ровно 0 dBm (близко к роутеру).
  doc["sta_connected"]   = staActive;
}
// ============================================================
// WEBSOCKET
// ============================================================

void onWsEvent(AsyncWebSocket *srv, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("[WS] Клиент #%u подключен\n", client->id());
    // Сразу отправляем текущие данные
    JsonDocument doc;
    fillStatusJSON(doc);
    String msg;
    serializeJson(doc, msg);
    client->text(msg);
  } else if (type == WS_EVT_DISCONNECT) {
    Serial.printf("[WS] Клиент #%u отключен\n", client->id());
  }
}

void wsBroadcast() {
  if (ws.count() == 0) return;
  JsonDocument doc;
  fillStatusJSON(doc);
  String msg;
  serializeJson(doc, msg);
  ws.textAll(msg);
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(50);  // Единственный delay — для стабилизации Serial (не влияет на логику)
  Serial.println(F("\n=== ESP32 Climate Station ==="));

  // 1. BME280
  initBME();
  readBME();  // Первое чтение

  // 2. WiFi
  wifiStartSTA();

  // 3. WebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // 4. HTTP Routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    fillStatusJSON(doc);
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;

    uint16_t total = histCount;
    if (total == 0) {
      doc["base_timestamp"] = (uint32_t)getEpochNow();
      doc["interval_sec"] = HISTORY_INTERVAL;
      doc["points"].to<JsonArray>();
      String out;
      serializeJson(doc, out);
      req->send(200, "application/json", out);
      return;
    }

    // Даунсемплинг
    uint16_t step = (total > MAX_JSON_POINTS) ? (total / MAX_JSON_POINTS) : 1;
    if (step < 1) step = 1;

    unsigned long nowEpoch = getEpochNow();
    uint16_t startIdx = (histHead - total + HISTORY_SIZE) % HISTORY_SIZE;

    // base_timestamp = время самой старой (i=0) точки в буфере — она всегда
    // попадает в выборку при любом step, так что сама формула была верной.
    // ФИКС: реальная проблема была в клиентском JS (index_html.h) — там для
    // построения оси X использовался base_timestamp + i*interval_sec, что
    // корректно ТОЛЬКО если каждая история строго соответствует шагу step.
    // См. фикс в index_html.h — там же добавлена защита от рассинхрона.
    unsigned long oldestEpoch = nowEpoch - ((unsigned long)total * HISTORY_INTERVAL);

    doc["base_timestamp"] = (uint32_t)oldestEpoch;
    doc["interval_sec"] = HISTORY_INTERVAL * step;  // Учитываем прореживание!

    JsonArray arr = doc["points"].to<JsonArray>();

    for (uint16_t i = 0; i < total; i += step) {
      uint16_t idx = (startIdx + i) % HISTORY_SIZE;
      JsonObject pt = arr.add<JsonObject>();
      pt["t"] = serialized(String(history[idx].t, 1));
      pt["h"] = serialized(String(history[idx].h, 1));
      pt["p"] = serialized(String(history[idx].p, 1));
    }

    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // --- Captive portal detection endpoints ---
  // ФИКС: ОС (Android/iOS/Windows) при подключении к AP делают запрос на
  // спец. URL, чтобы понять, есть ли интернет / нужно ли открыть portal.
  // Раньше на все эти запросы отдавался редирект на "/", но ОС ожидают
  // СТРОГО определённые коды и content-type — иначе portal либо не
  // открывается автоматически, либо система решает, что интернета нет
  // и сворачивает уведомление (это и было на первом скриншоте).
  auto isApMode = []() {
    return wifiState == WIFI_AP_ONLY || wifiState == WIFI_AP_STA_RECONNECTING;
  };

  // Android
  server.on("/generate_204", HTTP_GET, [isApMode](AsyncWebServerRequest *req) {
    if (isApMode()) req->redirect("/");
    else req->send(204);
  });
  server.on("/gen_204", HTTP_GET, [isApMode](AsyncWebServerRequest *req) {
    if (isApMode()) req->redirect("/");
    else req->send(204);
  });
  // iOS / macOS
  server.on("/hotspot-detect.html", HTTP_GET, [isApMode](AsyncWebServerRequest *req) {
    if (isApMode()) req->redirect("/");
    else req->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
  });
  server.on("/library/test/success.html", HTTP_GET, [isApMode](AsyncWebServerRequest *req) {
    if (isApMode()) req->redirect("/");
    else req->send(200, "text/html", "Success");
  });
  // Windows
  server.on("/ncsi.txt", HTTP_GET, [isApMode](AsyncWebServerRequest *req) {
    if (isApMode()) req->redirect("/");
    else req->send(200, "text/plain", "Microsoft NCSI");
  });
  server.on("/connecttest.txt", HTTP_GET, [isApMode](AsyncWebServerRequest *req) {
    if (isApMode()) req->redirect("/");
    else req->send(200, "text/plain", "Microsoft Connect Test");
  });

  // Captive portal redirect (fallback для всего остального в AP-режиме)
  server.onNotFound([isApMode](AsyncWebServerRequest *req) {
    if (isApMode()) {
      req->redirect("/");
    } else {
      req->send(404, "text/plain", "Not Found");
    }
  });

  server.begin();
  Serial.println(F("[HTTP] Сервер запущен на :80"));
}

// ============================================================
// LOOP (полностью неблокирующий)
// ============================================================

void loop() {
  unsigned long now = millis();

  // 1. WiFi state machine
  handleWiFi();

  // 2. Captive portal DNS (только в AP-режиме)
  if (dnsActive) {
    dnsServer.processNextRequest();
  }

  // 3. Чтение BME280 для истории (строго раз в 60с)
  if (now - last60sRead >= SENSOR_60S_MS) {
    last60sRead = now;
    readBME();
    if (bmeOK) {
      addHistoryPoint();
    }
  }

  // 4. Чтение + WebSocket broadcast (раз в 5с, только при наличии клиентов)
  if (ws.count() > 0 && (now - last5sRead >= SENSOR_5S_MS)) {
    last5sRead = now;
    readBME();
    wsBroadcast();
  }

  // 5. Очистка мёртвых WebSocket-клиентов
  ws.cleanupClients();
}