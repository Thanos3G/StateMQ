# StateMQ — Event-Driven, State-Based MQTT (ESP-IDF + Arduino wrapper)

StateMQ is a small, deterministic, state-machine–driven framework for MQTT-connected embedded systems.
It provides a readable abstraction for message-driven systems by expressing application
behavior through explicit states and periodic tasks, rather than callback-driven logic.

Current platform implementations use ESP-IDF’s `esp-mqtt` and FreeRTOS, while the core
logic remains platform-agnostic and shared between native ESP-IDF builds and an
Arduino library.



## Motivation and Core Idea

Through professional and academic work, I repeatedly encountered MQTT-controlled embedded systems whose behavior was inherently stateful.
In many projects, MQTT messages directly drove application logic, which over time led to scattered callbacks and tangled control flow.
This pattern made systems increasingly difficult to extend, reason about, and maintain safely.

StateMQ’s approach:

- Treat MQTT messages as events, not commands.
- Events trigger explicit state changes.
- The system is always in exactly one known state.
- States are represented internally by small integer identifiers.
- Application logic reacts to state changes rather than raw messages.
- Periodic work runs independently as scheduled tasks.

Execution model:

- MQTT messages are processed sequentially.
- State changes are resolved using a fixed, table-driven `(topic, payload) → state` mapping.
- Execution remains deterministic and predictable.

Originally developed for ESP32, the core abstraction is platform-agnostic and applicable to other state-driven, message-based embedded systems.


## Installation

StateMQ is split into a platform-agnostic core and platform-specific integrations.

### ESP-IDF

The repository includes a ready-to-build ESP-IDF project.
1. Clone the repository and enter the ESP-IDF project directory:

   ```bash
   git clone https://github.com/Thanos3G/StateMQ.git
   cd StateMQ/esp-idf
   ```
2. Configure Wi-Fi and MQTT settings using menuconfig:
    ```bash
    idf.py set-target esp32
    idf.py menuconfig
    ```
    Navigate to:
    ```bash
    StateMQ Node
    ```
    And set:

    - WiFi SSID

    - WiFi password

    - MQTT broker URI

3. Build and flash:
    ```bash
    idf.py build 
    idf.py -p DEVICE_PORT flash monitor 
    ```


### Arduino-ESP32

1. Clone this repository.
2. Copy the Arduino library folder into your Arduino libraries directory:

   ```text
   arduino/StateMQ  →  ~/Arduino/libraries/StateMQ
    ```
3. Restart the Arduino IDE.
4. Include the library in your sketch:
   ```text
    #include <StateMQ.h>
   ```
The Arduino integration uses ESP-IDF’s esp-mqtt internally while exposing the same
StateMQ core API.

## Scope and Intent

StateMQ provides a small, explicit layer for expressing system behavior
through states and simple periodic tasks, while keeping networking,
execution flow, and state evolution visible and predictable.

It intentionally avoids the overhead of hierarchical state machines for
small embedded systems, and uses FreeRTOS tasks only for fixed-period
work, not for control flow.

The platform integrations expose the full MQTT feature set, including QoS
levels, last-will messages, retained publications, and flexible topic
subscription and publication beyond the state mapping.

## Typical Use Cases

- Resource-constrained MQTT devices with defined operational states
- Systems where behavior must remain predictable as features grow
- Devices requiring concurrent periodic work (telemetry, monitoring, control)
- Projects where callback-driven logic has become difficult to handle


## Architecture Overview

```bash
                 External Events
                 (MQTT messages)
                        |
                        v
     +------------------------------------------+
     |        StateMQ + Platform Layer           |
     |-------------------------------------------|
     | - network and MQTT lifecycle              |
     | - event delivery                          |
     | - state rules and changes                 |
     | - known state tracking                    |
     | - periodic task scheduling                |
     +------------------------------------------+
                         |
                         v
                    state change
                         |
        +----------------+-------------------+
        |                |                   |
        v                v                   v
+----------------+ +----------------+ +----------------+
|  Task A        | |  Task B        | |  Task C        |
| (periodic)     | | (periodic)     | | (periodic)     |
+----------------+ +----------------+ +----------------+
        |                |                   |
        v                v                   v
                (run independently)
```
## Core API

### State Changes

StateMQ processes MQTT messages one at a time and resolves state changes
using a simple rule table that maps incoming messages to integer state identifiers.
State changes are serialized using a mutex to ensure thread-safe, deterministic updates.

When an MQTT message arrives, StateMQ checks each configured rule in the
order it was added. If a rule matches the message topic and payload, the
system jumps to the corresponding state. If no rule matches, the
current state remains unchanged.

```text
onMessage(topic, payload):
    for each rule in rules (in insertion order):
        if rule.topic == topic AND rule.payload == payload:
            currentStateId = rule.stateId
            return

    // no match → state remains unchanged
```
In systems with many rules or very high message rates, the same behavior
could be implemented using an indexed lookup instead of a linear scan.



### Message to State Mapping

State changes are defined declaratively by mapping incoming MQTT messages to states:


```cpp

StateMQ node;

//                        topic     payload  state
auto ON_ID    = node.map("node/cmd", "RUN", "ON");
auto OFF_ID   = node.map("node/cmd", "STP", "OFF");
auto RESET_ID = node.map("node/cmd", "RST", "RESET");

```
### Periodic Tasks

Tasks are declared in the core and executed as FreeRTOS tasks by the platform wrapper.

```cpp
node.taskEvery(
    "heartbeat",           // task name
    1000,                  // period (ms)
    small,                 // stack size preset (small=2048, medium=4096, large=8192)
    heartbeatTask,         // callback
    true                   // enabled
);
```

Tasks can be enabled or disabled at runtime.

### Subscriptions and Publishing

The platform wrappers expose basic MQTT publishing and subscription 
configuration for telemetry, logs, and auxiliary topics.

```cpp
// Publish arbitrary MQTT messages
esp.publish("node/log", "booted", /*qos=*/1, /*retain=*/false);

// Configure global and per-topic QoS
esp.setDefaultSubscribeQos(0);
esp.setSubscribeQos("node/topic", 2);

// Configure MQTT Last-Will message
esp.setLastWill("node/status", "offline", /*qos=*/1, /*retain=*/true);
```

Complete examples demonstrating message-to-state mappings are provided
in the Arduino and ESP-IDF example projects included in this repository.


## Platform Support

**Currently supported platforms:**

- **ESP-IDF**  
  Native Wi-Fi and `esp-mqtt`, event-driven startup, FreeRTOS-based task execution.

- **Arduino-ESP32**  
  Same StateMQ core, Arduino WiFi integration, and FreeRTOS task execution under the Arduino runtime.  
  Wi-Fi connection and readiness are handled using lightweight polling during startup due to Arduino runtime constraints, while MQTT messaging and state changes remain event-driven and deterministic.
  

The core abstraction is designed to be portable and may be adapted to
other platforms in the future, particularly environments that provide
FreeRTOS or a similar tasking model, including Arduino environments
using Arduino_FreeRTOS. Traditional AVR-based Arduino platforms are unlikely to be a good fit due to the lack of native
concurrency and MQTT library support.

## Design Constraints

StateMQ is designed for small, resource-constrained embedded systems.
To keep behavior predictable and memory usage explicit, the core (StateMQ.h) uses
fixed-size limits for the number of states, rules, tasks, and state names.

These limits are intentional, enforced by compile-time constants, and
documented in the core headers where they can be adjusted if needed.

These constraints also exist to preserve deterministic execution.
StateMQ avoids unbounded queues, dynamic task creation, and runtime
modification of state changing logic, ensuring that system behavior remains
repeatable under identical inputs. 


## Status

StateMQ is a stable, reusable core for state-driven, MQTT-connected
embedded systems, maintained as a reference implementation.


## License

MIT License

## Arduino Example


```cpp
#include <Arduino.h>
#include <StateMQ_ESP32.h>

const char* WIFI_SSID   = "your_ssid";
const char* WIFI_PASS   = "your_pass";
const char* MQTT_BROKER = "mqtt://192.168.1.10:1883";
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
  //Publish hi to hello/topic
  HELLO_ID = node.map(STATE_TOPIC, "hi", "HELLO");
  BYE_ID   = node.map(STATE_TOPIC, "bye",   "BYE");

  node.taskEvery("print", 1000, small, printTask, true);

  esp.begin(WIFI_SSID, WIFI_PASS, MQTT_BROKER);
}

void loop() {}
```
