// example.ino
//
// StateMQ Arduino example: simple HELLO / BYE state control.
//
// MQTT interface (what to publish / subscribe):
// - Publish to:   hello/state
//   Messages:     "hi", "bye"
//   Effect:       changes internal StateMQ state (integer StateId)
//
// Notes:
// - Demonstrates basic message-to-state mapping.
// - Periodic tasks react to the current state.
//

#include <Arduino.h>
#include <StateMQ_ESP32.h>

// WiFi credentials
const char* WIFI_SSID = "your_wifi_ssid";
const char* WIFI_PASS = "your_wifi_password";

// MQTT broker address
const char* MQTT_BROKER = "mqtt://broker_ip:1883";

// ---------------- pins ----------------
const int LED_PIN = 21;

// ---------------- topics ----------------
// Incoming control messages that select the device "State".
const char* STATE_TOPIC = "hello/state";

// ---------------- node ----------------
// Create the core state machine
StateMQ node;

// Configure the ESP32 MQTT node
StateMQEsp32 esp(node);

StateId HELLO_ID = CONNECTED_ID;
StateId BYE_ID   = CONNECTED_ID;

// ---------------- tasks ----------------

// Task 1: print message based on state
void printTask() {
  StateId s = node.stateId();

  if (s == HELLO_ID) {
    Serial.println("Hello world");
  }
  else if (s == BYE_ID) {
    Serial.println("Bye world");
  }
  else if (s == OFFLINE_ID) {
    Serial.println("Offline");
  }
}

// Task 2: control LED based on state
void ledTask() {
  StateId s = node.stateId();

  if (s == HELLO_ID) {
    digitalWrite(LED_PIN, HIGH);
  }
  else if (s == BYE_ID) {
    digitalWrite(LED_PIN, LOW);
  }
  else if (s == OFFLINE_ID) {
    // blink when offline
    digitalWrite(LED_PIN, (millis() / 500) % 2);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  // MQTT (topic,payload) -> StateId
  // Publish to hello/state with payload:
  //   "hi"  -> HELLO
  //   "bye" -> BYE
  HELLO_ID = node.map(STATE_TOPIC, "hi",  "HELLO");
  BYE_ID   = node.map(STATE_TOPIC, "bye", "BYE");

  // Create tasks:
  // taskEvery(
  //   "name",        // task name
  //   500,           // how often it runs (milliseconds)
  //   small,         // stack size (small ≈2 KB, medium ≈4 KB, large ≈8 KB)
  //   function,      // task function
  //   true           // task enabled
  // )
  node.taskEvery("print", 500, small, printTask, true);
  node.taskEvery("led",   100, small, ledTask, true);

  // Set QoS to subscribr topic explicitly
  esp.subscribe(STATE_TOPIC, /*qos=*/1);

  // Start Wi-Fi and MQTT
  esp.begin(WIFI_SSID, WIFI_PASS, MQTT_BROKER);
}

void loop() {
  // No superloop, everything handled by tasks
}
