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

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <stout/os.hpp>
#include <stout/try.hpp>

#include "detector/detector.hpp"

#include "master/master.hpp"

#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::Future;
using process::PID;

using std::map;
using std::vector;

using testing::_;


class MasterDetectorTest : public MesosClusterTest {};


TEST_F(MasterDetectorTest, File)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  Try<PID<Master> > master = cluster.masters.start();
  ASSERT_SOME(master);

  TestingIsolator isolator;
  Slave s(cluster.slaves.flags, true, &isolator, &cluster.files);
  PID<Slave> slave = process::spawn(&s);

  // Write "master" to a file and use the "file://" mechanism to
  // create a master detector for the slave.
  Try<std::string> path = os::mktemp();
  ASSERT_SOME(path);
  ASSERT_SOME(os::write(path.get(), master.get()));

  Try<MasterDetector*> detector =
    MasterDetector::create("file://" + path.get(), slave, false, true);

  os::rm(path.get());

  ASSERT_SOME(detector);

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master.get());

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  Future<vector<Offer> > offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  AWAIT_UNTIL(offers);

  driver.stop();
  driver.join();

  cluster.shutdown();

  process::terminate(slave);
  process::wait(slave);
}
