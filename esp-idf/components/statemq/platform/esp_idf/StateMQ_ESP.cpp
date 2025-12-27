#include "StateMQ_ESP.h"

#include <cstring>
#include <cstdio>
#include <new>

extern "C" {
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
}

namespace statemq {

static const char* TAG_WIFI = "statemq_wifi";
static const char* TAG_MQTT = "statemq_mqtt";



// Helpers

static uint32_t stackBytesFor(Stack s) {
  switch (s) {
    case Stack::Small:  return 2048;
    case Stack::Medium: return 4096;
    case Stack::Large:  return 8192;
    default:            return 2048;
  }
}

static int clamp_qos(int q) {
  if (q < 0) return 0;
  if (q > 2) return 2;
  return q;
}

static bool topic_seen(const char* topic, const char* const* seen, size_t seen_n) {
  for (size_t i = 0; i < seen_n; ++i) {
    if (seen[i] && topic && std::strcmp(seen[i], topic) == 0) return true;
  }
  return false;
}

static uint32_t uptime_ms() {
  return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// String helpers

char* StateMQEsp::dupstr(const char* s) {
  if (!s) return nullptr;
  const size_t n = std::strlen(s);
  char* out = new (std::nothrow) char[n + 1];
  if (!out) return nullptr;
  std::memcpy(out, s, n);
  out[n] = '\0';
  return out;
}

void StateMQEsp::freestr(char*& s) {
  delete[] s;
  s = nullptr;
}

// User task trampoline

void StateMQEsp::user_task_trampoline(void* arg) {
  auto* ctx = static_cast<UserTaskCtx*>(arg);
  if (!ctx) vTaskDelete(nullptr);

  for (;;) {
    if (ctx->cb) {
      ctx->cb();
    } else if (ctx->cbEx) {
      ctx->cbEx(ctx->user);
    }
    vTaskDelay(pdMS_TO_TICKS(ctx->period_ms));
  }
}


// Construction / destruction

StateMQEsp::StateMQEsp(StateMQ& core)
: core(core) {}

StateMQEsp::~StateMQEsp() {
  end(false);
}

// Configuration

void StateMQEsp::setKeepAliveSeconds(uint16_t sec) {
  keepAliveSec = (sec == 0) ? 30 : sec;
}

void StateMQEsp::setDefaultSubscribeQos(int qos) {
  defaultSubQos = clamp_qos(qos);
}

void StateMQEsp::setDefaultPublishQos(int qos) {
  defaultPubQos = clamp_qos(qos);
}


void StateMQEsp::freeQosOverrides() {
  for (size_t i = 0; i < qosCount; ++i) {
    freestr(qosTopic[i]);
    qosTopic[i] = nullptr;
  }
  qosCount = 0;
}

int StateMQEsp::qosForTopic(const char* topic) const {
  if (!topic) return defaultSubQos;

  for (size_t i = 0; i < qosCount; ++i) {
    if (qosTopic[i] && std::strcmp(qosTopic[i], topic) == 0) {
      return qosValue[i];
    }
  }
  return defaultSubQos;
}

void StateMQEsp::setLastWill(const char* topic,
                             const char* payload,
                             int qos,
                             bool retain) {
  if (!topic || !payload) return;

  qos = clamp_qos(qos);

  freestr(willTopic);
  freestr(willPayload);

  willTopic   = dupstr(topic);
  willPayload = dupstr(payload);

  if (!willTopic || !willPayload) {
    freestr(willTopic);
    freestr(willPayload);
    lwtEnabled = false;
    return;
  }

  willQos    = (int8_t)qos;
  willRetain = retain;
  lwtEnabled = true;
}

void StateMQEsp::clearLastWill() {
  freestr(willTopic);
  freestr(willPayload);
  lwtEnabled = false;
}

// Raw subscribe

int StateMQEsp::rawIndex(const char* topic) const {
  if (!topic) return -1;
  for (size_t i = 0; i < rawCount; ++i) {
    if (raw[i].topic[0] && std::strcmp(raw[i].topic, topic) == 0) return (int)i;
  }
  return -1;
}

bool StateMQEsp::subscribe(const char* topic, int qos) {
  if (!topic || !*topic) return false;

  qos = clamp_qos(qos);

  int idx = rawIndex(topic);
  if (idx < 0) {
    if (rawCount >= MAX_RAW_SUBS) return false;

    RawSlot& s = raw[rawCount];
    std::strncpy(s.topic, topic, RAW_TOPIC_LEN);
    s.topic[RAW_TOPIC_LEN - 1] = '\0';
    s.payload[0] = '\0';
    s.hasNew = false;
    rawCount++;
  }

  bool updated = false;
  for (size_t i = 0; i < qosCount; ++i) {
    if (qosTopic[i] && std::strcmp(qosTopic[i], topic) == 0) {
      qosValue[i] = (int8_t)qos;
      updated = true;
      break;
    }
  }
  if (!updated && qosCount < MAX_QOS_OVERRIDES) {
    char* copy = dupstr(topic);
    if (copy) {
      qosTopic[qosCount] = copy;
      qosValue[qosCount] = (int8_t)qos;
      qosCount++;
    }
  }

  // If already connected, subscribe immediately.
  if (mqttConnected && client) {
    esp_mqtt_client_subscribe(client, topic, qos);
  }

  return true;
}

const char* StateMQEsp::msg(const char* topic) {
  if (!topic || !*topic) return nullptr;

  int idx = rawIndex(topic);
  if (idx < 0) return nullptr;

  RawSlot& s = raw[(size_t)idx];
  if (!s.hasNew) return nullptr;

  s.hasNew = false;
  return s.payload;
}

// Lifecycle

void StateMQEsp::cleanup(bool disconnect_wifi, bool clear_config) {
  stopMqtt();

  wifiHasIp = false;
  mqttConnected = false;
  core.setConnected(false);

  // stop user tasks
  if (taskHandles) {
    for (size_t i = 0; i < taskHandlesCount; ++i) {
      if (taskHandles[i]) {
        vTaskDelete((TaskHandle_t)taskHandles[i]);
        taskHandles[i] = nullptr;
      }
    }
    delete[] taskHandles;
    taskHandles = nullptr;
  }

  // free task contexts
  if (taskCtxs) {
    for (size_t i = 0; i < taskHandlesCount; ++i) {
      delete taskCtxs[i];
      taskCtxs[i] = nullptr;
    }
    delete[] taskCtxs;
    taskCtxs = nullptr;
  }

  taskHandlesCount = 0;


  freestr(wifiSsid);
  freestr(wifiPass);
  freestr(brokerUri);

  if (clear_config) {
    freestr(stateTopic);
    freeQosOverrides();
    clearLastWill();
    rawCount = 0;
    for (size_t i = 0; i < MAX_RAW_SUBS; ++i) {
      raw[i].topic[0] = '\0';
      raw[i].payload[0] = '\0';
      raw[i].hasNew = false;
    }
    StatePublishTopic("", -1, false);
  }


  if (disconnect_wifi) {
    esp_wifi_disconnect();
    esp_wifi_stop();
  }
}

void StateMQEsp::end(bool disconnect_wifi) {
  cleanup(disconnect_wifi, true);
}

//public current and previous state upon transition
void StateMQEsp::StatePublishTopic(const char* topic, int qos, bool enable, bool retain) {
  freestr(stateTopic);
  stateTopic = dupstr(topic ? topic : "");
  statePubQos = qos;
  statePubEnabled = enable && (stateTopic && stateTopic[0]);
  statePubRetain = retain;

  hasLastStatePub = false;
  lastStatePub = StateMQ::OFFLINE_ID;
}



bool StateMQEsp::begin(const char* wifi_ssid,
                       const char* wifi_pass,
                       const char* broker_uri)
{
  cleanup(false, false);

  if (!wifi_ssid || !broker_uri) return false;

  wifiSsid   = dupstr(wifi_ssid);
  wifiPass   = dupstr(wifi_pass ? wifi_pass : "");
  brokerUri  = dupstr(broker_uri);

  if (!wifiSsid || !wifiPass || !brokerUri) {
    end(false);
    return false;
  }

  core.setConnected(false);

  // ---- init WiFi  ----
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &StateMQEsp::wifi_event_handler, this, nullptr));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &StateMQEsp::wifi_event_handler, this, nullptr));

  wifi_config_t wc{};
  std::memset(&wc, 0, sizeof(wc));
  std::strncpy((char*)wc.sta.ssid, wifiSsid, sizeof(wc.sta.ssid));
  std::strncpy((char*)wc.sta.password, wifiPass, sizeof(wc.sta.password));
  wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG_WIFI, "WiFi start -> connecting...");
  ESP_ERROR_CHECK(esp_wifi_connect());

  core.onStateChange(&StateMQEsp::on_state_change_trampoline, this);

  // ---- start tasks ----
  taskHandlesCount = core.taskCount();
  taskHandles = new (std::nothrow) void*[taskHandlesCount]();
  taskCtxs    = new (std::nothrow) UserTaskCtx*[taskHandlesCount](); // NEW

  for (size_t i = 0; i < core.taskCount(); ++i) {
    const TaskDef& t = core.task(i);

    auto* ctx = new (std::nothrow) UserTaskCtx{t.callback, t.callbackEx, t.user, t.period_ms};
    if (!ctx) continue;

    TaskHandle_t handle = nullptr;
    const uint32_t stackBytes = stackBytesFor(t.stack);

    if (xTaskCreate(
          user_task_trampoline,
          t.name ? t.name : "statemq_task",
          stackBytes / sizeof(StackType_t),
          ctx,
          1,
          &handle) != pdPASS) {
      delete ctx;
      if (taskCtxs && i < taskHandlesCount) taskCtxs[i] = nullptr;
      continue;
    }

    if (taskHandles && i < taskHandlesCount) taskHandles[i] = handle;
    if (taskCtxs    && i < taskHandlesCount) taskCtxs[i] = ctx;

    if (!t.enabled && handle) {
      vTaskSuspend(handle);
    }
  }

  return true;
}


// Runtime

bool StateMQEsp::connected() const {
  return mqttConnected;
}

bool StateMQEsp::publish(const char* topic,
                         const char* payload,
                         int qos,
                         bool retain) {
  if (!topic || !topic[0]) return false;
  if (!client || !mqttConnected) return false;

  int q = (qos < 0) ? defaultPubQos : qos;
  q = clamp_qos(q);

  const char* msg = payload ? payload : "";
  int msg_id = esp_mqtt_client_publish(client, topic, msg, 0, q, retain);
  return msg_id >= 0;
}

bool StateMQEsp::taskEnable(StateMQ::TaskId id, bool enable) {
  if (id >= core.taskCount()) return false;
  if (!taskHandles || id >= taskHandlesCount) return false;

  core.taskEnable(id, enable);

  TaskHandle_t h = (TaskHandle_t)taskHandles[id];
  if (!h) return false;

  enable ? vTaskResume(h) : vTaskSuspend(h);
  return true;
}

// Subscriptions

void StateMQEsp::subscribeAllUnique() {
  if (!client) return;

  const char* seen[64];
  size_t seen_n = 0;

  for (size_t i = 0; i < core.ruleCount(); ++i) {
    const char* t = core.rule(i).topic;
    if (!t || !*t) continue;
    if (topic_seen(t, seen, seen_n)) continue;

    if (seen_n < 64) seen[seen_n++] = t;
    esp_mqtt_client_subscribe(client, t, qosForTopic(t));
  }

  for (size_t i = 0; i < rawCount; ++i) {
    const char* t = raw[i].topic;
    if (!t || !*t) continue;
    if (topic_seen(t, seen, seen_n)) continue;

    if (seen_n < 64) seen[seen_n++] = t;
    esp_mqtt_client_subscribe(client, t, qosForTopic(t));
  }
}

// WiFi events 

void StateMQEsp::wifi_event_handler(void* arg,
                                    esp_event_base_t base,
                                    int32_t id,
                                    void* data) {
  (void)data;
  auto* self = static_cast<StateMQEsp*>(arg);
  if (!self) return;

  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    self->onWifiGotIp();
    return;
  }

  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    self->onWifiDisconnected();
    return;
  }
}

void StateMQEsp::onWifiGotIp() {
  wifiHasIp = true;
  ESP_LOGI(TAG_WIFI, "Got IP -> start MQTT");
  startMqttIfNeeded();
}

void StateMQEsp::onWifiDisconnected() {
  wifiHasIp = false;
  mqttConnected = false;
  core.setConnected(false);

  ESP_LOGW(TAG_WIFI, "WiFi disconnected -> reconnect");
  esp_wifi_connect();
}

// MQTT events 

void StateMQEsp::mqtt_event_handler(void* arg,
                                    esp_event_base_t base,
                                    int32_t id,
                                    void* data) {
  (void)base;
  (void)id;
  auto* self = static_cast<StateMQEsp*>(arg);
  if (!self) return;

  auto* e = (esp_mqtt_event_handle_t)data;
  if (!e) return;

  switch (e->event_id) {
    case MQTT_EVENT_CONNECTED:
      self->onMqttConnected();
      break;
    case MQTT_EVENT_DISCONNECTED:
      self->onMqttDisconnected();
      break;
    case MQTT_EVENT_DATA:
      self->onMqttData(e);
      break;
    default:
      break;
  }
}

void StateMQEsp::onMqttConnected() {
  mqttConnected = true;
  core.setConnected(true);
  ESP_LOGI(TAG_MQTT, "MQTT connected");
  subscribeAllUnique();
}

void StateMQEsp::onMqttDisconnected() {
  mqttConnected = false;
  core.setConnected(false);
  ESP_LOGW(TAG_MQTT, "MQTT disconnected");
}

void StateMQEsp::onMqttData(esp_mqtt_event_handle_t e) {
  const size_t tlen = (e->topic_len > 0) ? (size_t)e->topic_len : 0;
  const size_t dlen = (e->data_len  > 0) ? (size_t)e->data_len  : 0;

  if (tlen == 0) return;

  char topic[RAW_TOPIC_LEN];
  char data[RAW_PAYLOAD_LEN];

  size_t tcopy = (tlen < sizeof(topic) - 1) ? tlen : (sizeof(topic) - 1);
  size_t dcopy = (dlen < sizeof(data)  - 1) ? dlen : (sizeof(data)  - 1);

  std::memcpy(topic, e->topic, tcopy);
  topic[tcopy] = '\0';

  std::memcpy(data, e->data, dcopy);
  data[dcopy] = '\0';

  core.applyMessage(topic, data);

  int idx = rawIndex(topic);
  if (idx >= 0) {
    RawSlot& s = raw[(size_t)idx];
    std::strncpy(s.payload, data, RAW_PAYLOAD_LEN);
    s.payload[RAW_PAYLOAD_LEN - 1] = '\0';
    s.hasNew = true;
  }
}

// MQTT start/stop

void StateMQEsp::startMqttIfNeeded() {
  if (client) return;
  if (!wifiHasIp) return;
  if (!brokerUri || !brokerUri[0]) return;

  esp_mqtt_client_config_t c{};
  c.broker.address.uri = brokerUri;
  c.session.keepalive = (keepAliveSec > 0) ? keepAliveSec : 30;

  if (lwtEnabled && willTopic && willTopic[0]) {
    c.session.last_will.topic  = willTopic;
    c.session.last_will.msg    = willPayload ? willPayload : "offline";
    c.session.last_will.qos    = clamp_qos(willQos);
    c.session.last_will.retain = willRetain;
  }

  client = esp_mqtt_client_init(&c);
  if (!client) {
    ESP_LOGE(TAG_MQTT, "esp_mqtt_client_init failed");
    return;
  }

  // register events
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, &StateMQEsp::mqtt_event_handler, this);

  esp_err_t ok = esp_mqtt_client_start(client);
  if (ok != ESP_OK) {
    ESP_LOGE(TAG_MQTT, "esp_mqtt_client_start failed: %d", (int)ok);
    esp_mqtt_client_destroy(client);
    client = nullptr;
    return;
  }

  ESP_LOGI(TAG_MQTT, "MQTT started (waiting for MQTT_EVENT_CONNECTED) uri=%s", brokerUri);
}

void StateMQEsp::stopMqtt() {
  if (!client) return;

  esp_mqtt_client_stop(client);
  esp_mqtt_client_destroy(client);
  client = nullptr;
}


void StateMQEsp::on_state_change_trampoline(const StateMQ::StateChangeCtx& ctx) {
  auto* self = static_cast<StateMQEsp*>(ctx.user);
  if (!self) return;

  if (!self->statePubEnabled) return;
  if (!self->stateTopic || !self->stateTopic[0]) return;
  if (!self->client || !self->mqttConnected) return;

  // Use last published as prev
  StateMQ::StateId prevId = self->hasLastStatePub ? self->lastStatePub : ctx.prev;
  StateMQ::StateId currId = ctx.curr;
  self->lastStatePub = currId;
  self->hasLastStatePub = true;

  const char* prevName = self->core.stateName(prevId);
  const char* currName = self->core.stateName(currId);

  char payload[256];
  std::snprintf(payload, sizeof(payload),
                "{\"prev\":\"%s\",\"curr\":\"%s\",\"uptime_ms\":%u}",
                prevName ? prevName : "",
                currName ? currName : "",
                (unsigned)uptime_ms());

  int q = (self->statePubQos < 0) ? self->defaultPubQos : self->statePubQos;
  q = clamp_qos(q);

  esp_mqtt_client_publish(self->client, self->stateTopic, payload, 0, q, self->retainState);
}




} // namespace statemq
