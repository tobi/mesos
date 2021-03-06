/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>

#include <map>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <process/future.hpp>
#include <process/gmock.hpp>

#include "common/protobuf_utils.hpp"

#include "detector/detector.hpp"

#include "local/local.hpp"

#include "master/allocator.hpp"
#include "master/hierarchical_allocator_process.hpp"
#include "master/master.hpp"

#include "slave/process_isolator.hpp"
#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::protobuf;
using namespace mesos::internal::tests;

using mesos::internal::master::Allocator;
using mesos::internal::master::HierarchicalDRFAllocatorProcess;
using mesos::internal::master::Master;

using mesos::internal::slave::ProcessIsolator;
using mesos::internal::slave::Slave;
using mesos::internal::slave::STATUS_UPDATE_RETRY_INTERVAL;

using process::Clock;
using process::Future;
using process::Message;
using process::PID;

using std::string;
using std::map;
using std::vector;

using testing::_;
using testing::AnyOf;
using testing::AtMost;
using testing::DoAll;
using testing::ElementsAre;
using testing::Eq;
using testing::Not;
using testing::Return;
using testing::SaveArg;


class FaultToleranceTest : public MesosTest {};


// This test checks that when a slave is lost,
// its offer(s) is rescinded.
TEST_F(FaultToleranceTest, SlaveLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  ProcessIsolator isolator;

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_EQ(1u, offers.get().size());

  Future<Nothing> offerRescinded;
  EXPECT_CALL(sched, offerRescinded(&driver, offers.get()[0].id()))
    .WillOnce(FutureSatisfy(&offerRescinded));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, offers.get()[0].slave_id()))
    .WillOnce(FutureSatisfy(&slaveLost));

  process::terminate(slave);

  AWAIT_READY(offerRescinded);
  AWAIT_READY(slaveLost);

  driver.stop();
  driver.join();

  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


// This test checks that a scheduler gets a slave lost
// message for a partioned slave.
TEST_F(FaultToleranceTest, SlavePartitioned)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  // Set these expectations up before we spawn the slave (in
  // local::launch) so that we don't miss the first PING.
  Future<Message> ping = FUTURE_MESSAGE(Eq("PING"), _, _);

  // Drop all the PONGs to simulate slave partition.
  DROP_MESSAGES(Eq("PONG"), _, _);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  // Need to make sure the framework AND slave have registered with
  // master. Waiting for resource offers should accomplish both.
  AWAIT_READY(resourceOffers);

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  // Now advance through the PINGs.
  uint32_t pings = 0;
  while(true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == master::MAX_SLAVE_PING_TIMEOUTS) {
     break;
    }
    ping = FUTURE_MESSAGE(Eq("PING"), _, _);
    Clock::advance(master::SLAVE_PING_TIMEOUT.secs());
  }

  Clock::advance(master::SLAVE_PING_TIMEOUT.secs());

  AWAIT_READY(slaveLost);

  driver.stop();
  driver.join();

  local::shutdown();

  Clock::resume();
}


TEST_F(FaultToleranceTest, SchedulerFailover)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, 1 * Gigabyte, false);

  // Launch the first (i.e., failing) scheduler and wait until
  // registered gets called to launch the second (i.e., failover)
  // scheduler.

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillRepeatedly(Return());

  driver1.start();

  AWAIT_READY(frameworkId);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // gets a registered callback..

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId.get());

  MesosSchedulerDriver driver2(&sched2, framework2, master);

  Future<Nothing> sched2Registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId.get(), _))
    .WillOnce(FutureSatisfy(&sched2Registered));

  EXPECT_CALL(sched2, resourceOffers(&driver2, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched2, offerRescinded(&driver2, _))
    .Times(AtMost(1));

  // Scheduler1's expectations.
  EXPECT_CALL(sched1, offerRescinded(&driver1, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"));

  driver2.start();

  AWAIT_READY(sched2Registered);

  EXPECT_EQ(DRIVER_STOPPED, driver2.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver2.join());

  EXPECT_EQ(DRIVER_ABORTED, driver1.stop());
  EXPECT_EQ(DRIVER_STOPPED, driver1.join());

  local::shutdown();
}


TEST_F(FaultToleranceTest, FrameworkReliableRegistration)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  // Drop the first framework registered message, allow subsequent messages.
  Future<FrameworkRegisteredMessage> frameworkRegisteredMessage =
    DROP_PROTOBUF(FrameworkRegisteredMessage(), _, _);

  driver.start();

  AWAIT_READY(frameworkRegisteredMessage);

  Clock::advance(1.0); // TODO(benh): Pull out constant from SchedulerProcess.

  AWAIT_READY(registered); // Ensures registered message is received.

  driver.stop();
  driver.join();

  local::shutdown();

  Clock::resume();
}


TEST_F(FaultToleranceTest, FrameworkReregister)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  PID<Master> master = local::launch(1, 2, 1 * Gigabyte, 1 * Gigabyte, false);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillRepeatedly(Return());

  Future<process::Message> message =
    FUTURE_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _);

  driver.start();

  AWAIT_READY(message); // Framework registered message, to get the pid.
  AWAIT_READY(registered); // Framework registered call.

  Future<Nothing> reregistered;
  EXPECT_CALL(sched, reregistered(&driver, _))
    .WillOnce(FutureSatisfy(&reregistered));

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  // Simulate a spurious newMasterDetected event (e.g., due to ZooKeeper
  // expiration) at the scheduler.
  NewMasterDetectedMessage newMasterDetectedMsg;
  newMasterDetectedMsg.set_pid(master);

  process::post(message.get().to, newMasterDetectedMsg);

  AWAIT_READY(reregistered);

  driver.stop();
  driver.join();

  local::shutdown();
}


// TOOD(vinod): Disabling this test for now because
// of the following race condition breaking this test:
// We do a driver.launchTasks() after post(noMasterDetected)
// but since dispatch (which is used by launchTasks()) uses
// a different queue than post, it might so happen that the latter
// message is dequeued before the former, thus breaking the test.
TEST_F(FaultToleranceTest, DISABLED_TaskLost)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(0);

  EXPECT_CALL(exec, launchTask(_, _))
    .Times(0);

  EXPECT_CALL(exec, shutdown(_))
    .Times(0);

  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);
  vector<Offer> offers;
  trigger statusUpdateCall, resourceOffersCall;
  TaskStatus status;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&offers),
                    Trigger(&resourceOffersCall)))
    .WillRepeatedly(Return());

  EXPECT_CALL(sched, offerRescinded(&driver, _))
    .Times(AtMost(1));

  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(DoAll(SaveArg<1>(&status),
                    Trigger(&statusUpdateCall)));

  process::Message message;

  EXPECT_MESSAGE(Eq(FrameworkRegisteredMessage().GetTypeName()), _, _)
    .WillOnce(DoAll(SaveArgField<0>(&process::MessageEvent::message, &message),
                    Return(false)));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  // Simulate a spurious noMasterDetected event at the scheduler.
  NoMasterDetectedMessage noMasterDetectedMsg;
  process::post(message.to, noMasterDetectedMsg);

  EXPECT_NE(0u, offers.size());

  TaskInfo task;
  task.set_name("test task");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers[0].slave_id());
  task.mutable_resources()->MergeFrom(offers[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  driver.launchTasks(offers[0].id(), tasks);

  WAIT_UNTIL(statusUpdateCall);

  EXPECT_EQ(status.state(), TASK_LOST);

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


// This test checks that a failover scheduler gets the
// retried status update.
TEST_F(FaultToleranceTest, SchedulerFailoverStatusUpdate)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  // Launch the first (i.e., failing) scheduler.
  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());

  driver1.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  // Launch a task.
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  EXPECT_CALL(exec, registered(_, _, _, _))
    .Times(1);

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  // Drop the first status update message
  // between master and the scheduler.
  Future<StatusUpdateMessage> statusUpdateMessage =
    DROP_PROTOBUF(StatusUpdateMessage(), _, Not(AnyOf(Eq(master), Eq(slave))));

  driver1.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(statusUpdateMessage);

  // Now launch the second (i.e., failover) scheduler using the
  // framework id recorded from the first scheduler and wait until it
  // registers.

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(&sched2, framework2, master);

  Future<Nothing> registered2;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered2));

  // Scheduler1 should get an error due to failover.
  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"));

  driver2.start();

  AWAIT_READY(registered2);

  // Now advance time enough for the reliable timeout
  // to kick in and another status update is sent.
  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched2, statusUpdate(&driver2, _))
    .WillOnce(FutureSatisfy(&statusUpdate));

  Clock::advance(STATUS_UPDATE_RETRY_INTERVAL.secs());

  AWAIT_READY(statusUpdate);

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  AWAIT_READY(shutdown); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);

  Clock::resume();
}


TEST_F(FaultToleranceTest, ForwardStatusUpdateUnknownExecutor)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());
  Offer offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<Nothing> statusUpdate;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureSatisfy(&statusUpdate));    // TASK_RUNNING of task1.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offer.id(), tasks);

  // Wait until TASK_RUNNING of task1 is received.
  AWAIT_READY(statusUpdate);

  // Simulate the slave receiving status update from an unknown
  // (e.g. exited) executor of the given framework.
  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));           // TASK_RUNNING of task2.

  TaskID taskId;
  taskId.set_value("task2");

  StatusUpdate statusUpdate2 = createStatusUpdate(
      frameworkId, offer.slave_id(), taskId, TASK_RUNNING, "Dummy update");

  process::dispatch(slave, &Slave::statusUpdate, statusUpdate2);

  // Ensure that the scheduler receives task2's update.
  AWAIT_READY(status);
  EXPECT_EQ(taskId, status.get().task_id());
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver.stop();
  driver.join();

  AWAIT_READY(shutdown); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(FaultToleranceTest, SchedulerFailoverFrameworkMessage)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched1;
  MesosSchedulerDriver driver1(&sched1, DEFAULT_FRAMEWORK_INFO, master);

  FrameworkID frameworkId;
  EXPECT_CALL(sched1, registered(&driver1, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched1, resourceOffers(&driver1, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver1.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched1, statusUpdate(&driver1, _))
    .WillOnce(FutureArg<1>(&status));

  ExecutorDriver* execDriver;
  EXPECT_CALL(exec, registered(_, _, _, _))
    .WillOnce(SaveArg<0>(&execDriver));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver1.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  MockScheduler sched2;

  FrameworkInfo framework2; // Bug in gcc 4.1.*, must assign on next line.
  framework2 = DEFAULT_FRAMEWORK_INFO;
  framework2.mutable_id()->MergeFrom(frameworkId);

  MesosSchedulerDriver driver2(&sched2, framework2, master);

  Future<Nothing> registered;
  EXPECT_CALL(sched2, registered(&driver2, frameworkId, _))
    .WillOnce(FutureSatisfy(&registered));

  Future<Nothing> frameworkMessage;
  EXPECT_CALL(sched2, frameworkMessage(&driver2, _, _, _))
    .WillOnce(FutureSatisfy(&frameworkMessage));

  EXPECT_CALL(sched1, error(&driver1, "Framework failed over"));

  driver2.start();

  AWAIT_READY(registered);

  execDriver->sendFrameworkMessage("Executor to Framework message");

  AWAIT_READY(frameworkMessage);

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver1.stop();
  driver2.stop();

  driver1.join();
  driver2.join();

  AWAIT_READY(shutdown); // Ensures MockExecutor can be deallocated.

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


// This test checks that a scheduler exit shuts down the executor.
TEST_F(FaultToleranceTest, SchedulerExit)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  trigger statusUpdateMsg;
  process::Message message;

  MockExecutor exec;
  TestingIsolator isolator(DEFAULT_EXECUTOR_ID, &exec);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  FrameworkID frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(SaveArg<1>(&frameworkId));

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_resources()->MergeFrom(offers.get()[0].resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  vector<TaskInfo> tasks;
  tasks.push_back(task);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status.get().state());

  Future<Nothing> shutdown;
  EXPECT_CALL(exec, shutdown(_))
    .WillOnce(FutureSatisfy(&shutdown));

  driver.stop();
  driver.join();

  AWAIT_READY(shutdown);

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}


TEST_F(FaultToleranceTest, SlaveReliableRegistration)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Clock::pause();

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  ProcessIsolator isolator;

  // Drop the first slave registered message, allow subsequent messages.
  Future<SlaveRegisteredMessage> slaveRegisteredMessage
    = DROP_PROTOBUF(SlaveRegisteredMessage(), _, _);

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(slaveRegisteredMessage);

  Clock::advance(1.0); // TODO(benh): Pull out constant from Slave.

  AWAIT_READY(resourceOffers);

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);

  Clock::resume();
}


TEST_F(FaultToleranceTest, SlaveReregisterOnZKExpiration)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  HierarchicalDRFAllocatorProcess allocator;
  Allocator a(&allocator);
  Files files;
  Master m(&a, &files);
  PID<Master> master = process::spawn(&m);

  ProcessIsolator isolator;

  Slave s(slaveFlags, true, &isolator, &files);
  PID<Slave> slave = process::spawn(&s);

  BasicMasterDetector detector(master, slave, true);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<Nothing> resourceOffers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureSatisfy(&resourceOffers))
    .WillRepeatedly(Return());  // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(resourceOffers);

  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
      FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  // Simulate a spurious newMasterDetected event (e.g., due to ZooKeeper
  // expiration) at the slave.

  NewMasterDetectedMessage message;
  message.set_pid(master);

  process::post(slave, message);

  AWAIT_READY(slaveReregisteredMessage);

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}
