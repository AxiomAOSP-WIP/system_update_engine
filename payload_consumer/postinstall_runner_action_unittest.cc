//
// Copyright (C) 2012 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "update_engine/payload_consumer/postinstall_runner_action.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include <base/bind.h>
#include <base/files/file_util.h>
#include <base/message_loop/message_loop.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <brillo/bind_lambda.h>
#include <brillo/message_loops/base_message_loop.h>
#include <brillo/message_loops/message_loop_utils.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "update_engine/common/constants.h"
#include "update_engine/common/fake_boot_control.h"
#include "update_engine/common/fake_hardware.h"
#include "update_engine/common/test_utils.h"
#include "update_engine/common/utils.h"

using brillo::MessageLoop;
using chromeos_update_engine::test_utils::ScopedLoopbackDeviceBinder;
using std::string;
using std::vector;

namespace chromeos_update_engine {

class PostinstActionProcessorDelegate : public ActionProcessorDelegate {
 public:
  PostinstActionProcessorDelegate() = default;
  void ProcessingDone(const ActionProcessor* processor,
                      ErrorCode code) override {
    MessageLoop::current()->BreakLoop();
    processing_done_called_ = true;
  }
  void ProcessingStopped(const ActionProcessor* processor) override {
    MessageLoop::current()->BreakLoop();
    processing_stopped_called_ = true;
  }

  void ActionCompleted(ActionProcessor* processor,
                       AbstractAction* action,
                       ErrorCode code) override {
    if (action->Type() == PostinstallRunnerAction::StaticType()) {
      code_ = code;
      code_set_ = true;
    }
  }

  ErrorCode code_{ErrorCode::kError};
  bool code_set_{false};
  bool processing_done_called_{false};
  bool processing_stopped_called_{false};
};

class MockPostinstallRunnerActionDelegate
    : public PostinstallRunnerAction::DelegateInterface {
 public:
  MOCK_METHOD1(ProgressUpdate, void(double progress));
};

class PostinstallRunnerActionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_.SetAsCurrent();
    async_signal_handler_.Init();
    subprocess_.Init(&async_signal_handler_);
    // These tests use the postinstall files generated by "generate_images.sh"
    // stored in the "disk_ext2_unittest.img" image.
    postinstall_image_ =
        test_utils::GetBuildArtifactsPath("gen/disk_ext2_unittest.img");
  }

  // Setup an action processor and run the PostinstallRunnerAction with a single
  // partition |device_path|, running the |postinstall_program| command from
  // there.
  void RunPosinstallAction(const string& device_path,
                           const string& postinstall_program,
                           bool powerwash_required);

 public:
  void ResumeRunningAction() {
    ASSERT_NE(nullptr, postinstall_action_);
    postinstall_action_->ResumeAction();
  }

  void SuspendRunningAction() {
    if (!postinstall_action_ || !postinstall_action_->current_command_ ||
        test_utils::Readlink(base::StringPrintf(
            "/proc/%d/fd/0", postinstall_action_->current_command_)) !=
            "/dev/zero") {
      // We need to wait for the postinstall command to start and flag that it
      // is ready by redirecting its input to /dev/zero.
      loop_.PostDelayedTask(
          FROM_HERE,
          base::Bind(&PostinstallRunnerActionTest::SuspendRunningAction,
                     base::Unretained(this)),
          base::TimeDelta::FromMilliseconds(100));
    } else {
      postinstall_action_->SuspendAction();
      // Schedule to be resumed in a little bit.
      loop_.PostDelayedTask(
          FROM_HERE,
          base::Bind(&PostinstallRunnerActionTest::ResumeRunningAction,
                     base::Unretained(this)),
          base::TimeDelta::FromMilliseconds(100));
    }
  }

  void CancelWhenStarted() {
    if (!postinstall_action_ || !postinstall_action_->current_command_) {
      // Wait for the postinstall command to run.
      loop_.PostDelayedTask(
          FROM_HERE,
          base::Bind(&PostinstallRunnerActionTest::CancelWhenStarted,
                     base::Unretained(this)),
          base::TimeDelta::FromMilliseconds(10));
    } else {
      CHECK(processor_);
      processor_->StopProcessing();
    }
  }

 protected:
  base::MessageLoopForIO base_loop_;
  brillo::BaseMessageLoop loop_{&base_loop_};
  brillo::AsynchronousSignalHandler async_signal_handler_;
  Subprocess subprocess_;

  // The path to the postinstall sample image.
  string postinstall_image_;

  FakeBootControl fake_boot_control_;
  FakeHardware fake_hardware_;
  PostinstActionProcessorDelegate processor_delegate_;

  // The PostinstallRunnerAction delegate receiving the progress updates.
  PostinstallRunnerAction::DelegateInterface* setup_action_delegate_{nullptr};

  // A pointer to the posinstall_runner action and the processor.
  PostinstallRunnerAction* postinstall_action_{nullptr};
  ActionProcessor* processor_{nullptr};
};

void PostinstallRunnerActionTest::RunPosinstallAction(
    const string& device_path,
    const string& postinstall_program,
    bool powerwash_required) {
  ActionProcessor processor;
  processor_ = &processor;
  ObjectFeederAction<InstallPlan> feeder_action;
  InstallPlan::Partition part;
  part.name = "part";
  part.target_path = device_path;
  part.run_postinstall = true;
  part.postinstall_path = postinstall_program;
  InstallPlan install_plan;
  install_plan.partitions = {part};
  install_plan.download_url = "http://127.0.0.1:8080/update";
  install_plan.powerwash_required = powerwash_required;
  feeder_action.set_obj(install_plan);
  PostinstallRunnerAction runner_action(&fake_boot_control_, &fake_hardware_);
  postinstall_action_ = &runner_action;
  runner_action.set_delegate(setup_action_delegate_);
  BondActions(&feeder_action, &runner_action);
  ObjectCollectorAction<InstallPlan> collector_action;
  BondActions(&runner_action, &collector_action);
  processor.EnqueueAction(&feeder_action);
  processor.EnqueueAction(&runner_action);
  processor.EnqueueAction(&collector_action);
  processor.set_delegate(&processor_delegate_);

  loop_.PostTask(FROM_HERE,
                 base::Bind([&processor] { processor.StartProcessing(); }));
  loop_.Run();
  ASSERT_FALSE(processor.IsRunning());
  postinstall_action_ = nullptr;
  processor_ = nullptr;
  EXPECT_TRUE(processor_delegate_.processing_stopped_called_ ||
              processor_delegate_.processing_done_called_);
  if (processor_delegate_.processing_done_called_) {
    // Sanity check that the code was set when the processor finishes.
    EXPECT_TRUE(processor_delegate_.code_set_);
  }
}

TEST_F(PostinstallRunnerActionTest, ProcessProgressLineTest) {
  PostinstallRunnerAction action(&fake_boot_control_, &fake_hardware_);
  testing::StrictMock<MockPostinstallRunnerActionDelegate> mock_delegate_;
  action.set_delegate(&mock_delegate_);

  action.current_partition_ = 1;
  action.partition_weight_ = {1, 2, 5};
  action.accumulated_weight_ = 1;
  action.total_weight_ = 8;

  // 50% of the second action is 2/8 = 0.25 of the total.
  EXPECT_CALL(mock_delegate_, ProgressUpdate(0.25));
  action.ProcessProgressLine("global_progress 0.5");
  testing::Mock::VerifyAndClearExpectations(&mock_delegate_);

  // 1.5 should be read as 100%, to catch rounding error cases like 1.000001.
  // 100% of the second is 3/8 of the total.
  EXPECT_CALL(mock_delegate_, ProgressUpdate(0.375));
  action.ProcessProgressLine("global_progress 1.5");
  testing::Mock::VerifyAndClearExpectations(&mock_delegate_);

  // None of these should trigger a progress update.
  action.ProcessProgressLine("foo_bar");
  action.ProcessProgressLine("global_progress");
  action.ProcessProgressLine("global_progress ");
  action.ProcessProgressLine("global_progress NaN");
  action.ProcessProgressLine("global_progress Exception in ... :)");
}

// Test that postinstall succeeds in the simple case of running the default
// /postinst command which only exits 0.
TEST_F(PostinstallRunnerActionTest, RunAsRootSimpleTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPosinstallAction(loop.dev(), kPostinstallDefaultScript, false);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
  EXPECT_TRUE(processor_delegate_.processing_done_called_);

  // Since powerwash_required was false, this should not trigger a powerwash.
  EXPECT_FALSE(fake_hardware_.IsPowerwashScheduled());
}

TEST_F(PostinstallRunnerActionTest, RunAsRootRunSymlinkFileTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPosinstallAction(loop.dev(), "bin/postinst_link", false);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
}

TEST_F(PostinstallRunnerActionTest, RunAsRootPowerwashRequiredTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  // Run a simple postinstall program but requiring a powerwash.
  RunPosinstallAction(loop.dev(), "bin/postinst_example", true);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);

  // Check that powerwash was scheduled.
  EXPECT_TRUE(fake_hardware_.IsPowerwashScheduled());
}

// Runs postinstall from a partition file that doesn't mount, so it should
// fail.
TEST_F(PostinstallRunnerActionTest, RunAsRootCantMountTest) {
  RunPosinstallAction("/dev/null", kPostinstallDefaultScript, false);
  EXPECT_EQ(ErrorCode::kPostinstallRunnerError, processor_delegate_.code_);

  // In case of failure, Postinstall should not signal a powerwash even if it
  // was requested.
  EXPECT_FALSE(fake_hardware_.IsPowerwashScheduled());
}

// Check that the failures from the postinstall script cause the action to
// fail.
TEST_F(PostinstallRunnerActionTest, RunAsRootErrScriptTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPosinstallAction(loop.dev(), "bin/postinst_fail1", false);
  EXPECT_EQ(ErrorCode::kPostinstallRunnerError, processor_delegate_.code_);
}

// The exit code 3 and 4 are a specials cases that would be reported back to
// UMA with a different error code. Test those cases are properly detected.
TEST_F(PostinstallRunnerActionTest, RunAsRootFirmwareBErrScriptTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPosinstallAction(loop.dev(), "bin/postinst_fail3", false);
  EXPECT_EQ(ErrorCode::kPostinstallBootedFromFirmwareB,
            processor_delegate_.code_);
}

// Check that you can't specify an absolute path.
TEST_F(PostinstallRunnerActionTest, RunAsRootAbsolutePathNotAllowedTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPosinstallAction(loop.dev(), "/etc/../bin/sh", false);
  EXPECT_EQ(ErrorCode::kPostinstallRunnerError, processor_delegate_.code_);
}

#ifdef __ANDROID__
// Check that the postinstall file is relabeled to the postinstall label.
// SElinux labels are only set on Android.
TEST_F(PostinstallRunnerActionTest, RunAsRootCheckFileContextsTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  RunPosinstallAction(loop.dev(), "bin/self_check_context", false);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
}
#endif  // __ANDROID__

// Check that you can suspend/resume postinstall actions.
TEST_F(PostinstallRunnerActionTest, RunAsRootSuspendResumeActionTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);

  // We need to wait for the child to run and setup its signal handler.
  loop_.PostTask(FROM_HERE,
                 base::Bind(&PostinstallRunnerActionTest::SuspendRunningAction,
                            base::Unretained(this)));
  RunPosinstallAction(loop.dev(), "bin/postinst_suspend", false);
  // postinst_suspend returns 0 only if it was suspended at some point.
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
  EXPECT_TRUE(processor_delegate_.processing_done_called_);
}

// Test that we can cancel a postinstall action while it is running.
TEST_F(PostinstallRunnerActionTest, RunAsRootCancelPostinstallActionTest) {
  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);

  // Wait for the action to start and then cancel it.
  CancelWhenStarted();
  RunPosinstallAction(loop.dev(), "bin/postinst_suspend", false);
  // When canceling the action, the action never finished and therefore we had
  // a ProcessingStopped call instead.
  EXPECT_FALSE(processor_delegate_.code_set_);
  EXPECT_TRUE(processor_delegate_.processing_stopped_called_);
}

// Test that we parse and process the progress reports from the progress
// file descriptor.
TEST_F(PostinstallRunnerActionTest, RunAsRootProgressUpdatesTest) {
  testing::StrictMock<MockPostinstallRunnerActionDelegate> mock_delegate_;
  testing::InSequence s;
  EXPECT_CALL(mock_delegate_, ProgressUpdate(0));

  // The postinst_progress program will call with 0.25, 0.5 and 1.
  EXPECT_CALL(mock_delegate_, ProgressUpdate(0.25));
  EXPECT_CALL(mock_delegate_, ProgressUpdate(0.5));
  EXPECT_CALL(mock_delegate_, ProgressUpdate(1.));

  EXPECT_CALL(mock_delegate_, ProgressUpdate(1.));

  ScopedLoopbackDeviceBinder loop(postinstall_image_, false, nullptr);
  setup_action_delegate_ = &mock_delegate_;
  RunPosinstallAction(loop.dev(), "bin/postinst_progress", false);
  EXPECT_EQ(ErrorCode::kSuccess, processor_delegate_.code_);
}

}  // namespace chromeos_update_engine
