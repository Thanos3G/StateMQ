// StateMQ.cpp
#include "StateMQ.h"
#include <cstring>

namespace statemq {

// ------------ locking ------------
// All core state (rules/tasks/stateId/knownStates) is protected by this lock.
void StateMQ::lock() const {
#if defined(ESP_PLATFORM) || (defined(ARDUINO) && defined(ESP32))
  if (mutex) xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
#endif
}


void StateMQ::unlock() const {
#if defined(ESP_PLATFORM) || (defined(ARDUINO) && defined(ESP32))
  if (mutex) xSemaphoreGiveRecursive(mutex);
#endif
}



// ------------ known state helpers ------------
bool StateMQ::isKnownState(const char* s) const {
  if (!s) return false;

  if (std::strncmp(s, OFFLINE_STATE,   STATE_LEN) == 0) return true;
  if (std::strncmp(s, CONNECTED_STATE, STATE_LEN) == 0) return true;

  for (size_t i = 0; i < knownStateCount; ++i) {
    if (std::strncmp(knownStates[i], s, STATE_LEN) == 0) return true;
  }
  return false;
}

void StateMQ::addKnownState(const char* s) {
  if (!s) return;

  if (std::strncmp(s, OFFLINE_STATE,   STATE_LEN) == 0) return;
  if (std::strncmp(s, CONNECTED_STATE, STATE_LEN) == 0) return;

  if (isKnownState(s)) return;
  if (knownStateCount >= MAX_KNOWN_STATES) return;

  std::strncpy(knownStates[knownStateCount], s, STATE_LEN);
  knownStates[knownStateCount][STATE_LEN - 1] = '\0';
  knownStateCount++;
}

// StateId layout:
//   0 = OFFLINE_ID
//   1 = CONNECTED_ID
//   2.. = user-defined known states in insertion order (knownStates[])
// StateId values must remain stable for the lifetime of the node.


StateMQ::StateId StateMQ::stateIdForKnown(const char* s) const {
  if (!s) return CONNECTED_ID;

  if (std::strncmp(s, OFFLINE_STATE,   STATE_LEN) == 0) return OFFLINE_ID;
  if (std::strncmp(s, CONNECTED_STATE, STATE_LEN) == 0) return CONNECTED_ID;

  for (size_t i = 0; i < knownStateCount; ++i) {
    if (std::strncmp(knownStates[i], s, STATE_LEN) == 0) {
      return (StateId)(2 + i);
    }
  }
  return CONNECTED_ID;
}

// Unknown/invalid IDs resolve to CONNECTED_STATE to keep the public API
// always returning a valid state.

const char* StateMQ::stateStrForId(StateId id) const {
  if (id == OFFLINE_ID) return OFFLINE_STATE;
  if (id == CONNECTED_ID) return CONNECTED_STATE;

  const size_t idx = (size_t)(id - 2);
  if (idx < knownStateCount) return knownStates[idx];

  return CONNECTED_STATE;
}

// ------------ ctor ------------
StateMQ::StateMQ()
  : ruleCount_(0),
    taskCount_(0),
    stateId_(OFFLINE_ID),
    lastUserStateId_(CONNECTED_ID),
    knownStateCount(0),
    connected_(false),
    stateCb(nullptr),
    stateCbEx(nullptr),
    stateCbUser(nullptr),
    mutex(nullptr)
{
#if defined(ESP_PLATFORM) || (defined(ARDUINO) && defined(ESP32))
  mutex = xSemaphoreCreateRecursiveMutexStatic(&mutexBuf);
  if (!mutex) {
    mutex = xSemaphoreCreateRecursiveMutex();
  }
#endif
}


// ------------ USER API ------------
StateMQ::StateId StateMQ::map(const char* topic, const char* message, const char* state) {
  if (!topic || !message || !state) return CONNECTED_ID;

  Guard g(*this);
  if (ruleCount_ >= MAX_RULES) return CONNECTED_ID;

  // Reserved states are not inserted into the rule table as user states.
  // Returning their IDs allows user code to compare against them.

  if (std::strncmp(state, OFFLINE_STATE,   STATE_LEN) == 0) return OFFLINE_ID;
  if (std::strncmp(state, CONNECTED_STATE, STATE_LEN) == 0) return CONNECTED_ID;

  addKnownState(state);
  StateId id = stateIdForKnown(state);

  rules[ruleCount_].topic   = topic;
  rules[ruleCount_].message = message;
  rules[ruleCount_].stateId = id;
  ruleCount_++;

  return id;
}

StateMQ::TaskId StateMQ::taskEvery(const char* name,
                                   uint32_t period_ms,
                                   Stack stack,
                                   void (*callback)(),
                                   bool enabled) {
  if (!callback) return (TaskId)-1;

  Guard g(*this);
  if (taskCount_ >= MAX_TASKS) return (TaskId)-1;

  tasks[taskCount_] = TaskDef{
    name, period_ms, stack,
    callback,
    nullptr,
    nullptr,
    enabled
  };
  return taskCount_++;
}

StateMQ::TaskId StateMQ::taskEvery(const char* name,
                                   uint32_t period_ms,
                                   Stack stack,
                                   void (*callback)(void*),
                                   void* user,
                                   bool enabled) {
  if (!callback) return (TaskId)-1;

  Guard g(*this);
  if (taskCount_ >= MAX_TASKS) return (TaskId)-1;

  tasks[taskCount_] = TaskDef{
    name, period_ms, stack,
    nullptr,
    callback,
    user,
    enabled
  };
  return taskCount_++;
}



bool StateMQ::taskEnable(TaskId id, bool enable) {
  Guard g(*this);
  if (id >= taskCount_) return false;
  tasks[id].enabled = enable;
  return true;
}

bool StateMQ::taskEnabled(TaskId id) const {
  Guard g(*this);
  if (id >= taskCount_) return false;
  return tasks[id].enabled;
  
}
// Returns a thread-local copy to provide a stable C-string even if the
// underlying state changes immediately after this call.

const char* StateMQ::state() const {
  thread_local char copy[STATE_LEN];
  Guard g(*this);

  const char* s = OFFLINE_STATE;

  if (!connected_) {
    s = OFFLINE_STATE;
  } else {
    if (stateId_ >= 2) {
      const size_t idx = (size_t)(stateId_ - 2);
      s = (idx < knownStateCount) ? knownStates[idx] : CONNECTED_STATE;
    } else {
      s = CONNECTED_STATE;
    }
  }

  std::strncpy(copy, s, STATE_LEN);
  copy[STATE_LEN - 1] = '\0';
  return copy;
}

const char* StateMQ::stateName(StateId id) const {
  Guard g(*this);
  return stateStrForId(id);
}


StateMQ::StateId StateMQ::stateId() const {
  Guard g(*this);

  if (!connected_) return OFFLINE_ID;
  if (stateId_ >= 2) return stateId_;
  return CONNECTED_ID;
}

bool StateMQ::connected() const {
  Guard g(*this);
  return connected_;
}

void StateMQ::onStateChange(StateChangeCb cb) {
  Guard g(*this);
  stateCb = cb;
}

void StateMQ::onStateChange(StateChangeCbEx cb, void* user) {
  Guard g(*this);
  stateCbEx = cb;
  stateCbUser = user;
}

// ------------ PLATFORM API ------------
// Topic/payload matching is string-based; on match we transition using integer state IDs.

bool StateMQ::applyMessage(const char* topic, const char* payload) {
  if (!topic || !payload) return false;

  StateId matched = CONNECTED_ID;
  int16_t matchedRule = -1;
  bool found = false;

  {
    Guard g(*this);
    for (size_t i = 0; i < ruleCount_; ++i) {
      if (std::strcmp(rules[i].topic, topic) == 0 &&
          std::strcmp(rules[i].message, payload) == 0) {
        matched = rules[i].stateId;
        matchedRule = (int16_t)i;
        found = true;
        break;
      }
    }
  }

  if (found) {
    setStateId(matched, true, StateChangeCause::RuleMatch, topic, payload, matchedRule);
    return true;
  }
  return false;
}

void StateMQ::setConnected(bool connectedIn) {
  StateId target = OFFLINE_ID;
  StateChangeCause cause = connectedIn ? StateChangeCause::Connected : StateChangeCause::Disconn;

  {
    Guard g(*this);
    connected_ = connectedIn;

    if (!connected_) {
      target = OFFLINE_ID;
    } else {
      target = (lastUserStateId_ >= 2) ? lastUserStateId_ : CONNECTED_ID;
    }
  }

  setStateId(target, false, cause, nullptr, nullptr, -1);
}

size_t StateMQ::taskCount() const {
  Guard g(*this);
  return taskCount_;
}

const TaskDef& StateMQ::task(size_t index) const {
  Guard g(*this);
  return tasks[index];
}

size_t StateMQ::ruleCount() const {
  Guard g(*this);
  return ruleCount_;
}

const Rule& StateMQ::rule(size_t index) const {
  Guard g(*this);
  return rules[index];
}

// ------------ INTERNAL ------------
// Topic/payload matching is string-based; on match we transition using integer state IDs.

void StateMQ::setStateId(StateId desiredId,
                         bool userState,
                         StateChangeCause cause,
                         const char* topic,
                         const char* payload,
                         int16_t ruleIndex) {
  StateChangeCb cb = nullptr;
  StateChangeCbEx cbEx = nullptr;
  StateChangeCtx ctx{};

  bool fire = false;

  {
    Guard g(*this);

    const StateId prev = stateId_;
    StateId desired = desiredId;
    StateId applied = desiredId;

    if (!connected_) {
      applied = OFFLINE_ID;
      if (stateId_ == OFFLINE_ID) return;
      stateId_ = OFFLINE_ID;
      fire = true;
    } else {
      if (applied != OFFLINE_ID && applied != CONNECTED_ID) {
        const size_t idx = (size_t)(applied - 2);
        if (idx >= knownStateCount) applied = CONNECTED_ID;
      }

      if (userState && applied != OFFLINE_ID && applied != CONNECTED_ID) {
        lastUserStateId_ = applied;
      }

      if (stateId_ == applied) return;

      stateId_ = applied;
      fire = true;
    }

    cb   = stateCb;
    cbEx = stateCbEx;

    ctx.prev = prev;
    ctx.desired = desired;
    ctx.curr = stateId_;
    ctx.cause = cause;
    ctx.ruleIndex = ruleIndex;
    ctx.topic = topic;
    ctx.payload = payload;
    ctx.user = stateCbUser;
  }

  if (!fire) return;

  if (cb) cb(ctx.prev, ctx.curr);
  if (cbEx) cbEx(ctx);
}

} // namespace statemq
