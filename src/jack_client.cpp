/*
 * File: jack_client.cpp
 *
 *
 * Copyright 2020 Harald Postner <Harald at free_creations.de>.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "jack_client.h"
#include "spdlog/spdlog.h"
#include <mutex>
namespace jackClient {
inline namespace impl {

jack_client_t *g_hJackClient = nullptr;

/**
 * Protects the jackClient from being simultaneously accessed by multiple threads
 * while the state might change.
 */
static std::mutex g_stateAccessMutex;

static State g_stateFlag{State::closed};

inline State stateInternal() { return g_stateFlag; }

/**
 * Returns a string representation of the given state.
 * @param state - the state
 * @return a string representation of the given state
 */
std::string stateAsString(State state) {
  switch (state) {
  case State::closed:
    return "closed";
  case State::idle:
    return "idle";
  case State::running:
    return "running";
  }
}

std::string clientNameInternal() noexcept {
  if (g_stateFlag == State::closed) {
    return std::string("");
  }
  const char *actualClientName = jack_get_client_name(g_hJackClient);
  return std::string(actualClientName);
}

/**
 * we suppress all error messages from the JACK server.
 * @param msg - the message supplied by the server.
 */
void jackErrorCallback(const char *msg) {
  SPDLOG_INFO("jackClient::jackErrorCallback - "
              "\n --- message ---- \n"
              "{}"
              "\n --- end ---- \n",
              msg);
}
/**
 * we suppress all info messages from the JACK server.
 * @param msg - the message supplied by the server.
 */
void jackInfoCallback(const char *msg) {
  SPDLOG_INFO("jackClient::jackInfoCallback - "
              "\n --- message ---- \n"
              "{}"
              "\n --- end ---- \n",
              msg);
}

void stopInternal() {
  switch (g_stateFlag) {
  case State::closed:
  case State::idle:
    return; // do nothing if already stopped
  case State::running: {
    if (g_hJackClient) {
      SPDLOG_TRACE("jackClient::stopInternal - stopping \"{}\".", clientNameInternal());
      int err = jack_deactivate(g_hJackClient);
      if (err) {
        SPDLOG_ERROR("jackClient::stopInternal - Error({})", err);
      }
    }
  }
  }
  g_stateFlag = State::idle;
}

/** The duration of one cycle measured in SysTimeUnits */
static auto g_cycleLength = sysClock::SysTimeUnits();
/** The (input) deadline of the previous cycle */
static auto g_previousDeadline = sysClock::TimePoint();

using namespace std::chrono_literals;
/**
 * A short time elapse to compensate for possible jitter in synchronicity between JACKs timing and
 * systems timing.
 */
constexpr sysClock::SysTimeUnits JITTER_COMPENSATION{500us};
/**
 * Indicates the number of times that were needed to reset the global timing variables.
 * This counter is useful for debugging. Ideally, it stays at one for the entire session.
 */
std::atomic<int> g_resetTimingCount{0};
/**
 * Set the global timing variables to initial values so that a timing reset is forced
 * in the next cycle.
 *
 * Side Effects
 * ------------
 * The following global variables are reset:
 *
 * - **g_previousDeadline** will be set to the start point of the system clock.
 * - **g_cycleLength** will be set to zero.
 * - **g_resetTimingCount** will be set to zero.
 *
 */
void invalidateTiming() {
  g_cycleLength = sysClock::SysTimeUnits();
  g_previousDeadline = sysClock::TimePoint();
  g_resetTimingCount = 0;
}
/**
 * Recalculate the global timing variables that are needed to estimate the deadline for each cycle.
 *
 * Side Effects
 * ------------
 * The following global variables are set:
 *
 * - **g_previousDeadline** will be set to the same value as the one returned.
 * - **g_cycleLength** will be set to JACKs current best estimate of the cycle-period.
 * - **g_resetTimingCount** will be incremented by one.
 *
 * @return the deadline for incoming events that shall be taken into account in the current
 * cycle.
 */
sysClock::TimePoint resetTiming() {
  g_resetTimingCount++;
  jack_nframes_t cf; // (unused) JACKs frame time counter at the start of the current cycle
  jack_time_t cUs;   // (unused) JACKs microseconds time at the start of the current cycle
  jack_time_t nUs;   // (unused) JACKs microseconds time of the start of the next cycle
  float periodUsecs; //  JACKs current best estimate of the cycle-period time in microseconds.

  int err = jack_get_cycle_times(g_hJackClient, &cf, &cUs, &nUs, &periodUsecs);
  if (err) {
    SPDLOG_CRITICAL("jackClient::resetTiming - error({})", err);
    return sysClock::now();
  }
  g_cycleLength = sysClock::toSysTimeUnits(periodUsecs);
  SPDLOG_TRACE("jackClient::resetTiming - count {}, cycleLength {} us", g_resetTimingCount,
               sysClock::toMicrosecondFloat(g_cycleLength));

  jack_nframes_t framesSinceCycleStart = jack_frames_since_cycle_start(g_hJackClient);

  auto newDeadline = sysClock::now() - frames2duration(framesSinceCycleStart) - JITTER_COMPENSATION;
  g_previousDeadline = newDeadline;

  return newDeadline;
}
/**
 * Verify if the given value can be used as deadline (for incoming events) in
 * the current cycle.
 * @param deadline - a proposed value for the deadline
 * @return true if the given value lays (within some limits) in the previous cycle.
 */
bool isPlausible(sysClock::TimePoint deadline) {
  auto latestPossible = sysClock::now();
  if (deadline >= latestPossible) {
    SPDLOG_TRACE("jackClient::isPlausible - too late by {} us",
                 sysClock::toMicrosecondFloat(deadline - latestPossible));
    return false;
  }
  auto earliestPossible = sysClock::now() - g_cycleLength - JITTER_COMPENSATION;
  if (deadline < earliestPossible) {
    SPDLOG_TRACE("jackClient::isPlausible - too early by {} us",
                 sysClock::toMicrosecondFloat(earliestPossible - deadline));
    return false;
  }
  return true;
}

/**
 * Determine the new deadline for the current cycle.
 *
 * Side Effects
 * ------------
 * The following global variable is set:
 *
 * - **g_previousDeadline** will be set to the same value as the one returned.
 *
 * @return the deadline for incoming events that shall be taken into account in the current
 * cycle.
 */
sysClock::TimePoint newDeadline() {
  // lets first try the simple, fast and steady solution which consists in
  // simply adding the cycle-length to the previous deadline.
  auto tentativeDeadline = g_previousDeadline + g_cycleLength;
  if (isPlausible(tentativeDeadline)) {
    // OK, the simple solution is sufficient... so lets update the global variable and return
    g_previousDeadline = tentativeDeadline;
    return tentativeDeadline;
  }
  // simply adding the cycle-length to the previous deadline did not give a good result.
  // We'll need to completely reset our timing.
  return resetTiming();
}

/**
 * The 'g_customCallback' supersedes the `jackInternalCallback`.
 */
ProcessCallback g_customCallback = ProcessCallback();

/**
 * This callback will be invoked by the JACK server on each cycle.
 * It delegates to the custom defined callback.
 * @param nFrames - number of frames in the current cycle
 * @param arg - (unused) a pointer to an arbitrary, user supplied, data.
 * @return  0 on success, a non-zero value otherwise. __Returning a non-Zero value will stop
 * the client__.
 */
int jackInternalCallback(jack_nframes_t nFrames, [[maybe_unused]] void *arg) {
  if (g_customCallback) {
    return g_customCallback(nFrames, newDeadline());
  }
  return 0;
}
} // namespace impl
/**
 * The name given by the JACK server to this client.
 * As long as the client is not connected to the server, an empty string will be returned.
 * @return the name of this client.
 */
std::string deviceName() noexcept {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  return clientNameInternal();
}
/**
 * Disconnect this client from the JACK server.
 *
 * After this function has returned, the `jackClient` is back into the `closed` state.
 */
void close() noexcept {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  if (g_stateFlag == State::closed) {
    return;
  }
  stopInternal();

  if (g_hJackClient) {
    SPDLOG_TRACE("jackClient::close - closing \"{}\".", clientNameInternal());
    int err = jack_client_close(g_hJackClient);
    if (err) {
      SPDLOG_ERROR("jackClient::close - Error({})", err);
    }
  }

  g_hJackClient = nullptr;
  g_stateFlag = State::closed;
}
/**
 * Open an external client session with the JACK server.
 *
 * When this function succeeds the `jackClient` is in `idle` state.
 *
 * @param deviceName - a desired name for this client.
 * The server may modify this name to create a unique variant, if needed.
 * @param noStartServer - if true, does not automatically start the JACK server when it is not
 * already running.
 * @throws BadStateException - if the `jackClient` is not in `closed` state.
 * @throws ServerNotRunningException - if the JACK server is not running.
 * @throws ServerException - if the JACK server has encountered an other problem.
 */
void open(const char *deviceName, bool noStartServer) noexcept(false) {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  SPDLOG_TRACE("jackClient::open");

  if (g_stateFlag != State::closed) {
    throw BadStateException("Cannot open JACK client. Wrong state " + stateAsString(g_stateFlag));
  }

  // suppress jack error messages
  jack_set_error_function(jackErrorCallback);
  jack_set_info_function(jackInfoCallback);

  jack_status_t status;
  JackOptions options = (noStartServer) ? JackNoStartServer : JackNullOption;
  g_hJackClient = jack_client_open(deviceName, options, &status);
  if (!g_hJackClient) {
    SPDLOG_ERROR("Error opening JACK status={}.", status);
    throw ServerNotRunningException();
  }
  g_stateFlag = State::idle;
}
/**
 * Tell the Jack server to stop calling the processCallback function.
 * This client will be removed from the process graph. All ports belonging to
 * this client are closed.
 *
 * After this function returns, the `jackClient` is back into the `idle` state.
 */
void stop() noexcept {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  SPDLOG_TRACE("jackClient::stop");
  stopInternal();
}

/**
 * Indicates the current state of the `jackClient`.
 *
 * This function will block as long as a state-change is ongoing.
 *
 * @return the current state of the `jackClient`.
 */
State state() {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  return stateInternal();
}

/**
 * Start a new session.
 *
 * The processCallback function will be invoked by JACK server on each cycle.
 *
 * This function can only be called from the `connected` state.
 * After this function succeeds, the `jackClient` is in `running` state.
 *
 * @throws BadStateException - if this function is called on a state other than `idle`.
 * @throws ServerException - if the JACK server has encountered a problem.
 */
void activate() noexcept(false) {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  SPDLOG_TRACE("jackClient::activate");
  if (g_stateFlag != State::idle) {
    throw BadStateException("Cannot activate JACK client. Wrong state " +
                            stateAsString(g_stateFlag));
  }

  invalidateTiming();

  int err = jack_activate(g_hJackClient);
  if (err) {
    throw ServerException("Failed to activate JACK client!");
  }

  g_stateFlag = State::running;
}
/**
 * Tell the Jack server to call the given processCallback function on each cycle.
 *
 * `registerProcessCallback()` can only be called from the `idle` state.
 *
 * @param processCallback - the function to be called
 * @throws BadStateException - if this function is called from a state other than `connected`.
 * @throws ServerException - if the JACK server has encountered an other problem.
 */
void registerProcessCallback(const ProcessCallback &processCallback) noexcept(false) {
  std::unique_lock<std::mutex> lock{g_stateAccessMutex};
  SPDLOG_TRACE("jackClient::registerProcessCallback");
  if (g_stateFlag != State::idle) {
    throw BadStateException("Cannot register callback. Wrong state " + stateAsString(g_stateFlag));
  }
  g_customCallback = processCallback;
  int err = jack_set_process_callback(g_hJackClient, jackInternalCallback, nullptr);
  if (err) {
    throw ServerException("JACK error when registering callback.");
  }
}
} // namespace jackClient