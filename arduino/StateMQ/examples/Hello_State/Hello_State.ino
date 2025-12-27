#include <Arduino.h>
#include <StateMQ_ESP32.h>

const char* WIFI_SSID = "your_wifi_ssid";
const char* WIFI_PASS = "your_wifi_password";
const char* MQTT_BROKER = "mqtt://broker_ip:1883";
const char* STATE_TOPIC = "hello/topic";

StateMQ node;
StateMQEsp32 esp(node);

StateId HELLO_ID;
StateId BYE_ID;

void printTask() {
  auto st = node.stateId();

  if (st == HELLO_ID) Serial.println("Hello world");
  else if (st == BYE_ID) Serial.println("Bye world");
}

void setup() {
  Serial.begin(115200);


  HELLO_ID = node.map(STATE_TOPIC, "hi", "HELLO");
  BYE_ID   = node.map(STATE_TOPIC, "bye",   "BYE");

  node.taskEvery("print", 1000, small, printTask, true);

  esp.begin(WIFI_SSID, WIFI_PASS, MQTT_BROKER);
}

void loop() {}
