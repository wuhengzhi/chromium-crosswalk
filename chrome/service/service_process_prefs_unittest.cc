// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/files/scoped_temp_dir.h"
#include "base/message_loop/message_loop.h"
#include "base/sequenced_task_runner.h"
#include "chrome/service/service_process_prefs.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

class ServiceProcessPrefsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());

    prefs_.reset(new ServiceProcessPrefs(
        temp_dir_.path().AppendASCII("service_process_prefs.txt"),
        message_loop_.task_runner().get()));
  }

  void TearDown() override { prefs_.reset(); }

  // The path to temporary directory used to contain the test operations.
  base::ScopedTempDir temp_dir_;
  // A message loop that we can use as the file thread message loop.
  base::MessageLoop message_loop_;
  std::unique_ptr<ServiceProcessPrefs> prefs_;
};

// Test ability to retrieve prefs
TEST_F(ServiceProcessPrefsTest, RetrievePrefs) {
  prefs_->SetBoolean("testb", true);
  prefs_->SetString("tests", "testvalue");
  prefs_->WritePrefs();
  message_loop_.RunUntilIdle();
  prefs_->SetBoolean("testb", false);   // overwrite
  prefs_->SetString("tests", std::string());  // overwrite
  prefs_->ReadPrefs();
  EXPECT_EQ(prefs_->GetBoolean("testb", false), true);
  EXPECT_EQ(prefs_->GetString("tests", std::string()), "testvalue");
}
