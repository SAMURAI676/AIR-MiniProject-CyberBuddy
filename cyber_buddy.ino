#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// ================= НАСТРОЙКИ =================
#define WIFI_SSID_1 "имя_домашней_сети"
#define WIFI_PASS_1 "пароль_домашней_сети"
#define WIFI_SSID_2 "имя_точки_доступа_телефона"
#define WIFI_PASS_2 "пароль_точки_доступа"

#define BOT_TOKEN "ТОКЕН_ОТ_BOTFATHER"
#define CHAT_ID   "ЧИСЛОВОЙ_ID"

#define DHTTYPE DHT11
// =================================================================

// ---------- Пины ----------
#define PIN_PIR    13   // HC-SR501 OUT
#define PIN_DHT    4    // DHT11 S (сигнал)
#define PIN_RELAY1 26   // Реле 1 (подсветка)
#define PIN_RELAY2 27   // Реле 2 (вентилятор)

// Если реле включается НИЗКИМ уровнем, поменяй HIGH и LOW местами
#define RELAY_ON  HIGH
#define RELAY_OFF LOW

DHT dht(PIN_DHT, DHTTYPE);

// ---------- OLED ----------
Adafruit_SSD1306 display(128, 64, &Wire, -1);

// ---------- Telegram ----------
WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);

// ---------- Настройки логики ----------
const float T_ON  = 28.0;                       // вход в ALERT
const float T_OFF = 27.0;                       // выход из ALERT (гистерезис)
const unsigned long PRESENCE_TIMEOUT = 5000;    // 5 с без движения -> IDLE
const unsigned long DHT_INTERVAL     = 2000;    // опрос DHT
const unsigned long BOT_INTERVAL     = 1000;    // опрос команд Telegram
const unsigned long GREET_COOLDOWN   = 300000;  // приветствие не чаще раза в 5 мин
const unsigned long ALERT_COOLDOWN   = 60000;   // тревога не чаще раза в минуту

// ---------- Конечный автомат ----------
enum SystemState { STATE_IDLE, STATE_ACTIVE, STATE_ALERT };
SystemState currentState = STATE_IDLE;

// ---------- Скользящее среднее ----------
const int numReadings = 5;
float readings[numReadings];
int readIndex = 0;
float total = 0;
float smoothedTemp = 0;
bool filled = false;
bool dhtOk = false;

// ---------- Служебные переменные ----------
bool overheat = false;              // перегрев (с гистерезисом)
volatile bool manualAlert = false;  // тревога, включённая командой /alert
bool present = false;               // человек рядом
bool haveMotion = false;
unsigned long lastMotion = 0;
unsigned long lastDht = 0;
unsigned long lastRender = 0;
unsigned long lastGreet = 0;
unsigned long lastAlertMsg = 0;
unsigned long lastBotCheck = 0;
bool greetedOnce = false;
bool alertedOnce = false;
bool botSynced = false;

// Прерывание PIR: запоминает срабатывание, даже если плата занята
volatile bool motionFlag = false;
void IRAM_ATTR onMotion() { motionFlag = true; }

// Очередь сообщений для Telegram (отправляет отдельная задача)
struct Msg { char text[256]; };
QueueHandle_t msgQueue;

// Wi-Fi: две сети по очереди, без блокировки программы
const char* wifiSsids[] = { WIFI_SSID_1, WIFI_SSID_2 };
const char* wifiPass[]  = { WIFI_PASS_1, WIFI_PASS_2 };
int wifiIdx = 0;
bool wifiTrying = false;
unsigned long wifiTryStart = 0;

void wifiService() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiTrying = false;
    return;
  }
  if (!wifiTrying || millis() - wifiTryStart > 10000) {
    if (wifiTrying) wifiIdx = (wifiIdx + 1) % 2;
    Serial.print("WiFi: пробую ");
    Serial.println(wifiSsids[wifiIdx]);
    WiFi.disconnect();
    WiFi.begin(wifiSsids[wifiIdx], wifiPass[wifiIdx]);
    wifiTrying = true;
    wifiTryStart = millis();
  }
}

void addReading(float t) {
  if (!filled) {
    for (int i = 0; i < numReadings; i++) readings[i] = t;
    total = t * numReadings;
    filled = true;
  } else {
    total -= readings[readIndex];
    readings[readIndex] = t;
    total += t;
    readIndex = (readIndex + 1) % numReadings;
  }
  smoothedTemp = total / numReadings;
}

const char* stateName(SystemState s) {
  switch (s) {
    case STATE_IDLE:   return "IDLE";
    case STATE_ACTIVE: return "ACTIVE";
    default:           return "ALERT";
  }
}

void applyOutputs() {
  switch (currentState) {
    case STATE_IDLE:
      digitalWrite(PIN_RELAY1, RELAY_OFF);
      digitalWrite(PIN_RELAY2, RELAY_OFF);
      break;
    case STATE_ACTIVE:
      digitalWrite(PIN_RELAY1, RELAY_ON);
      digitalWrite(PIN_RELAY2, RELAY_OFF);
      break;
    case STATE_ALERT:
      digitalWrite(PIN_RELAY1, RELAY_ON);
      digitalWrite(PIN_RELAY2, RELAY_ON);
      break;
  }
}

void render() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (filled) {
    display.print(smoothedTemp, 1);
    display.print(F(" C"));
  } else {
    display.print(F("T: --"));
  }
  if (!dhtOk) display.print(F(" ERR"));
  display.setCursor(104, 0);
  display.print(WiFi.status() == WL_CONNECTED ? F("WiFi") : F("----"));

  display.setTextSize(2);
  display.setCursor(22, 24);
  const char* modeText = "";
  switch (currentState) {
    case STATE_IDLE:   display.print(F("(- _ -)")); modeText = "Mode: SLEEP";    break;
    case STATE_ACTIVE: display.print(F("(^ _ ^)")); modeText = "Mode: WELCOME!"; break;
    case STATE_ALERT:  display.print(F("(o _ o)")); modeText = "ALERT!";         break;
  }
  display.setTextSize(1);
  display.setCursor(15, 52);
  display.print(modeText);
  display.display();
}

// Кладёт сообщение в очередь и сразу возвращается (не блокирует программу)
void notify(const String& text) {
  Msg m;
  text.toCharArray(m.text, sizeof(m.text));
  xQueueSend(msgQueue, &m, 0);
}

// Команды из Telegram: /status, /alert, /help
void handleTelegram() {
  unsigned long now = millis();
  if (now - lastBotCheck < BOT_INTERVAL) return;
  lastBotCheck = now;

  int n = bot.getUpdates(bot.last_message_received + 1);

  if (!botSynced) {   // старые сообщения, накопившиеся пока плата была выключена, пропускаем
    while (n) n = bot.getUpdates(bot.last_message_received + 1);
    botSynced = true;
    return;
  }

  while (n) {
    for (int i = 0; i < n; i++) {
      String chat = bot.messages[i].chat_id;
      if (chat != String(CHAT_ID)) continue;   // чужим пользователям не отвечаем
      String text = bot.messages[i].text;

      if (text.startsWith("/alert")) {
        manualAlert = !manualAlert;
        bot.sendMessage(chat, manualAlert ? "Тревога и вентилятор включены вручную (/alert чтобы выключить)"
                                          : "Ручная тревога выключена", "");
      } else if (text.startsWith("/status")) {
        String msg = "Температура: ";
        msg += filled ? String(smoothedTemp, 1) + " °C" : String("нет данных");
        msg += "\nСостояние: ";
        msg += stateName(currentState);
        msg += "\nПрисутствие: ";
        msg += present ? "да" : "нет";
        msg += "\nРучная тревога: ";
        msg += manualAlert ? "вкл" : "выкл";
        bot.sendMessage(chat, msg, "");
      } else {
        bot.sendMessage(chat, "Команды:\n/status - показать состояние\n/alert - вкл/выкл тревогу и вентилятор", "");
      }
    }
    n = bot.getUpdates(bot.last_message_received + 1);
  }
}

// Отдельная задача на втором ядре: вся работа с Telegram идёт здесь
void telegramTask(void* param) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      Msg m;
      while (xQueueReceive(msgQueue, &m, 0) == pdTRUE) {
        bot.sendMessage(CHAT_ID, String(m.text), "");
      }
      handleTelegram();
    }
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_PIR, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_PIR), onMotion, RISING);

  pinMode(PIN_RELAY1, OUTPUT);
  pinMode(PIN_RELAY2, OUTPUT);
  digitalWrite(PIN_RELAY1, RELAY_OFF);
  digitalWrite(PIN_RELAY2, RELAY_OFF);

  dht.begin();

  Wire.begin(21, 22);
  Wire.setClock(400000);   // быстрый режим шины: экран обновляется быстрее
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("Ошибка инициализации SSD1306"));
    for (;;) delay(1000);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 10);
  display.println(F("CyberBuddy Starting..."));
  display.display();

  WiFi.mode(WIFI_STA);
  secured_client.setInsecure();   // учебный проект: без проверки сертификата Telegram

  msgQueue = xQueueCreate(5, sizeof(Msg));
  xTaskCreatePinnedToCore(telegramTask, "tg", 16384, NULL, 1, NULL, 0);

  delay(1000);
}

void loop() {
  unsigned long now = millis();

  wifiService();

  // SENSE: PIR (прерывание + прямое чтение)
  if (motionFlag || digitalRead(PIN_PIR) == HIGH) {
    motionFlag = false;
    lastMotion = now;
    haveMotion = true;
  }
  present = haveMotion && (now - lastMotion < PRESENCE_TIMEOUT);

  // SENSE + PROCESS: DHT раз в 2 с, скользящее среднее
  if (now - lastDht >= DHT_INTERVAL) {
    lastDht = now;
    float t = dht.readTemperature();
    if (isnan(t)) {
      dhtOk = false;
    } else {
      dhtOk = true;
      addReading(t);
    }
    Serial.print("PIR: "); Serial.print(present);
    Serial.print(" | T: "); Serial.print(smoothedTemp);
    Serial.print(" | DHT ok: "); Serial.print(dhtOk);
    Serial.print(" | WiFi: "); Serial.print(WiFi.status());
    Serial.print(" | State: "); Serial.println(stateName(currentState));
  }

  // PROCESS: порог с гистерезисом
  if (filled) {
    if (!overheat && smoothedTemp > T_ON) overheat = true;
    else if (overheat && smoothedTemp < T_OFF) overheat = false;
  }

  // THINK: конечный автомат
  SystemState next = (overheat || manualAlert) ? STATE_ALERT
                   : (present ? STATE_ACTIVE : STATE_IDLE);
  bool changed = (next != currentState);
  currentState = next;

  // ACT: реле и экран сразу, Telegram через очередь
  applyOutputs();
  if (changed || now - lastRender >= 250) {
    lastRender = now;
    render();
  }

  if (changed) {
    if (currentState == STATE_ACTIVE && (!greetedOnce || now - lastGreet > GREET_COOLDOWN)) {
      greetedOnce = true;
      lastGreet = now;
      notify("Привет! Обнаружено присутствие за столом! (^ _ ^)");
    } else if (currentState == STATE_ALERT && overheat &&
               (!alertedOnce || now - lastAlertMsg > ALERT_COOLDOWN)) {
      alertedOnce = true;
      lastAlertMsg = now;
      notify("⚠️ Внимание! Высокая температура: " + String(smoothedTemp, 1) + " °C");
    }
  }
}
