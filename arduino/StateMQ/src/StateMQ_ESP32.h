// StateMQ_ESP32.h
#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "StateMQ.h"
#include "mqtt_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#ifndef STATEMQ_TASK_PRIORITY_USER
#define STATEMQ_TASK_PRIORITY_USER 1
#endif

using StateMQ = statemq::StateMQ;

// Stack aliases
static constexpr statemq::Stack small  = statemq::Stack::Small;
static constexpr statemq::Stack medium = statemq::Stack::Medium;
static constexpr statemq::Stack large  = statemq::Stack::Large;

// State aliases
static constexpr const char* OFFLINE = statemq::StateMQ::OFFLINE_STATE;

// StateId aliases
using StateId = statemq::StateMQ::StateId;
static constexpr StateId OFFLINE_ID   = statemq::StateMQ::OFFLINE_ID;
static constexpr StateId CONNECTED_ID = statemq::StateMQ::CONNECTED_ID;

class StateMQEsp32 {
public:
  explicit StateMQEsp32(statemq::StateMQ& core);
  ~StateMQEsp32();

  void setKeepAliveSeconds(uint16_t sec);

  void setDefaultSubscribeQos(int qos);
  void setSubscribeQos(const char* topic, int qos);

  bool begin(const char* wifi_ssid,
             const char* wifi_pass,
             const char* broker_uri);

  void end(bool disconnect_wifi = false);

  bool subscribe(const char* topic, int qos = 0);
  const char* msg(const char* topic);

  void StatePublishTopic(const char* topic, int qos = -1, bool enable = true, bool retain = true);

  bool publish(const char* topic,
               const char* payload,
               int qos = 0,
               bool retain = false);

  bool connected() const;

  void setLastWill(const char* topic,
                   const char* payload,
                   int qos = 1,
                   bool retain = true);
  void clearLastWill();


  // enable/disable core tasks at runtime
  bool taskEnable(statemq::StateMQ::TaskId id, bool enable);

private:
  struct UserTaskCtx {
    StateMQEsp32* owner = nullptr;

    // Core task callbacks 
    void (*fn)() = nullptr;
    void (*fnEx)(void*) = nullptr;
    void* user = nullptr;

    uint32_t period_ms = 1000;
    TaskHandle_t handle = nullptr;
    UserTaskCtx* next = nullptr;

    statemq::StateMQ::TaskId id = 0;
  };

  static void user_task_trampoline(void* arg);
  static void mqtt_event_handler_trampoline(void* handler_args,
                                            const char* base,
                                            int event_id,
                                            void* event_data);
  static void reconnect_task_trampoline(void* arg);

  static void on_state_change_trampoline(const statemq::StateMQ::StateChangeCtx& ctx);

  void reconnectLoop();
  void startReconnectTask();

  void onMqttEvent(esp_mqtt_event_handle_t event);
  void subscribeAllUnique();
  void cleanup(bool disconnect_wifi, bool clear_config);

  void silenceEspIdfNoise();
  int qosForTopic(const char* topic) const;


  static char* dupstr(const char* s);
  static void  freestr(char*& s);

  void freeQosOverrides();
  void freeUserTasks();

  bool tryLockCoreForMs(uint32_t ms);
  void unlockCore();
  void lockCoreBlocking();

  static uint32_t stackBytesFor(statemq::Stack s);

private:
  statemq::StateMQ& core;
  esp_mqtt_client_handle_t mqtt = nullptr;

  char* wifiSsid  = nullptr;
  char* wifiPass  = nullptr;
  char* brokerUri = nullptr;

  char* stateTopic = nullptr;
  int   statePubQos = -1;
  bool  statePubEnabled = false;

  statemq::StateMQ::StateId lastStatePub = statemq::StateMQ::OFFLINE_ID;
  bool hasLastStatePub = false;
  bool  statePubRetain = true;


  volatile bool mqttConnected = false;

  uint16_t keepAliveSec = 60;
  int defaultSubQos = 0;

  TaskHandle_t reconnectTask = nullptr;

  uint32_t backoffMs = 2000;
  uint32_t nextTryMs = 0;
  bool printedWifi = false;
  bool printedMqtt = false;

  UserTaskCtx* userTasks = nullptr;

  static constexpr size_t MAX_RAW_SUBS = 16;
  static constexpr size_t RAW_TOPIC_LEN = 64;
  static constexpr size_t RAW_PAYLOAD_LEN = 128;

  struct RawSlot {
    char topic[RAW_TOPIC_LEN];
    char payload[RAW_PAYLOAD_LEN];
    volatile bool hasNew;
  };

  RawSlot raw[MAX_RAW_SUBS]{};
  size_t  rawCount = 0;

  int rawIndex(const char* topic) const;

  char*  willTopic   = nullptr;
  char*  willPayload = nullptr;
  int8_t willQos     = 1;
  bool   willRetain  = true;

  static constexpr size_t MAX_QOS_OVERRIDES = 16;
  char*  qosTopic[MAX_QOS_OVERRIDES]{};
  int8_t qosValue[MAX_QOS_OVERRIDES]{};
  size_t qosCount = 0;
};
