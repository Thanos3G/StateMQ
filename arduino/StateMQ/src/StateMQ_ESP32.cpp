// StateMQ_ESP32.cpp
#include "StateMQ_ESP32.h"

#include <new>
#include <cstring>

#include "esp_log.h"

// Stack sizing
uint32_t StateMQEsp32::stackBytesFor(statemq::Stack s) {
  switch (s) {
    case statemq::Stack::Small:  return 2048;
    case statemq::Stack::Medium: return 4096;
    case statemq::Stack::Large:  return 8192;
    default:                     return 2048;
  }
}

char* StateMQEsp32::dupstr(const char* s) {
  if (!s) return nullptr;
  const size_t n = strlen(s);
  char* out = new (std::nothrow) char[n + 1];
  if (!out) return nullptr;
  memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

void StateMQEsp32::freestr(char*& s) {
  if (s) {
    delete[] s;
    s = nullptr;
  }
}

// core locking (recursive mutex)
bool StateMQEsp32::tryLockCoreForMs(uint32_t ms) {
  SemaphoreHandle_t m = core.mutexHandle();
  if (!m) return true;
  return xSemaphoreTakeRecursive(m, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void StateMQEsp32::lockCoreBlocking() {
  SemaphoreHandle_t m = core.mutexHandle();
  if (!m) return;
  xSemaphoreTakeRecursive(m, portMAX_DELAY);
}

void StateMQEsp32::unlockCore() {
  SemaphoreHandle_t m = core.mutexHandle();
  if (!m) return;
  xSemaphoreGiveRecursive(m);
}

// Construction / destruction
StateMQEsp32::StateMQEsp32(statemq::StateMQ& core)
: core(core) {}

StateMQEsp32::~StateMQEsp32() {
  end(false);
}

// settings
void StateMQEsp32::setKeepAliveSeconds(uint16_t sec) {
  if (sec == 0) sec = 60;
  keepAliveSec = sec;
}

void StateMQEsp32::setDefaultSubscribeQos(int qos) {
  if (qos < 0) qos = 0;
  if (qos > 2) qos = 2;
  defaultSubQos = qos;
}

void StateMQEsp32::freeQosOverrides() {
  for (size_t i = 0; i < qosCount; ++i) {
    freestr(qosTopic[i]);
    qosTopic[i] = nullptr;
  }
  qosCount = 0;
}

void StateMQEsp32::setSubscribeQos(const char* topic, int qos) {
  if (!topic) return;

  if (qos < 0) qos = 0;
  if (qos > 2) qos = 2;

  for (size_t i = 0; i < qosCount; ++i) {
    if (qosTopic[i] && strcmp(qosTopic[i], topic) == 0) {
      qosValue[i] = (int8_t)qos;
      return;
    }
  }

  if (qosCount < MAX_QOS_OVERRIDES) {
    char* copy = dupstr(topic);
    if (!copy) return;
    qosTopic[qosCount] = copy;
    qosValue[qosCount] = (int8_t)qos;
    qosCount++;
  }
}

int StateMQEsp32::qosForTopic(const char* topic) const {
  if (!topic) return defaultSubQos;

  for (size_t i = 0; i < qosCount; ++i) {
    if (qosTopic[i] && strcmp(qosTopic[i], topic) == 0) {
      return qosValue[i];
    }
  }
  return defaultSubQos;
}

void StateMQEsp32::setLastWill(const char* topic,
                               const char* payload,
                               int qos,
                               bool retain) {
  if (!topic || !payload) return;

  if (qos < 0) qos = 0;
  if (qos > 2) qos = 2;

  freestr(willTopic);
  freestr(willPayload);

  willTopic   = dupstr(topic);
  willPayload = dupstr(payload);

  if (!willTopic || !willPayload) {
    freestr(willTopic);
    freestr(willPayload);
    return;
  }

  willQos = (int8_t)qos;
  willRetain = retain;
}

void StateMQEsp32::clearLastWill() {
  freestr(willTopic);
  freestr(willPayload);
}

void StateMQEsp32::silenceEspIdfNoise() {
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_WARN);
  esp_log_level_set("TRANSPORT", ESP_LOG_WARN);
  esp_log_level_set("TRANSPORT_TCP", ESP_LOG_WARN);
}

// RAW Subscribe
int StateMQEsp32::rawIndex(const char* topic) const {
  if (!topic) return -1;
  for (size_t i = 0; i < rawCount; ++i) {
    if (raw[i].topic[0] && strcmp(raw[i].topic, topic) == 0) return (int)i;
  }
  return -1;
}

bool StateMQEsp32::subscribe(const char* topic, int qos) {
  if (!topic || !*topic) return false;

  if (qos < 0) qos = 0;
  if (qos > 2) qos = 2;

  lockCoreBlocking();

  int idx = rawIndex(topic);
  if (idx < 0) {
    if (rawCount >= MAX_RAW_SUBS) {
      unlockCore();
      return false;
    }

    RawSlot& s = raw[rawCount];
    strncpy(s.topic, topic, RAW_TOPIC_LEN);
    s.topic[RAW_TOPIC_LEN - 1] = '\0';
    s.payload[0] = '\0';
    s.hasNew = false;

    rawCount++;
  }

  unlockCore();

  // remember QoS override
  setSubscribeQos(topic, qos);

  // if already connected, subscribe immediately
  if (mqtt && mqttConnected) {
    esp_mqtt_client_subscribe(mqtt, topic, qos);
  }

  return true;
}

const char* StateMQEsp32::msg(const char* topic) {
  if (!topic || !*topic) return nullptr;

  lockCoreBlocking();

  int idx = rawIndex(topic);
  if (idx < 0) {
    unlockCore();
    return nullptr;
  }

  RawSlot& s = raw[(size_t)idx];
  if (!s.hasNew) {
    unlockCore();
    return nullptr;
  }

  s.hasNew = false;
  const char* out = s.payload;

  unlockCore();
  return out;
}

// Cleanup helpers
void StateMQEsp32::freeUserTasks() {
  UserTaskCtx* cur = userTasks;
  userTasks = nullptr;

  while (cur) {
    UserTaskCtx* next = cur->next;

    if (cur->handle) {
      vTaskDelete(cur->handle);
      cur->handle = nullptr;
    }

    delete cur;
    cur = next;
  }
}

void StateMQEsp32::cleanup(bool disconnect_wifi, bool clear_config) {
  if (reconnectTask) {
    vTaskDelete(reconnectTask);
    reconnectTask = nullptr;
  }

  freeUserTasks();

  if (mqtt) {
    esp_mqtt_client_stop(mqtt);
    esp_mqtt_client_destroy(mqtt);
    mqtt = nullptr;
  }

  mqttConnected = false;

  lockCoreBlocking();
  core.setConnected(false);
  unlockCore();

  freestr(wifiSsid);
  freestr(wifiPass);
  freestr(brokerUri);
  

  if (clear_config) {
    freeQosOverrides();
    freestr(willTopic);
    freestr(willPayload);
    freestr(stateTopic);
    statePubEnabled = false;
    hasLastStatePub = false;


    rawCount = 0;
    for (size_t i = 0; i < MAX_RAW_SUBS; ++i) {
      raw[i].topic[0] = '\0';
      raw[i].payload[0] = '\0';
      raw[i].hasNew = false;
    }
  }

  backoffMs = 2000;
  nextTryMs = 0;
  printedWifi = false;
  printedMqtt = false;

  if (disconnect_wifi) {
    WiFi.disconnect(true);
  }
}

void StateMQEsp32::end(bool disconnect_wifi) {
  cleanup(disconnect_wifi, true);
}

// begin
bool StateMQEsp32::begin(const char* wifi_ssid,
                         const char* wifi_pass,
                         const char* broker_uri) {
  cleanup(false, false);

  if (!wifi_ssid || !broker_uri) return false;

  wifiSsid  = dupstr(wifi_ssid);
  wifiPass  = dupstr(wifi_pass ? wifi_pass : "");
  brokerUri = dupstr(broker_uri);

  if (!wifiSsid || !wifiPass || !brokerUri) {
    end(false);
    return false;
  }

  silenceEspIdfNoise();

  Serial.println("[WiFi] connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid, wifiPass);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    if (millis() - start > 15000) {
      Serial.println("[WiFi] connect timeout");
      lockCoreBlocking();
      core.setConnected(false);
      unlockCore();
      return false;
    }
  }

  Serial.println("[WiFi] connected");
  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.localIP());

  Serial.println("[MQTT] starting...");

  esp_mqtt_client_config_t mcfg = {};
  // Note: Arduino core uses ESP-IDF underneath; newer IDF uses broker.address.uri + session.keepalive

  mcfg.uri       = brokerUri;
  mcfg.keepalive = keepAliveSec;

  if (willTopic && willPayload) {
    mcfg.lwt_topic  = willTopic;
    mcfg.lwt_msg    = willPayload;
    mcfg.lwt_qos    = willQos;
    mcfg.lwt_retain = willRetain;
  }

  mqtt = esp_mqtt_client_init(&mcfg);
  if (!mqtt) {
    Serial.println("[MQTT] init failed");
    return false;
  }

  esp_mqtt_client_register_event(
    mqtt,
    MQTT_EVENT_ANY,
    &mqtt_event_handler_trampoline,
    this
  );

  if (esp_mqtt_client_start(mqtt) != ESP_OK) {
    Serial.println("[MQTT] start failed");
    esp_mqtt_client_destroy(mqtt);
    mqtt = nullptr;
    return false;
  }

  // ---- Create user tasks  ----
  userTasks = nullptr;

  for (size_t i = 0; i < core.taskCount(); ++i) {
    const statemq::TaskDef& t = core.task(i);

    auto* ctx = new (std::nothrow) UserTaskCtx{};
    if (!ctx) continue;

    ctx->owner = this;
    ctx->fn = t.callback;
    ctx->fnEx = t.callbackEx;
    ctx->user = t.user;
    ctx->period_ms = t.period_ms;
    ctx->handle = nullptr;
    ctx->next = nullptr;
    ctx->id = i;

    TaskHandle_t handle = nullptr;
    const uint32_t stackBytes = stackBytesFor(t.stack);

    BaseType_t ok = xTaskCreatePinnedToCore(
      user_task_trampoline,
      t.name ? t.name : "statemq_task",
      stackBytes / sizeof(StackType_t),
      ctx,
      STATEMQ_TASK_PRIORITY_USER,
      &handle,
      1
    );

    if (ok != pdPASS) {
      delete ctx;
      continue;
    }

    ctx->handle = handle;

    ctx->next = userTasks;
    userTasks = ctx;

    if (!t.enabled && handle) {
      vTaskSuspend(handle);
    }
  }

  startReconnectTask();
  return true;
}

// reconnect supervisor
void StateMQEsp32::startReconnectTask() {
  if (reconnectTask) return;

  xTaskCreatePinnedToCore(
    reconnect_task_trampoline,
    "statemq_reconnect",
    3072 / sizeof(StackType_t),
    this,
    STATEMQ_TASK_PRIORITY_USER,
    &reconnectTask,
    1
  );
}

void StateMQEsp32::reconnect_task_trampoline(void* arg) {
  auto* self = static_cast<StateMQEsp32*>(arg);
  if (!self) return;

  for (;;) {
    self->reconnectLoop();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void StateMQEsp32::reconnectLoop() {
  const uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (!printedWifi) {
      Serial.println("[WiFi] disconnected");
      printedWifi = true;
    }
    printedMqtt = false;


    // Block may influcence connectivity, please uncomment if it works for you
    // if (now >= nextTryMs) {
    //   Serial.println("[WiFi] reconnecting...");
    //   WiFi.disconnect(false);
    //   WiFi.begin(wifiSsid ? wifiSsid : "", wifiPass ? wifiPass : "");
    //   nextTryMs = now + backoffMs;
    //   backoffMs = (backoffMs * 2U > 30000U) ? 30000U : (backoffMs * 2U);
    // }
    return;
  }

  printedWifi = false;

  // MQTT state is declared by events; if connected, reset retry scheduling.
  if (!mqtt || mqttConnected) {
    backoffMs = 2000;
    nextTryMs = 0;
    printedMqtt = false;
    return;
  }

  if (!printedMqtt) {
    Serial.println("[MQTT] disconnected");
    printedMqtt = true;
  }

  if (now >= nextTryMs) {
    Serial.println("[MQTT] reconnecting...");
    esp_mqtt_client_reconnect(mqtt);
    nextTryMs = now + backoffMs;
    backoffMs = (backoffMs * 2U > 30000U) ? 30000U : (backoffMs * 2U);
  }
}

// MQTT events
void StateMQEsp32::onMqttEvent(esp_mqtt_event_handle_t event) {
  if (!event) return;

  switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
      mqttConnected = true;
      printedMqtt = false;
      Serial.println("[MQTT] connected");

      lockCoreBlocking();
      core.setConnected(true);
      unlockCore();

      // event-driven
      subscribeAllUnique();
      break;

    case MQTT_EVENT_DISCONNECTED:
      mqttConnected = false;

      lockCoreBlocking();
      core.setConnected(false);
      unlockCore();

      nextTryMs = millis() + backoffMs;
      break;

    case MQTT_EVENT_DATA: {
      char topic[RAW_TOPIC_LEN];
      char data[RAW_PAYLOAD_LEN];

      const size_t tlen = (event->topic_len > 0) ? (size_t)event->topic_len : 0;
      const size_t dlen = (event->data_len  > 0) ? (size_t)event->data_len  : 0;
      if (tlen == 0) break;

      size_t tcopy = (tlen < sizeof(topic) - 1) ? tlen : (sizeof(topic) - 1);
      size_t dcopy = (dlen < sizeof(data)  - 1) ? dlen : (sizeof(data)  - 1);

      memcpy(topic, event->topic, tcopy);
      topic[tcopy] = '\0';

      memcpy(data, event->data, dcopy);
      data[dcopy] = '\0';

      lockCoreBlocking();

      core.applyMessage(topic, data);

      int idx = rawIndex(topic);
      if (idx >= 0) {
        RawSlot& s = raw[(size_t)idx];
        strncpy(s.payload, data, RAW_PAYLOAD_LEN);
        s.payload[RAW_PAYLOAD_LEN - 1] = '\0';
        s.hasNew = true;
      }

      unlockCore();
      break;
    }

    default:
      break;
  }
}

// subscribe unique topics using per-topic QoS
void StateMQEsp32::subscribeAllUnique() {
  if (!mqtt) return;

  lockCoreBlocking();

  // subscribe STATE topics
  const size_t n = core.ruleCount();
  for (size_t i = 0; i < n; ++i) {
    const statemq::Rule& r = core.rule(i);
    if (!r.topic) continue;

    bool already = false;
    for (size_t j = 0; j < i; ++j) {
      if (core.rule(j).topic && strcmp(core.rule(j).topic, r.topic) == 0) {
        already = true;
        break;
      }
    }
    if (!already) {
      esp_mqtt_client_subscribe(mqtt, r.topic, qosForTopic(r.topic));
    }
  }

  // subscribe RAW topics (once)
  for (size_t i = 0; i < rawCount; ++i) {
    const char* t = raw[i].topic;
    if (t && *t) {
      esp_mqtt_client_subscribe(mqtt, t, qosForTopic(t));
    }
  }

  unlockCore();
}

// publish helper
bool StateMQEsp32::publish(const char* topic,
                           const char* payload,
                           int qos,
                           bool retain) {
  if (!topic || !payload) return false;

  if (!mqtt || !mqttConnected) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  if (qos < 0) qos = 0;
  if (qos > 2) qos = 2;

  return esp_mqtt_client_publish(mqtt, topic, payload, 0, qos, retain) >= 0;
}

//enable state publish
void StateMQEsp32::StatePublishTopic(const char* topic, int qos, bool enable, bool retain) {
  freestr(stateTopic);
  stateTopic = dupstr(topic ? topic : "");
  statePubQos = qos;
  statePubEnabled = enable && stateTopic && stateTopic[0];
  statePubRetain  = retain;

  hasLastStatePub = false;
  lastStatePub = statemq::StateMQ::OFFLINE_ID;

   core.onStateChange(&StateMQEsp32::on_state_change_trampoline, this);

}



bool StateMQEsp32::connected() const {
  return (WiFi.status() == WL_CONNECTED) && mqttConnected;
}

// enable/disable a task by id
bool StateMQEsp32::taskEnable(statemq::StateMQ::TaskId id, bool enable) {
  core.taskEnable(id, enable);

  UserTaskCtx* cur = userTasks;
  while (cur) {
    if (cur->id == id && cur->handle) {
      enable ? vTaskResume(cur->handle) : vTaskSuspend(cur->handle);
      return true;
    }
    cur = cur->next;
  }
  return false;
}

// trampolines
void StateMQEsp32::user_task_trampoline(void* arg) {
  UserTaskCtx* ctx = static_cast<UserTaskCtx*>(arg);
  if (!ctx) vTaskDelete(nullptr);

  for (;;) {
    bool locked = true;
    if (ctx && ctx->owner) {
      locked = ctx->owner->tryLockCoreForMs(10);
    }

    if (locked) {
      if (ctx->fn) {
        ctx->fn();
      } else if (ctx->fnEx) {
        ctx->fnEx(ctx->user);
      }

      if (ctx && ctx->owner) ctx->owner->unlockCore();
    }

    vTaskDelay(pdMS_TO_TICKS(ctx ? ctx->period_ms : 1000));
  }
}

void StateMQEsp32::mqtt_event_handler_trampoline(void* handler_args,
                                                 const char* base,
                                                 int event_id,
                                                 void* event_data) {
  (void)base;
  (void)event_id;

  StateMQEsp32* self = static_cast<StateMQEsp32*>(handler_args);
  if (!self) return;

  self->onMqttEvent((esp_mqtt_event_handle_t)event_data);
}

void StateMQEsp32::on_state_change_trampoline(const statemq::StateMQ::StateChangeCtx& ctx) {
  auto* self = static_cast<StateMQEsp32*>(ctx.user);
  if (!self) return;

  if (!self->statePubEnabled) return;
  if (!self->stateTopic || !self->stateTopic[0]) return;
  if (!self->mqtt || !self->mqttConnected) return;

  //last published as prev to keep prev/curr consistent 
  statemq::StateMQ::StateId prevId =
      self->hasLastStatePub ? self->lastStatePub : ctx.prev;

  statemq::StateMQ::StateId currId = ctx.curr;

  self->lastStatePub = currId;
  self->hasLastStatePub = true;

  const char* prevName = self->core.stateName(prevId);
  const char* currName = self->core.stateName(currId);

  // Arduino uptime 
  const uint32_t uptime_ms = (uint32_t)millis();

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"prev\":\"%s\",\"curr\":\"%s\",\"uptime_ms\":%lu}",
           prevName ? prevName : "",
           currName ? currName : "",
           (unsigned long)uptime_ms);

  int q = self->statePubQos;
  if (q < 0) q = 1;        
  if (q > 2) q = 2;

  esp_mqtt_client_publish(
      self->mqtt,
      self->stateTopic,
      payload,
      0,
      q,
      self->statePubRetain
  );
}

