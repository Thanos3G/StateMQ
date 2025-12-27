// Simple MQTT example (superloop).
//
// * Connects to WiFi and an MQTT broker
// * Listens for state messages on a topic
// * Changes internal state based on received messages
// * Sends a reply whenever the state changes

#include <Arduino.h>
#include <StateMQ_ESP32.h>

using StateId = StateMQ::StateId;

// WiFi credentials
const char* WIFI_SSID = "your_wifi_ssid";
const char* WIFI_PASS = "your_wifi_password";

// MQTT broker address
const char* MQTT_BROKER = "mqtt://broker_ip:1883";

// ---------------- topics ----------------
// Incoming control messages that select the device "State".
const char* STATE_TOPIC  = "mqtt/state";

// Outgoing informational status messages (retained).
const char* STATUS_TOPIC = "mqtt/status";

// LWT topic published by broker when device disappears.
const char* LWT_TOPIC  = "mqtt/will";

// Optional raw subscription (not part of the state rule table).
const char* CHAT_TOPIC   = "mqtt/chat";

// ---------------- node ----------------
StateMQ node;
StateMQEsp32 esp(node);

static StateId HELLO_ID = CONNECTED_ID;
static StateId BYE_ID   = CONNECTED_ID;

void setup() {
  Serial.begin(115200);

  // MQTT (topic,payload) -> StateId
  // Publish to mqtt/state with payload:
  //   "hi"      -> HELLO
  //   "goodbye" -> BYE
  HELLO_ID = node.map(STATE_TOPIC, "hi",      "HELLO");
  BYE_ID   = node.map(STATE_TOPIC, "goodbye", "BYE");


  // Keep alive interval (seconds)
  esp.setKeepAliveSeconds(5);

  // Raw subscription (not part of the state rule table).
  esp.subscribe(CHAT_TOPIC, /*qos=*/0);

  //Publish State Change
  esp.StatePublishTopic("lab/node/status", /*qos=*/1, /*enable=*/true, /*retain=*/true);


   //Set Last Will Message
  esp.setLastWill(LWT_TOPIC, "offline", 2, true);

  // Start Wi-Fi and MQTT
  esp.begin(WIFI_SSID, WIFI_PASS, MQTT_BROKER);
}

void loop() {
  // Optional raw subscription (not part of the state rule table).
  const char* m = esp.msg(CHAT_TOPIC);
  if (m) Serial.println(m);

  // Compares the state with the previous
  static StateId last = OFFLINE_ID;
  StateId now = node.stateId();

  if (last == now) return;
  last = now;

  // Publish based on state
  if (now == HELLO_ID) {
    esp.publish(STATUS_TOPIC, "hi back", /*qos=*/1, /*retain=*/true);
  }
  else if (now == BYE_ID) {
    esp.publish(STATUS_TOPIC, "see you", /*qos=*/1, /*retain=*/true);
  }
}
