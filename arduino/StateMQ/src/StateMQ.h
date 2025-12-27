// StateMQ.h
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

#if defined(ESP_PLATFORM) || (defined(ARDUINO) && defined(ESP32))
  #include "freertos/FreeRTOS.h"
  #include "freertos/semphr.h"
#else
  #error "StateMQ supports ESP32 only."
#endif


namespace statemq {

// Resource hint for scheduled callbacks.
// Does not imply a dedicated thread or RTOS task.
enum class Stack {
  Small,
  Medium,
  Large
};

struct TaskDef {
  const char* name;
  uint32_t    period_ms;
  Stack       stack;
  void      (*callback)();
  // Context
  void      (*callbackEx)(void* user);
  void*       user;
  bool        enabled;
};



// Maps an incoming topic + payload to a declared state.
struct Rule {
  const char* topic;
  const char* message;
  uint8_t     stateId;
};

// Core StateMQ node holding state, rules, and scheduled tasks.
class StateMQ {
public:
  using TaskId  = size_t;
  using StateId = uint8_t;

  StateMQ();

  // Declare a valid state and map it to a topic/message pair.
  StateId map(const char* topic, const char* message, const char* state);

  // Register a periodic callback managed by the internal scheduler.
  TaskId taskEvery(const char* name,
                  uint32_t period_ms,
                  Stack stack,
                  void (*callback)(),
                  bool enabled = true);

  // context
  TaskId taskEvery(const char* name,
                  uint32_t period_ms,
                  Stack stack,
                  void (*callback)(void* user),
                  void* user,
                  bool enabled = true);


  bool taskEnable(TaskId id, bool enable);
  bool taskEnabled(TaskId id) const;

  // Always returns a valid state (OFFLINE, CONNECTED, or user state).
  const char* state() const;

  StateId stateId() const;

  bool connected() const;

  enum class StateChangeCause : uint8_t {
    Unknown   = 0,
    RuleMatch = 1,
    Connected = 2,
    Disconn   = 3
  };

  struct StateChangeCtx {
    StateId prev;
    StateId desired;
    StateId curr;
    StateChangeCause cause;
    int16_t ruleIndex;
    const char* topic;
    const char* payload;
    void* user;
  };

  using StateChangeCb   = void (*)(StateId prev, StateId next);
  using StateChangeCbEx = void (*)(const StateChangeCtx& ctx);

  void onStateChange(StateChangeCb cb);
  void onStateChange(StateChangeCbEx cb, void* user = nullptr);

  const char* stateName(StateId id) const;

  // Platform backends drive these functions.
  bool applyMessage(const char* topic, const char* payload);
  void setConnected(bool connected);

  size_t taskCount() const;
  const TaskDef& task(size_t index) const;

  size_t ruleCount() const;
  const Rule& rule(size_t index) const;

  static constexpr const char* OFFLINE_STATE   = "OFFLINE";
  static constexpr const char* CONNECTED_STATE = "CONNECTED";

  static constexpr StateId OFFLINE_ID   = 0;
  static constexpr StateId CONNECTED_ID = 1;

  SemaphoreHandle_t mutexHandle() const { return mutex; }

private:
  void lock() const;
  void unlock() const;

  struct Guard {
    explicit Guard(const StateMQ& n) : n(n) { n.lock(); }
    ~Guard() { n.unlock(); }
    const StateMQ& n;
  };

  void setStateId(StateId desiredId,
                  bool userState,
                  StateChangeCause cause,
                  const char* topic,
                  const char* payload,
                  int16_t ruleIndex);

  bool isKnownState(const char* s) const;
  void addKnownState(const char* s);
  StateId stateIdForKnown(const char* s) const;
  const char* stateStrForId(StateId id) const;

  // Maximum number of user-defined states (excluding OFFLINE / CONNECTED).
  static constexpr size_t MAX_KNOWN_STATES = 32;

  // Maximum number of (topic, payload) → state rules.
  static constexpr size_t MAX_RULES        = 32;

  // Maximum number of scheduled periodic tasks.
  static constexpr size_t MAX_TASKS        = 8;

  // Maximum length of state names (including null terminator).
  static constexpr size_t STATE_LEN        = 16;


  Rule     rules[MAX_RULES];
  size_t   ruleCount_;

  TaskDef  tasks[MAX_TASKS];
  size_t   taskCount_;

  StateId  stateId_;
  StateId  lastUserStateId_;

  char     knownStates[MAX_KNOWN_STATES][STATE_LEN];
  size_t   knownStateCount;

  bool     connected_;
  StateChangeCb   stateCb;
  StateChangeCbEx stateCbEx;
  void*           stateCbUser;

  mutable StaticSemaphore_t mutexBuf;
  mutable SemaphoreHandle_t mutex;
};

} // namespace statemq
