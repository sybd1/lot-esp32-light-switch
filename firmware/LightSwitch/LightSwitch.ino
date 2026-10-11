#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <time.h>
#include "driver/ledc.h"
#include "secrets.h"
#include "index_html.h"

// 각도만 나중에 바꾸면 된다. 왼쪽과 오른쪽의 사이 각도로 돌아온 뒤 힘을 뺀다.
static const int ANGLE_LEFT = 45;
static const int ANGLE_RIGHT = 135;
static const int ANGLE_MID = (ANGLE_LEFT + ANGLE_RIGHT) / 2;
static const int PRESS_ON_ANGLE = 105;
static const int PRESS_OFF_ANGLE = 60;

static const int SERVO_PIN = 13;
static const uint32_t PRESS_HOLD_MS = 2000;
static const uint32_t RETURN_SETTLE_MS = 500;
static const uint8_t SERVO_BITS = 14;
static const int SERVO_MIN_US = 500;
static const int SERVO_MAX_US = 2400;
static const char *AP_SSID = "LightSwitch";
static const char *AP_PASSWORD = "lightswitch";
static const int RULE_LIMIT = 8;

enum class Job : uint8_t { None, On, Off, Left, Right };
enum class Phase : uint8_t { Idle, Pressing, Returning };

struct Rule {
  uint8_t used;
  uint8_t action;
  uint8_t hour;
  uint8_t minute;
  uint8_t days;
  uint8_t repeat;
  uint8_t enabled;
  uint8_t reserved;
};

static WebServer server(80);
static Preferences prefs;
static Rule rules[RULE_LIMIT];
static bool setupMode = false;
static bool timerReady = false;
static bool signalOn = false;
static int lightState = -1;
static Job active = Job::None;
static Job queued = Job::None;
static Phase phase = Phase::Idle;
static uint32_t phaseStart = 0;
static int lastFiredMinute = -1;
static uint32_t lastWifiTry = 0;
static String staSsid;
static String staPass;

static int angleFor(Job job) {
  switch (job) {
    case Job::On: return PRESS_ON_ANGLE;
    case Job::Off: return PRESS_OFF_ANGLE;
    case Job::Left: return ANGLE_LEFT;
    case Job::Right: return ANGLE_RIGHT;
    default: return ANGLE_MID;
  }
}

static void rememberLight(int state) {
  lightState = state;
  prefs.putInt("state", state);
}

static void loadRules() {
  memset(rules, 0, sizeof(rules));
  size_t size = prefs.getBytesLength("rules");
  if (size == sizeof(rules)) prefs.getBytes("rules", rules, sizeof(rules));
  lightState = prefs.getInt("state", -1);
}

static void saveRules() {
  prefs.putBytes("rules", rules, sizeof(rules));
}

static uint32_t dutyForAngle(int angle) {
  int pulse = map(angle, 0, 180, SERVO_MIN_US, SERVO_MAX_US);
  uint32_t maxDuty = (1u << SERVO_BITS) - 1u;
  return (uint32_t)pulse * maxDuty / 20000u;
}

static bool driveAngle(int angle) {
  angle = constrain(angle, 0, 180);
  uint32_t duty = dutyForAngle(angle);
  if (!timerReady) {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.timer_num = LEDC_TIMER_0;
    timer.duty_resolution = LEDC_TIMER_14_BIT;
    timer.freq_hz = 50;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK) {
      Serial.println("서보 타이머 설정 실패");
      return false;
    }
    ledc_channel_config_t channel = {};
    channel.gpio_num = SERVO_PIN;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = duty;
    channel.hpoint = 0;
    if (ledc_channel_config(&channel) != ESP_OK) {
      Serial.println("서보 신호 설정 실패");
      return false;
    }
    timerReady = true;
    signalOn = true;
    return true;
  }
  if (ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty) != ESP_OK ||
      ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) != ESP_OK) {
    Serial.println("서보 각도 변경 실패");
    return false;
  }
  if (!signalOn) {
    if (ledc_set_pin(SERVO_PIN, LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) != ESP_OK) {
      Serial.println("서보 신호 연결 실패");
      return false;
    }
    signalOn = true;
  }
  return true;
}

static void releaseServo() {
  if (!signalOn) return;
  ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
  gpio_reset_pin((gpio_num_t)SERVO_PIN);
  pinMode(SERVO_PIN, INPUT);
  signalOn = false;
  Serial.println("힘 해제");
}

static void beginJob(Job job) {
  if (!driveAngle(angleFor(job))) return;
  active = job;
  phase = Phase::Pressing;
  phaseStart = millis();
  if (job == Job::On) rememberLight(1);
  if (job == Job::Off) rememberLight(0);
  Serial.printf("누름 각도 %d\n", angleFor(job));
}

static const char *requestJob(Job job) {
  if (setupMode) return "setup";
  if (phase == Phase::Idle) {
    beginJob(job);
    return active == Job::None ? "failed" : "started";
  }
  queued = job;
  return "busy";
}

static void finishJob() {
  releaseServo();
  active = Job::None;
  phase = Phase::Idle;
  if (queued == Job::None) return;
  Job next = queued;
  queued = Job::None;
  beginJob(next);
}

static void updateMotion() {
  if (phase == Phase::Pressing && (active == Job::On || active == Job::Off) &&
      millis() - phaseStart >= RETURN_SETTLE_MS) {
    finishJob();
    return;
  }
  if (phase == Phase::Pressing && millis() - phaseStart >= PRESS_HOLD_MS) {
    if (!driveAngle(ANGLE_MID)) {
      finishJob();
      return;
    }
    phase = Phase::Returning;
    phaseStart = millis();
    Serial.printf("사이 각도 %d\n", ANGLE_MID);
  } else if (phase == Phase::Returning && millis() - phaseStart >= RETURN_SETTLE_MS) {
    finishJob();
  }
}

static bool clockNow(struct tm &timeinfo) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  localtime_r(&now, &timeinfo);
  return timeinfo.tm_year + 1900 >= 2024;
}

static void runSchedules() {
  struct tm timeinfo;
  if (!clockNow(timeinfo)) return;
  int minuteOfDay = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  if (minuteOfDay == lastFiredMinute) return;
  lastFiredMinute = minuteOfDay;
  for (int i = 0; i < RULE_LIMIT; i++) {
    Rule &rule = rules[i];
    if (!rule.used || !rule.enabled) continue;
    if (rule.hour != timeinfo.tm_hour || rule.minute != timeinfo.tm_min) continue;
    if ((rule.days & (1u << timeinfo.tm_wday)) == 0) continue;
    requestJob(rule.action ? Job::On : Job::Off);
    if (!rule.repeat) {
      rule.enabled = 0;
      saveRules();
    }
  }
}

static String jsonState() {
  struct tm timeinfo;
  bool synced = clockNow(timeinfo);
  char clock[24] = "";
  if (synced) {
    snprintf(clock, sizeof(clock), "%04d-%02d-%02d %02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min);
  }
  String ip = WiFi.localIP().toString();
  String body = "{\"light\":";
  body += lightState;
  body += ",\"busy\":";
  body += (phase == Phase::Idle ? "false" : "true");
  body += ",\"time\":\"";
  body += clock;
  body += "\",\"ip\":\"";
  body += ip;
  body += "\",\"rules\":[";
  bool first = true;
  for (int i = 0; i < RULE_LIMIT; i++) {
    if (!rules[i].used) continue;
    if (!first) body += ",";
    first = false;
    body += "{\"id\":";
    body += i;
    body += ",\"action\":";
    body += rules[i].action;
    body += ",\"hour\":";
    body += rules[i].hour;
    body += ",\"minute\":";
    body += rules[i].minute;
    body += ",\"days\":";
    body += rules[i].days;
    body += ",\"repeat\":";
    body += rules[i].repeat ? "true" : "false";
    body += ",\"enabled\":";
    body += rules[i].enabled ? "true" : "false";
    body += "}";
  }
  body += "]}";
  return body;
}

static void sendJson(int code, const String &body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

static Job jobFromText(const String &name) {
  if (name == "on") return Job::On;
  if (name == "off") return Job::Off;
  if (name == "left") return Job::Left;
  if (name == "right") return Job::Right;
  return Job::None;
}

static void handleMove() {
  Job job = jobFromText(server.arg("to"));
  if (job == Job::None) {
    sendJson(400, "{\"result\":\"bad\"}");
    return;
  }
  const char *result = requestJob(job);
  String body = "{\"result\":\"";
  body += result;
  body += "\"}";
  sendJson(200, body);
}

static void handlePlainOn() {
  const char *result = requestJob(Job::On);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", result);
}

static void handlePlainOff() {
  const char *result = requestJob(Job::Off);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", result);
}

static void handleAddRule() {
  String timeText = server.arg("time");
  int hour = timeText.substring(0, 2).toInt();
  int minute = timeText.substring(3, 5).toInt();
  if (timeText.length() < 5 || hour > 23 || minute > 59) {
    sendJson(400, "{\"result\":\"bad-time\"}");
    return;
  }
  uint8_t days = 0;
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) != "d") continue;
    int day = server.arg(i).toInt();
    if (day >= 0 && day <= 6) days |= (1u << day);
  }
  if (days == 0) {
    sendJson(400, "{\"result\":\"bad-day\"}");
    return;
  }
  int slot = -1;
  for (int i = 0; i < RULE_LIMIT; i++) {
    if (!rules[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    sendJson(400, "{\"result\":\"full\"}");
    return;
  }
  rules[slot].used = 1;
  rules[slot].action = server.arg("action") == "off" ? 0 : 1;
  rules[slot].hour = hour;
  rules[slot].minute = minute;
  rules[slot].days = days;
  rules[slot].repeat = server.hasArg("repeat") ? 1 : 0;
  rules[slot].enabled = 1;
  saveRules();
  sendJson(200, "{\"result\":\"saved\"}");
}

static void handleDeleteRule() {
  int id = server.arg("id").toInt();
  if (id < 0 || id >= RULE_LIMIT || !rules[id].used) {
    sendJson(400, "{\"result\":\"bad\"}");
    return;
  }
  memset(&rules[id], 0, sizeof(Rule));
  saveRules();
  sendJson(200, "{\"result\":\"deleted\"}");
}

static void handleWifiReset() {
  prefs.remove("ssid");
  prefs.remove("pass");
  server.send(200, "text/plain", "reset");
  delay(300);
  ESP.restart();
}

static void handleWifiSave() {
  String ssid = server.arg("ssid");
  String password = server.arg("password");
  ssid.trim();
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 63) {
    server.send(400, "text/plain", "bad");
    return;
  }
  prefs.putString("ssid", ssid);
  prefs.putString("pass", password);
  server.send(200, "text/html", "<!DOCTYPE html><meta charset=utf-8><p>저장했다. 보드가 집 Wi-Fi로 다시 시작된다.</p>");
  delay(400);
  ESP.restart();
}

static void addRoutes() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    if (setupMode) server.send_P(200, "text/html", SETUP_HTML);
    else server.send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/favicon.ico", HTTP_GET, []() { server.send(204); });
  if (setupMode) {
    server.on("/wifi", HTTP_POST, handleWifiSave);
    return;
  }
  server.on("/api/state", HTTP_GET, []() { sendJson(200, jsonState()); });
  server.on("/api/move", HTTP_POST, handleMove);
  server.on("/api/rules", HTTP_POST, handleAddRule);
  server.on("/api/rules/delete", HTTP_POST, handleDeleteRule);
  server.on("/api/wifi/reset", HTTP_POST, handleWifiReset);
  server.on("/on", HTTP_GET, handlePlainOn);
  server.on("/off", HTTP_GET, handlePlainOff);
}

static void startSetupAp() {
  setupMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.println("설정용 Wi-Fi: LightSwitch / lightswitch");
  Serial.println("주소: http://192.168.4.1");
  addRoutes();
  server.begin();
}

static void startHomeWifi() {
  setupMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("light-switch");
  WiFi.begin(staSsid.c_str(), staPass.c_str());
  Serial.printf("집 Wi-Fi 연결 중: %s\n", staSsid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("집 Wi-Fi에 붙지 못했다. 설정 화면으로 전환한다.");
    startSetupAp();
    return;
  }
  configTzTime("KST-9", "pool.ntp.org", "time.google.com");
  if (MDNS.begin("light-switch")) Serial.println("주소 이름: http://light-switch.local");
  Serial.print("주소: http://");
  Serial.println(WiFi.localIP());
  Serial.print("도착 시 열 주소: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/on");
  addRoutes();
  server.begin();
}

static void loadWifi() {
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("pass", "");
  if (staSsid.length() == 0 && WIFI_SSID[0] != '\0') {
    staSsid = WIFI_SSID;
    staPass = WIFI_PASSWORD;
  }
}

static void maintainWifi() {
  if (setupMode || WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry < 10000) return;
  lastWifiTry = millis();
  Serial.println("Wi-Fi 재연결");
  WiFi.begin(staSsid.c_str(), staPass.c_str());
}

static void pollSerial() {
  if (!Serial.available()) return;
  char key = Serial.read();
  if (key == 'l') requestJob(Job::Left);
  if (key == 'r') requestJob(Job::Right);
  if (key == 'n') requestJob(Job::On);
  if (key == 'f') requestJob(Job::Off);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("형광등 스위치");
  Serial.println("누른 뒤 2초 후에 사이 각도로 돌아오고 힘을 뺀다.");
  prefs.begin("light", false);
  loadRules();
  loadWifi();
  if (staSsid.length() == 0) startSetupAp();
  else startHomeWifi();
}

void loop() {
  server.handleClient();
  updateMotion();
  if (!setupMode) {
    maintainWifi();
    runSchedules();
    pollSerial();
  }
}
