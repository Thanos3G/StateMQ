#include <Arduino.h>
#include <StateMQ_ESP32.h>

// WiFi credentials
const char* WIFI_SSID = "your_wifi_ssid";
const char* WIFI_PASS = "your_wifi_password";

// MQTT broker address
const char* MQTT_BROKER = "mqtt://broker_ip:1883";

// MQTT topic used to control the state
const char* STATE_TOPIC = "blocking/state";

// LED pin 
const int LED_PIN = 21;

// Create the core state machine
StateMQ node;

// Configure the ESP32 MQTT node
StateMQEsp32 esp(node);

using StateId = StateMQ::StateId;
static StateId ALIVE_ID = CONNECTED_ID;

// Task 1: blocking task (intentionally bad)
void blockingTask(void*) {
  // Infinite blocking loop
  while (true) {
    // blink LED forever 
    digitalWrite(LED_PIN, HIGH);
    delay(200);  
    digitalWrite(LED_PIN, LOW);
    delay(200);   
  }
}

// Task 2: runs normally
void aliveTask() {
  static unsigned long last = 0;
  if (millis() - last < 1000) return;
  last = millis();

  // Prints the current state
  // Falls back to CONNECTED and OFFLINE depending on broker status
  StateId s = node.stateId();

  if (s == ALIVE_ID) {
    Serial.println("ALIVE");
  }
  else if (s == OFFLINE_ID) {
    Serial.println("OFFLINE");
  }
  else if (s == CONNECTED_ID) {
    Serial.println("CONNECTED");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  // Map MQTT messages to internal states
  ALIVE_ID = node.map(STATE_TOPIC, "alive", "ALIVE");

  // Create tasks:
  // taskEvery(
  //   "name",        // task name
  //   200,           // how often it runs (milliseconds)
  //   small,         // stack size (≈2 KB, medium ≈4 KB, large ≈8 KB)
  //   function,      // task function
  //   true           // task enabled
  // )
  node.taskEvery("alive", 200, small, aliveTask, true);

  // Create an explicit FreeRTOS task at LOWER priority 
  xTaskCreate(
    blockingTask,
    "block",
    2048 / sizeof(StackType_t),
    nullptr,
    0,          // lower priority 
    nullptr
  );

  // Connect to WiFi and MQTT
  esp.begin(WIFI_SSID, WIFI_PASS, MQTT_BROKER);
}

void loop() {
  // No superloop, everything handled by tasks
}
