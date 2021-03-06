/*
 * File: alsa_helper_test.cpp
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

#include "alsa_helper.h"
#include "spdlog/spdlog.h"
#include "gtest/gtest.h"
#include <climits>
#include <sys_clock.h>
#include <thread>

namespace unitTestHelpers {
// The fixture for testing class AlsaHelper.
class AlsaHelperTest : public ::testing::Test {

protected:
  AlsaHelperTest() {
    spdlog::set_level(spdlog::level::info);
    // spdlog::set_level(spdlog::level::trace);
    SPDLOG_INFO("AlsaHelperTest - started");
  }

  ~AlsaHelperTest() override { SPDLOG_INFO("AlsaHelperTest - ended"); }

  /**
   * Will be called right before each test.
   */
  void SetUp() override { AlsaHelper::openAlsaSequencer(); }

  /**
   * Will be called immediately after each test.
   */
  void TearDown() override { AlsaHelper::closeAlsaSequencer(); }
};

/**
 * The AlsaSequencer can be opened and can be  closed.
 */
TEST_F(AlsaHelperTest, openCloseAlsaSequencer) {
  // just let SetUp() and TearDown() do the work
}

/**
 * The Receiver of the AlsaHelper can be started and stopped.
 */
TEST_F(AlsaHelperTest, startStopEventReceiver) {

  auto futureEventCount = AlsaHelper::startEventReceiver();

  std::this_thread::sleep_for(
      std::chrono::milliseconds(std::lround(2.5 * AlsaHelper::SHUTDOWN_POLL_PERIOD_MS)));

  AlsaHelper::stopEventReceiver(futureEventCount);

  auto eventCount = futureEventCount.get();
  EXPECT_EQ(eventCount, 0);
}

/**
 * The AlsaHelper can emit events.
 */
TEST_F(AlsaHelperTest, sendEvents) {

  auto hEmitterPort = AlsaHelper::createOutputPort("output");

  // int eventCount = 2*4*60; // Emit during two minutes (enough time to connect the sequencer).
  int eventCount = 3; // for automatic testing
  // Send events four Notes per second (240 BPM).
  AlsaHelper::sendEvents(hEmitterPort, eventCount, 250);
}

/**
 * The Receiver of the AlsaHelper can receive events.
 */
TEST_F(AlsaHelperTest, receiveEvents) {

  auto futureEventCount = AlsaHelper::startEventReceiver();
  AlsaHelper::createInputPort("input");

  // long listeningTimeMs = 2*60*1000;  // enough time to manually connect keyboard or sequencer).
  long listeningTimeMs = 2; // for automatic test
  std::this_thread::sleep_for(std::chrono::milliseconds(listeningTimeMs));
  AlsaHelper::stopEventReceiver(futureEventCount);
}

/**
 * The Receiver of the AlsaHelper can send and receive events.
 */
TEST_F(AlsaHelperTest, sendReceiveEvents) {

  auto futureEventCount = AlsaHelper::startEventReceiver();

  auto hReceiverPort = AlsaHelper::createInputPort("input");
  auto hEmitterPort = AlsaHelper::createOutputPort("output");
  AlsaHelper::connectPorts(hEmitterPort, hReceiverPort);

  int eventPairsEmitted = 7;
  AlsaHelper::sendEvents(hEmitterPort, eventPairsEmitted, 250);

  AlsaHelper::stopEventReceiver(futureEventCount);
  auto eventsReceived = futureEventCount.get();

  EXPECT_EQ(2 * eventPairsEmitted, eventsReceived);
}

/**
 * The test-clock should be monotonic
 */
TEST_F(AlsaHelperTest, getClock) {
  using namespace std::chrono_literals;

  auto testClock = AlsaHelper::clock();
  long previousTimePoint{LONG_MIN};
  constexpr long repetitions = 100;

  auto start = testClock->now();
  for (int i = 0; i < repetitions; i++) {
    long testNow = testClock->now();
    std::this_thread::sleep_for(2ms);
    // check for monotonic increase and avoid to be optimized away.
    EXPECT_GE(testNow, previousTimePoint);
    previousTimePoint = testNow;
  }
  EXPECT_GT(previousTimePoint, start);
}
} // namespace unitTestHelpers
