#include <Arduino.h>
#include <StateMQ_ESP32.h>

const char* WIFI_SSID   = "your_wifi_ssid";
const char* WIFI_PASS   = "your_wifi_password";
const char* MQTT_BROKER = "mqtt://broker_ip:1883";
const char* STATE_TOPIC = "hello/topic";

StateMQ node;
StateMQEsp32 esp(node);

StateId HELLO_ID;
StateId BYE_ID;

void printTask() {
  StateId st = node.stateId();

  if (st == HELLO_ID)      Serial.println("Hello world");
  else if (st == BYE_ID)   Serial.println("Bye world");
  else if (st == OFFLINE_ID)   Serial.println("(offline)");
  else if (st == CONNECTED_ID) Serial.println("(connected)");
}

// edge transition
static void onEdge(const statemq::StateMQ::StateChangeCtx& ctx) {
  Serial.print("[edge] ");
  Serial.print(node.stateName(ctx.prev));
  Serial.print(" -> ");
  Serial.println(node.stateName(ctx.curr));

  // edge rule
  if (ctx.curr == HELLO_ID && ctx.prev != HELLO_ID) {
    Serial.println("Entered HELLO (one-shot)");
  }

  // edge rule
  if (ctx.prev == HELLO_ID && ctx.curr == BYE_ID) {
    Serial.println("HELLO -> BYE transition (one-shot)");
  }
}

void setup() {
  Serial.begin(115200);

  HELLO_ID = node.map(STATE_TOPIC, "hi",  "HELLO");
  BYE_ID   = node.map(STATE_TOPIC, "bye", "BYE");

  node.taskEvery("print", 1000, small, printTask, true);
  // edge call
  node.onStateChange(&onEdge, nullptr);
  esp.begin(WIFI_SSID, WIFI_PASS, MQTT_BROKER);
}

void loop() {

}
