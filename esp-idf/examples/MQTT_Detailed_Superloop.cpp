// Simple MQTT example (superloop).
//
// * Connects to WiFi and an MQTT broker
// * Listens for commands on a topic
// * Changes internal state based on received messages
// * Sends a reply whenever the state changes

#include <cstring>

#include "sdkconfig.h"
#include "StateMQ_ESP.h"

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

using namespace statemq;

// MQTT Topics
static constexpr const char* STATE_TOPIC  = "mqtt/state";
static constexpr const char* STATUS_TOPIC = "mqtt/status";
static constexpr const char* WILL_TOPIC   = "mqtt/will";
static constexpr const char* CHAT_TOPIC   = "hello/chat";

static StateMQ node;
static StateMQEsp esp(node);

using StateId = StateMQ::StateId;
static StateId HELLO_ID = StateMQ::CONNECTED_ID;
static StateId BYE_ID   = StateMQ::CONNECTED_ID;

extern "C" void app_main(void) {

  // Set states based on message
  HELLO_ID = node.map(STATE_TOPIC, "hi",      "HELLO");
  BYE_ID   = node.map(STATE_TOPIC, "goodbye", "BYE");

  // Subscribe QoS for mapped states
  esp.setDefaultSubscribeQos(1);

  //MQTT Keep_Alive
  esp.setKeepAliveSeconds(5);

  //Umapped Subscribe
  esp.subscribe(CHAT_TOPIC, 1);

  // Last Will Message
  esp.setLastWill(WILL_TOPIC, "offline", 1, false);

  // menuconfig credentials
  const char* ssid   = CONFIG_STATEMQ_WIFI_SSID;
  const char* pass   = CONFIG_STATEMQ_WIFI_PASS;
  const char* broker = CONFIG_STATEMQ_BROKER_URI;

  // Start Wi-Fi and MQTT (state topic stays in main)
  esp.begin(
    ssid,
    pass,
    broker
  );

  //Compares the state with the previous
  StateId last = StateMQ::OFFLINE_ID;

  while (true) {
    // Read raw subscribed messages
    const char* msg = esp.msg(CHAT_TOPIC);
    if (msg) {
      printf("%s\n", msg);
    }

    StateId now = node.stateId();
    if (now == last) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    last = now;

    // Publish based on state
    if (now == HELLO_ID) {
      esp.publish(STATUS_TOPIC, "hi back", 1, true);
    }
    else if (now == BYE_ID) {
      esp.publish(STATUS_TOPIC, "see you", 1, true);
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }

}
