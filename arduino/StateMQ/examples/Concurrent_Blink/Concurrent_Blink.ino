#include <Arduino.h>
#include <StateMQ_ESP32.h>

using StateId = StateMQ::StateId;

// ---------------- pins ----------------
static constexpr int LED1 = 21;
static constexpr int LED2 = 22;

// invert if active-low
static constexpr bool LED1_INVERT = false;
static constexpr bool LED2_INVERT = false;

// ---------------- topics ----------------
static constexpr const char* STATE_TOPIC = "lab/node/in";
static constexpr const char* LOG_TOPIC   = "lab/node/log";
static constexpr const char* LWT_TOPIC   = "lab/node/lwt";

// WiFi credentials
const char* WIFI_SSID = "your_wifi_ssid";
const char* WIFI_PASS = "your_wifi_password";

// MQTT broker address
const char* MQTT_BROKER = "mqtt://broker_ip:1883";

// ---------------- node ----------------
StateMQ node;
StateMQEsp32 esp(node);

static StateId RUNNING_ID = CONNECTED_ID;
static StateId IDLE_ID    = CONNECTED_ID;
static StateId PATTERN_ID = CONNECTED_ID;

// ---------------- helpers ----------------
static inline void setLed(int pin, bool invert, bool on) {
  int level = on ? HIGH : LOW;
  if (invert) level = (level == HIGH) ? LOW : HIGH;
  digitalWrite(pin, level);
}

static inline bool offlineBlink200() {
  uint32_t t = millis();
  return ((t / 200) & 1) != 0;
}

static inline bool patternLed1() {
  uint32_t t = millis() % 1600;
  if (t < 200)  return true;
  if (t < 400)  return false;
  if (t < 600)  return true;
  if (t < 800)  return false;
  if (t < 1000) return true;
  return false;
}

static inline bool patternLed2() {
  uint32_t t = millis() % 2000;
  if (t < 150) return true;
  if (t < 300) return false;
  if (t < 450) return true;
  if (t < 600) return false;
  return true;
}

// ---------------- LED tasks ----------------
void led1Task() {
  StateId s = node.stateId();
  bool on = false;

  if (s == OFFLINE_ID)        on = offlineBlink200();
  else if (s == CONNECTED_ID) on = false;
  else if (s == IDLE_ID)      on = false;
  else if (s == RUNNING_ID)   on = true;
  else                        on = patternLed1();

  setLed(LED1, LED1_INVERT, on);
}

void led2Task() {
  StateId s = node.stateId();
  bool on = false;

  if (s == OFFLINE_ID)        on = offlineBlink200();
  else if (s == CONNECTED_ID) on = false;
  else if (s == IDLE_ID)      on = false;
  else if (s == RUNNING_ID)   on = true;
  else                        on = patternLed2();

  setLed(LED2, LED2_INVERT, on);
}

// ---------------- publish task ----------------
void publishTask() {
  static StateId last = OFFLINE_ID;

  StateId now = node.stateId();
  if (now == last) return;
  last = now;

  const char* msg = "state changed";
  if (now == PATTERN_ID)         msg = "leds following their own pattern";
  else if (now == RUNNING_ID)    msg = "run: leds ON";
  else if (now == IDLE_ID)       msg = "idle: leds OFF";
  else if (now == OFFLINE_ID)    msg = "offline: blinking";
  else if (now == CONNECTED_ID)  msg = "connected: awaiting state";

  // retained so you always see last log even if you subscribe late
  esp.publish(LOG_TOPIC, msg, 2, true);
}

void setup() {
  Serial.begin(115200);

  pinMode(LED1, OUTPUT);
  pinMode(LED2, OUTPUT);
  setLed(LED1, LED1_INVERT, false);
  setLed(LED2, LED2_INVERT, false);

  // commands -> states
  RUNNING_ID = node.map(STATE_TOPIC, "run",     "RUNNING");
  IDLE_ID    = node.map(STATE_TOPIC, "stop",    "IDLE");
  PATTERN_ID = node.map(STATE_TOPIC, "pattern", "PATTERN");

  // tasks (FreeRTOS tasks created by the wrapper)
  node.taskEvery("led1", 200, small, led1Task, true);
  node.taskEvery("led2", 200, small, led2Task, true);
  node.taskEvery("pub",  200, small, publishTask, true);

  // MQTT config via existing Arduino wrapper APIs
  esp.setDefaultSubscribeQos(2);
  esp.setSubscribeQos(STATE_TOPIC, 2);
  esp.setKeepAliveSeconds(1);
  
  //Publish State Change
  esp.StatePublishTopic("lab/node/status", /*qos=*/1, /*enable=*/true, /*retain=*/true);


  //Set Last Will Message
  esp.setLastWill(LWT_TOPIC, "offline", 2, true);

  // connect
  esp.begin(WIFI_SSID, WIFI_PASS, MQTT_BROKER);
}

void loop() {
  // No superloop, everything handled by tasks
}
