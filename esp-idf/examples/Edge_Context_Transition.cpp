// main/app_main.cpp
//
// StateMQ ESP-IDF example: HELLO / BYE with edge-triggered transitions.
//
// MQTT interface:
// - Publish to:   hello/state
//   Payloads:     "hi", "bye"
//
// Behavior:
// - Periodic tasks react to the current state (level)
// - onStateChange() reacts to transitions (edge)

#include <cstring>

#include "sdkconfig.h"
#include "StateMQ_ESP.h"

#include "driver/gpio.h"
#include "esp_timer.h"

using namespace statemq;

// ---------------- pins ----------------
static constexpr gpio_num_t LED_PIN = GPIO_NUM_21;

// ---------------- topics ----------------
static constexpr const char* STATE_TOPIC  = "hello/state";

// ---------------- node ----------------
static StateMQ node;
static StateMQEsp esp(node);

using StateId = StateMQ::StateId;
static StateId HELLO_ID = StateMQ::CONNECTED_ID;
static StateId BYE_ID   = StateMQ::CONNECTED_ID;

// ---------------- helpers ----------------
static inline uint32_t nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static inline void ledWrite(bool on) {
  gpio_set_level(LED_PIN, on ? 1 : 0);
}

// ---------------- tasks ----------------

// Task 1: print message based on state
static void printTask() {
  StateId s = node.stateId();

  if (s == HELLO_ID) {
    printf("Hello world\n");
  }
  else if (s == BYE_ID) {
    printf("Bye world\n");
  }
  else if (s == StateMQ::OFFLINE_ID) {
    printf("Offline\n");
  }
  else if (s == StateMQ::CONNECTED_ID) {
    printf("Connected\n");
  }
}
// Task 2: control LED based on state
static void ledTask() {
  StateId s = node.stateId();

  if (s == HELLO_ID) {
    ledWrite(true);
  }
  else if (s == BYE_ID) {
    ledWrite(false);
  }
  else if (s == StateMQ::OFFLINE_ID) {
    // blink when offline
    ledWrite(((nowMs() / 500) % 2) != 0);
  }
}

// ---------------- EDGE CALLBACK ----------------
//
// exactly once per transition
static void onEdge(const StateMQ::StateChangeCtx& ctx) {
  printf("[edge] %s -> %s (cause=%u)\n",
         node.stateName(ctx.prev),
         node.stateName(ctx.curr),
         (unsigned)ctx.cause);

  // Enter HELLO
  if (ctx.curr == HELLO_ID && ctx.prev != HELLO_ID) {
    printf("[edge] Entered HELLO (one-shot)\n");
  }

  // HELLO -> BYE
  if (ctx.prev == HELLO_ID && ctx.curr == BYE_ID) {
    printf("[edge] HELLO -> BYE (one-shot)\n");
  }

  // Leaving OFFLINE
  if (ctx.prev == StateMQ::OFFLINE_ID &&
      ctx.curr != StateMQ::OFFLINE_ID) {
    printf("[edge] Device came online\n");
  }
}

extern "C" void app_main(void) {
  // GPIO setup
  gpio_config_t io{};
  io.mode = GPIO_MODE_OUTPUT;
  io.pin_bit_mask = (1ULL << LED_PIN);
  gpio_config(&io);
  ledWrite(false);

  // MQTT (topic,payload) -> StateId
  HELLO_ID = node.map(STATE_TOPIC, "hi",  "HELLO");
  BYE_ID   = node.map(STATE_TOPIC, "bye", "BYE");

  // tasks
  node.taskEvery("print", 500, Stack::Small, printTask, true);
  node.taskEvery("led",   100, Stack::Small, ledTask, true);

  // edge call
  node.onStateChange(onEdge);

  // Subscribe to state topic with specific QoS
  esp.StatePublishTopic("hello/status", /*qos=*/1, /*retain*/true, /*enable=*/true);


  // Subscribe to state topic with specific QoS
  esp.subscribe(STATE_TOPIC, /*qos=*/1);

  // menuconfig credentials
  const char* ssid   = CONFIG_STATEMQ_WIFI_SSID;
  const char* pass   = CONFIG_STATEMQ_WIFI_PASS;
  const char* broker = CONFIG_STATEMQ_BROKER_URI;

  // Start Wi-Fi and MQTT
  esp.begin(ssid, pass, broker);
}
