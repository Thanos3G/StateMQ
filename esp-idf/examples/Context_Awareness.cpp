// main/app_main.cpp
//
// StateMQ ESP-IDF example: state-driven MQTT control.
// Prints full state-change context to UART.
//
// MQTT interface (incoming):
// - Publish to:   lab/node/state
//   Messages:     "run", "stop", "pattern"
//   Effect:       changes internal StateMQ state (integer StateId)
//
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
static constexpr const char* STATE_TOPIC = "lab/node/state";

// ---------------- node ----------------
static StateMQ node;
static StateMQEsp esp(node);

using StateId = StateMQ::StateId;
static StateId RUNNING_ID = StateMQ::CONNECTED_ID;
static StateId IDLE_ID    = StateMQ::CONNECTED_ID;
static StateId PATTERN_ID = StateMQ::CONNECTED_ID;

// ---------------- demo context ----------------
struct DemoCtx {
  gpio_num_t led1;
  gpio_num_t led2;
  bool led1Invert;
  bool led2Invert;
};

static DemoCtx* g_demo = nullptr;

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
  return ((t / 200) & 1U) != 0;
}

static bool patternLed1() {
  uint32_t t = nowMs() % 1600;
  if (t < 200)  return true;
  if (t < 400)  return false;
  if (t < 600)  return true;
  if (t < 800)  return false;
  if (t < 1000) return true;
  return false;
}

static bool patternLed2() {
  uint32_t t = nowMs() % 2000;
  if (t < 150) return true;
  if (t < 300) return false;
  if (t < 450) return true;
  if (t < 600) return false;
  return true;
}

static const char* causeName(StateMQ::StateChangeCause c) {
  switch (c) {
    case StateMQ::StateChangeCause::Unknown:   return "Unknown";
    case StateMQ::StateChangeCause::RuleMatch: return "RuleMatch";
    case StateMQ::StateChangeCause::Connected: return "Connected";
    case StateMQ::StateChangeCause::Disconn:   return "Disconn";
    default:                                   return "???";
  }
}

// ---------------- LED tasks ----------------
static void led1Task() {
  if (!g_demo) return;

  StateId s = node.stateId();
  bool on = false;

  if (s == StateMQ::OFFLINE_ID)       on = offlineBlink200();
  else if (s == StateMQ::CONNECTED_ID) on = false;
  else if (s == IDLE_ID)              on = false;
  else if (s == RUNNING_ID)           on = true;
  else                                on = patternLed1();

  setLed(g_demo->led1, g_demo->led1Invert, on);
}

static void led2Task() {
  if (!g_demo) return;

  StateId s = node.stateId();
  bool on = false;

  if (s == StateMQ::OFFLINE_ID)        on = offlineBlink200();
  else if (s == StateMQ::CONNECTED_ID) on = false;
  else if (s == IDLE_ID)               on = false;
  else if (s == RUNNING_ID)            on = true;
  else                                 on = patternLed2();

  setLed(g_demo->led2, g_demo->led2Invert, on);
}


// ---------------- state-change context print (FULL) ----------------
static void onStateChangeCtx(const StateMQ::StateChangeCtx& ctx) {
  const DemoCtx* demo = static_cast<const DemoCtx*>(ctx.user);

  const char* prevName    = node.stateName(ctx.prev);
  const char* desiredName = node.stateName(ctx.desired);
  const char* currName    = node.stateName(ctx.curr);

  // Also show what public APIs return *right now*
  const bool isConn = node.connected();
  const StateId apiId = node.stateId();
  const char* apiState = node.state();

  printf("\n[StateMQ] state change\n");
  printf("  prev        : %u (%s)\n",   (unsigned)ctx.prev,    prevName ? prevName : "");
  printf("  desired     : %u (%s)\n",   (unsigned)ctx.desired, desiredName ? desiredName : "");
  printf("  curr        : %u (%s)\n",   (unsigned)ctx.curr,    currName ? currName : "");
  printf("  cause       : %u (%s)\n",   (unsigned)ctx.cause,   causeName(ctx.cause));
  printf("  ruleIdx     : %d\n",        (int)ctx.ruleIndex);
  printf("  topic       : %s\n",        ctx.topic ? ctx.topic : "(null)");
  printf("  payload     : %s\n",        ctx.payload ? ctx.payload : "(null)");
  printf("  user ptr    : %p\n",        ctx.user);

  if (demo) {
    printf("  user DemoCtx: led1=%d inv=%s, led2=%d inv=%s\n",
       (int)demo->led1, demo->led1Invert ? "true" : "false",
       (int)demo->led2, demo->led2Invert ? "true" : "false");
  } else {
    printf("  user DemoCtx: (null)\n");
  }

  printf("  api.connected() : %d\n", (int)isConn);
  printf("  api.stateId()   : %u (%s)\n", (unsigned)apiId, node.stateName(apiId));
  printf("  api.state()     : %s\n", apiState ? apiState : "(null)");
  printf("  nowMs()         : %u\n", (unsigned)nowMs());
}

extern "C" void app_main(void) {
  // Create demo ctx with static lifetime
  static DemoCtx demo{
    .led1 = LED1,
    .led2 = LED2,
    .led1Invert = LED1_INVERT,
    .led2Invert = LED2_INVERT
  };
  g_demo = &demo;

  // GPIO setup
  gpio_config_t io{};
  io.mode = GPIO_MODE_OUTPUT;
  io.pin_bit_mask = (1ULL << demo.led1) | (1ULL << demo.led2);
  gpio_config(&io);

  setLed(demo.led1, demo.led1Invert, false);
  setLed(demo.led2, demo.led2Invert, false);

  // MQTT (topic,payload) -> StateId
  RUNNING_ID = node.map(STATE_TOPIC, "run",     "RUNNING");
  IDLE_ID    = node.map(STATE_TOPIC, "stop",    "IDLE");
  PATTERN_ID = node.map(STATE_TOPIC, "pattern", "PATTERN");

  // Print full state-change context
  node.onStateChange(onStateChangeCtx, /*user=*/&demo);

  esp.StatePublishTopic("lab/node/status", /*qos=*/1, /*enable=*/true);

  // tasks
  node.taskEvery("led1", 200, Stack::Small, led1Task, true);
  node.taskEvery("led2", 200, Stack::Small, led2Task, true);

  esp.setKeepAliveSeconds(5);

  // menuconfig credentials
  const char* ssid   = CONFIG_STATEMQ_WIFI_SSID;
  const char* pass   = CONFIG_STATEMQ_WIFI_PASS;
  const char* broker = CONFIG_STATEMQ_BROKER_URI;

  // Start Wi-Fi and MQTT
  esp.begin(ssid, pass, broker);
}
