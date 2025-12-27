// StateMQ_ESP.h
#pragma once

#include <cstddef>
#include <cstdint>

#include "StateMQ.h"

extern "C" {
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace statemq {

class StateMQEsp {
public:
  explicit StateMQEsp(StateMQ& core);
  ~StateMQEsp();

  void setKeepAliveSeconds(uint16_t sec);

  void setDefaultSubscribeQos(int qos);

  void setLastWill(const char* topic,
                   const char* payload,
                   int qos = 1,
                   bool retain = true);
  void clearLastWill();

  void setDefaultPublishQos(int qos);

  bool subscribe(const char* topic, int qos = 0);
  const char* msg(const char* topic);

  bool begin(const char* wifi_ssid,
             const char* wifi_pass,
             const char* broker_uri
             );

  void end(bool disconnect_wifi = false);

  bool publish(const char* topic,
               const char* payload,
               int qos = -1,
               bool retain = false);

  bool connected() const;

  bool taskEnable(StateMQ::TaskId id, bool enable);

  // enable state publish topic in one call
  void StatePublishTopic(const char* topic, int qos = -1, bool enable = true, bool retain = true);


private:
  static char* dupstr(const char* s);
  static void  freestr(char*& s);

  void cleanup(bool disconnect_wifi, bool clear_config);
  void freeQosOverrides();

  int  qosForTopic(const char* topic) const;
  void subscribeAllUnique();

  static constexpr size_t MAX_RAW_SUBS    = 16;
  static constexpr size_t RAW_TOPIC_LEN   = 96;
  static constexpr size_t RAW_PAYLOAD_LEN = 256;

  struct RawSlot {
    char topic[RAW_TOPIC_LEN];
    char payload[RAW_PAYLOAD_LEN];
    bool hasNew;
  };

  int rawIndex(const char* topic) const;

  struct UserTaskCtx {
    void (*cb)();
    void (*cbEx)(void*);
    void* user;
    uint32_t period_ms;
  };

  static void user_task_trampoline(void* arg);

  static void wifi_event_handler(void* arg,
                                 esp_event_base_t base,
                                 int32_t id,
                                 void* data);

  static void mqtt_event_handler(void* arg,
                                 esp_event_base_t base,
                                 int32_t id,
                                 void* data);

  void onWifiGotIp();
  void onWifiDisconnected();

  void onMqttConnected();
  void onMqttDisconnected();
  void onMqttData(esp_mqtt_event_handle_t e);

  void startMqttIfNeeded();
  void stopMqtt();

private:
  StateMQ& core;

  char* wifiSsid  = nullptr;
  char* wifiPass  = nullptr;
  char* brokerUri = nullptr;

  char* stateTopic = nullptr;
  int   statePubQos = -1;
  bool  statePubEnabled = false;
  bool  statePubRetain = true;


  esp_mqtt_client_handle_t client = nullptr;

  volatile bool wifiHasIp = false;
  volatile bool mqttConnected = false;

  uint16_t keepAliveSec = 30;
  int defaultSubQos = 1;
  int defaultPubQos = 1;
  bool retainState  = true;


  bool  lwtEnabled  = false;
  char* willTopic   = nullptr;
  char* willPayload = nullptr;
  int8_t willQos    = 1;
  bool  willRetain  = true;

  static constexpr size_t MAX_QOS_OVERRIDES = 16;
  char*  qosTopic[MAX_QOS_OVERRIDES]{};
  int8_t qosValue[MAX_QOS_OVERRIDES]{};
  size_t qosCount = 0;

 void** taskHandles = nullptr;
  UserTaskCtx** taskCtxs = nullptr;    
  size_t taskHandlesCount = 0;


  RawSlot raw[MAX_RAW_SUBS]{};
  size_t rawCount = 0;

  StateMQ::StateId lastStatePub = StateMQ::OFFLINE_ID;
  bool hasLastStatePub = false;

  static void on_state_change_trampoline(const StateMQ::StateChangeCtx& ctx);

};

} // namespace statemq
