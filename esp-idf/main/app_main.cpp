// main/app_main.cpp
//
// StateMQ ESP-IDF example: state-driven MQTT control.
//
// MQTT interface (what to publish / subscribe):
// - Publish to:   lab/node/in
//   Messages:     "run", "stop", "pattern"
//
// - Device publishes to: lab/node/log
//   Messages:     human-readable status text (retained)
//
// - Optional chat: subscribe to hello/chat
//   Messages:     printed to UART
//

#include <cstring>
#include <cstdio>

#include "sdkconfig.h"
#include "StateMQ_ESP.h"

#include "driver/gpio.h"
#include "esp_timer.h"

using namespace statemq;

// ---------------- pins ----------------
static constexpr gpio_num_t LED1 = GPIO_NUM_21;
static constexpr gpio_num_t LED2 = GPIO_NUM_22;

// invert if active-low
static constexpr bool LED1_INVERT = false;
static constexpr bool LED2_INVERT = false;

// ---------------- topics ----------------
static constexpr const char* STATE_TOPIC = "lab/node/in";
static constexpr const char* LOG_TOPIC   = "lab/node/log";
static constexpr const char* CHAT_TOPIC  = "hello/chat";

// ---------------- node ----------------
static StateMQ node;
static StateMQEsp esp(node);

using StateId = StateMQ::StateId;
static StateId RUNNING_ID = StateMQ::CONNECTED_ID;
static StateId IDLE_ID    = StateMQ::CONNECTED_ID;
static StateId PATTERN_ID = StateMQ::CONNECTED_ID;

// ---------------- helpers ----------------
static void setLed(gpio_num_t pin, bool invert, bool on) {
  int level = on ? 1 : 0;
  if (invert) level = !level;
  gpio_set_level(pin, level);
}

static uint32_t nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool offlineBlink200() {
  uint32_t t = nowMs();
  return ((t / 200U) & 1U) != 0;
}

static bool patternLed1() {
  uint32_t t = nowMs() % 1600U;
  if (t < 200)  return true;
  if (t < 400)  return false;
  if (t < 600)  return true;
  if (t < 800)  return false;
  if (t < 1000) return true;
  return false;
}

static bool patternLed2() {
  uint32_t t = nowMs() % 2000U;
  if (t < 150) return true;
  if (t < 300) return false;
  if (t < 450) return true;
  if (t < 600) return false;
  return true;
}

// ---------------- context structs ----------------
struct LedTaskCtx {
  gpio_num_t pin;
  bool invert;
  bool usePattern; 
};

struct PubTaskCtx {
  const char* topic;
  int qos;
  bool retain;
};

struct ChatTaskCtx {
  const char* topic;
};

// ---------------- context-aware tasks ----------------
static void ledTask(void* u) {
  auto* c = static_cast<LedTaskCtx*>(u);
  if (!c) return;

  const StateId s = node.stateId();
  bool on = false;

  if (s == StateMQ::OFFLINE_ID) {
    on = offlineBlink200();
  } else if (s == StateMQ::CONNECTED_ID) {
    on = false;
  } else if (s == IDLE_ID) {
    on = false;
  } else if (s == RUNNING_ID) {
    on = true;
  } else {
    on = c->usePattern ? (c->pin == LED1 ? patternLed1() : patternLed2()) : false;
  }

  setLed(c->pin, c->invert, on);
}

static void chatTask(void* u) {
  auto* c = static_cast<ChatTaskCtx*>(u);
  if (!c || !c->topic) return;

  const char* m = esp.msg(c->topic);
  if (!m) return;
  printf("[chat] %s\n", m);
}

static void publishTask(void* u) {
  auto* c = static_cast<PubTaskCtx*>(u);
  if (!c || !c->topic) return;

  static StateId last = StateMQ::OFFLINE_ID;

  StateId now = node.stateId();
  if (now == last) return;
  last = now;

  const char* msg = nullptr;

  if (now == PATTERN_ID)                 msg = "leds following their own pattern";
  else if (now == RUNNING_ID)            msg = "run: leds ON";
  else if (now == IDLE_ID)               msg = "idle: leds OFF";
  else if (now == StateMQ::OFFLINE_ID)   msg = "offline: blinking";
  else if (now == StateMQ::CONNECTED_ID) msg = "connected: awaiting state";
  else                                   msg = "state changed";

  esp.publish(c->topic, msg, c->qos, c->retain);
}

extern "C" void app_main(void) {
  // GPIO setup
  gpio_config_t io{};
  io.mode = GPIO_MODE_OUTPUT;
  io.pin_bit_mask = (1ULL << LED1) | (1ULL << LED2);
  gpio_config(&io);

  setLed(LED1, LED1_INVERT, false);
  setLed(LED2, LED2_INVERT, false);

  // MQTT (topic,payload) -> StateId
  RUNNING_ID = node.map(STATE_TOPIC, "run",     "RUNNING");
  IDLE_ID    = node.map(STATE_TOPIC, "stop",    "IDLE");
  PATTERN_ID = node.map(STATE_TOPIC, "pattern", "PATTERN");

  // Context instances (static lifetime)
  static LedTaskCtx led1Ctx{ .pin = LED1, .invert = LED1_INVERT, .usePattern = true };
  static LedTaskCtx led2Ctx{ .pin = LED2, .invert = LED2_INVERT, .usePattern = true };

  static PubTaskCtx pubCtx{ .topic = LOG_TOPIC, .qos = 2, .retain = true };
  static ChatTaskCtx chatCtx{ .topic = CHAT_TOPIC };

  // tasks (context-aware overload)
  node.taskEvery("led1", 200, Stack::Small, ledTask, &led1Ctx, true);
  node.taskEvery("led2", 200, Stack::Small, ledTask, &led2Ctx, true);
  node.taskEvery("pub",  200, Stack::Small, publishTask, &pubCtx, true);
  node.taskEvery("chat", 50,  Stack::Small, chatTask, &chatCtx, true);

  // Publish upon state transition
  esp.StatePublishTopic("lab/node/status", /*qos=*/1, /*retain*/true, /*enable=*/true);

  // Subscription to unmapped topic
  esp.subscribe(CHAT_TOPIC, 0);

  esp.setKeepAliveSeconds(5);
  esp.setLastWill("lab/node/lwt", "offline", 2, true);

  // menuconfig credentials
  const char* ssid   = CONFIG_STATEMQ_WIFI_SSID;
  const char* pass   = CONFIG_STATEMQ_WIFI_PASS;
  const char* broker = CONFIG_STATEMQ_BROKER_URI;

  // Start Wi-Fi and MQTT
  esp.begin(ssid, pass, broker);
}
