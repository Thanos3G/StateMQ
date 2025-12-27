// main/app_main.cpp
//
// StateMQ ESP-IDF example: blocking task isolation.
//
// MQTT interface (what to publish / subscribe):
// - Publish to:   blocking/state
//   Messages:     "alive"
//   Effect:       changes internal StateMQ state 
//
// Notes:
// - Demonstrates coexistence of StateMQ-managed tasks with a blocking FreeRTOS task.
// - StateMQ tasks continue to run deterministically.
//

#include <cstring>
#include <cstdio>
#include <string>

#include "sdkconfig.h"
#include "StateMQ_ESP.h"

#include "driver/gpio.h"
#include "esp_timer.h"

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

using namespace statemq;
using namespace std;

// ---------------- pins ----------------
static constexpr gpio_num_t LED_PIN = GPIO_NUM_21;

// ---------------- topics ----------------
// Incoming control messages that select the device "State".
static constexpr const char* STATE_TOPIC = "blocking/state";

// ---------------- node ----------------
static StateMQ node;
static StateMQEsp esp(node);

using StateId = StateMQ::StateId;
static StateId ALIVE_ID = StateMQ::CONNECTED_ID;

// ---------------- helpers ----------------
static inline void ledWrite(bool on) {
  gpio_set_level(LED_PIN, on ? 1 : 0);
}

static uint32_t nowMs() {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ---------------- FreeRTOS task ----------------
// Blocking task (intentionally bad).
// Runs forever and blocks its own time slice.
static void blockingTask(void*) {
  while (true) {
    ledWrite(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    ledWrite(false);
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ---------------- StateMQ task ----------------
// Prints the current logical state once per second.
static void aliveTask() {
  static uint32_t last = 0;
  uint32_t now = nowMs();
  if (now - last < 1000) return;
  last = now;

  printf("%s\n", node.state());
}

extern "C" void app_main(void) {
  // GPIO setup
  gpio_config_t io{};
  io.mode = GPIO_MODE_OUTPUT;
  io.pin_bit_mask = (1ULL << LED_PIN);
  gpio_config(&io);
  ledWrite(false);

  // MQTT (topic,payload) -> StateId
  // Publish to blocking/state with payload:
  //   "alive" -> ALIVE
  ALIVE_ID = node.map(STATE_TOPIC, "alive", "ALIVE");

  // tasks
  node.taskEvery("alive", 200, Stack::Small, aliveTask, true);

  // Explicit FreeRTOS task at lower priority than StateMQ tasks
  xTaskCreate(
    blockingTask,
    "block",
    2048 / sizeof(StackType_t),
    nullptr,
    0, //priority 0, StateMQ default is 1
    nullptr
  );

  // menuconfig credentials
  const char* ssid   = CONFIG_STATEMQ_WIFI_SSID;
  const char* pass   = CONFIG_STATEMQ_WIFI_PASS;
  const char* broker = CONFIG_STATEMQ_BROKER_URI;

  // Start Wi-Fi and MQTT (state topic stays in main)
  esp.begin(ssid, pass, broker);
}
