// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/metrics/file_metrics_provider.h"

#include "base/files/file_util.h"
#include "base/files/memory_mapped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/macros.h"
#include "base/metrics/histogram.h"
#include "base/metrics/histogram_flattener.h"
#include "base/metrics/histogram_snapshot_manager.h"
#include "base/metrics/persistent_histogram_allocator.h"
#include "base/metrics/persistent_memory_allocator.h"
#include "base/metrics/sparse_histogram.h"
#include "base/metrics/statistics_recorder.h"
#include "base/strings/stringprintf.h"
#include "base/test/test_simple_task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "components/metrics/metrics_pref_names.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {
const char kMetricsName[] = "TestMetrics";
const char kMetricsFilename[] = "file.metrics";
}  // namespace

namespace metrics {

class HistogramFlattenerDeltaRecorder : public base::HistogramFlattener {
 public:
  HistogramFlattenerDeltaRecorder() {}

  void RecordDelta(const base::HistogramBase& histogram,
                   const base::HistogramSamples& snapshot) override {
    recorded_delta_histogram_names_.push_back(histogram.histogram_name());
  }

  void InconsistencyDetected(base::HistogramBase::Inconsistency problem)
      override {
    ASSERT_TRUE(false);
  }

  void UniqueInconsistencyDetected(
      base::HistogramBase::Inconsistency problem) override {
    ASSERT_TRUE(false);
  }

  void InconsistencyDetectedInLoggedCount(int amount) override {
    ASSERT_TRUE(false);
  }

  std::vector<std::string> GetRecordedDeltaHistogramNames() {
    return recorded_delta_histogram_names_;
  }

 private:
  std::vector<std::string> recorded_delta_histogram_names_;

  DISALLOW_COPY_AND_ASSIGN(HistogramFlattenerDeltaRecorder);
};

class FileMetricsProviderTest : public testing::Test {
 protected:
  FileMetricsProviderTest()
      : task_runner_(new base::TestSimpleTaskRunner()),
        thread_task_runner_handle_(task_runner_),
        prefs_(new TestingPrefServiceSimple) {
    EXPECT_TRUE(temp_dir_.CreateUniqueTempDir());
    FileMetricsProvider::RegisterPrefs(prefs_->registry(), kMetricsName);
  }

  ~FileMetricsProviderTest() override {
    base::GlobalHistogramAllocator::ReleaseForTesting();
  }

  TestingPrefServiceSimple* prefs() { return prefs_.get(); }
  base::FilePath temp_dir() { return temp_dir_.path(); }
  base::FilePath metrics_file() {
    return temp_dir_.path().AppendASCII(kMetricsFilename);
  }

  FileMetricsProvider* provider() {
    if (!provider_)
      provider_.reset(new FileMetricsProvider(task_runner_, prefs()));
    return provider_.get();
  }

  void OnDidCreateMetricsLog() { provider()->OnDidCreateMetricsLog(); }

  void RecordHistogramSnapshots(
      base::HistogramSnapshotManager* snapshot_manager) {
    provider()->RecordHistogramSnapshots(snapshot_manager);
  }

  void RunTasks() {
    task_runner_->RunUntilIdle();
  }

  void WriteMetricsFile(const base::FilePath& path,
                        base::PersistentHistogramAllocator* metrics) {
    base::File writer(path, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
    ASSERT_TRUE(writer.IsValid()) << path.value();
    ASSERT_EQ(static_cast<int>(metrics->used()),
              writer.Write(0, (const char*)metrics->data(), metrics->used()));
  }

  void WriteMetricsFileAtTime(const base::FilePath& path,
                              base::PersistentHistogramAllocator* metrics,
                              base::Time write_time) {
    WriteMetricsFile(path, metrics);
    base::TouchFile(path, write_time, write_time);
  }

  void CreateMetricsFileWithHistograms(int histogram_count) {
    // Get this first so it isn't created inside the persistent allocator.
    base::GlobalHistogramAllocator::GetCreateHistogramResultHistogram();

    base::GlobalHistogramAllocator::CreateWithLocalMemory(
        64 << 10, 0, kMetricsName);

    // Create both sparse and normal histograms in the allocator.
    base::SparseHistogram::FactoryGet("h0", 0)->Add(0);
    for (int i = 1; i < histogram_count; ++i) {
      base::Histogram::FactoryGet(base::StringPrintf("h%d", i), 1, 100, 10, 0)
          ->Add(i);
    }

    std::unique_ptr<base::PersistentHistogramAllocator> histogram_allocator =
        base::GlobalHistogramAllocator::ReleaseForTesting();
    WriteMetricsFile(metrics_file(), histogram_allocator.get());
  }

 private:
  scoped_refptr<base::TestSimpleTaskRunner> task_runner_;
  base::ThreadTaskRunnerHandle thread_task_runner_handle_;

  base::ScopedTempDir temp_dir_;
  std::unique_ptr<TestingPrefServiceSimple> prefs_;
  std::unique_ptr<FileMetricsProvider> provider_;

  DISALLOW_COPY_AND_ASSIGN(FileMetricsProviderTest);
};

TEST_F(FileMetricsProviderTest, AccessMetrics) {
  ASSERT_FALSE(PathExists(metrics_file()));
  CreateMetricsFileWithHistograms(2);

  // Register the file and allow the "checker" task to run.
  ASSERT_TRUE(PathExists(metrics_file()));
  provider()->RegisterSource(metrics_file(),
                             FileMetricsProvider::SOURCE_HISTOGRAMS_ATOMIC_FILE,
                             FileMetricsProvider::ASSOCIATE_CURRENT_RUN,
                             kMetricsName);

  // Record embedded snapshots via snapshot-manager.
  OnDidCreateMetricsLog();
  RunTasks();
  {
    HistogramFlattenerDeltaRecorder flattener;
    base::HistogramSnapshotManager snapshot_manager(&flattener);
    snapshot_manager.StartDeltas();
    RecordHistogramSnapshots(&snapshot_manager);
    snapshot_manager.FinishDeltas();
    EXPECT_EQ(2U, flattener.GetRecordedDeltaHistogramNames().size());
  }

  // Make sure a second call to the snapshot-recorder doesn't break anything.
  {
    HistogramFlattenerDeltaRecorder flattener;
    base::HistogramSnapshotManager snapshot_manager(&flattener);
    snapshot_manager.StartDeltas();
    RecordHistogramSnapshots(&snapshot_manager);
    snapshot_manager.FinishDeltas();
    EXPECT_EQ(0U, flattener.GetRecordedDeltaHistogramNames().size());
  }

  // Second full run on the same file should produce nothing.
  OnDidCreateMetricsLog();
  RunTasks();
  {
    HistogramFlattenerDeltaRecorder flattener;
    base::HistogramSnapshotManager snapshot_manager(&flattener);
    snapshot_manager.StartDeltas();
    RecordHistogramSnapshots(&snapshot_manager);
    snapshot_manager.FinishDeltas();
    EXPECT_EQ(0U, flattener.GetRecordedDeltaHistogramNames().size());
  }

  // Update the time-stamp of the file to indicate that it is "new" and
  // must be recorded.
  {
    base::File touch(metrics_file(),
                     base::File::FLAG_OPEN | base::File::FLAG_WRITE);
    ASSERT_TRUE(touch.IsValid());
    base::Time next = base::Time::Now() + base::TimeDelta::FromSeconds(1);
    touch.SetTimes(next, next);
  }

  // This run should again have "new" histograms.
  OnDidCreateMetricsLog();
  RunTasks();
  {
    HistogramFlattenerDeltaRecorder flattener;
    base::HistogramSnapshotManager snapshot_manager(&flattener);
    snapshot_manager.StartDeltas();
    RecordHistogramSnapshots(&snapshot_manager);
    snapshot_manager.FinishDeltas();
    EXPECT_EQ(2U, flattener.GetRecordedDeltaHistogramNames().size());
  }
}

TEST_F(FileMetricsProviderTest, AccessDirectory) {
  ASSERT_FALSE(PathExists(metrics_file()));

  // Get this first so it isn't created inside the persistent allocator.
  base::GlobalHistogramAllocator::GetCreateHistogramResultHistogram();

  base::GlobalHistogramAllocator::CreateWithLocalMemory(
      64 << 10, 0, kMetricsName);
  base::GlobalHistogramAllocator* allocator =
      base::GlobalHistogramAllocator::Get();
  base::HistogramBase* histogram;

  // Create files starting with a timestamp a few minutes back.
  base::Time base_time = base::Time::Now() - base::TimeDelta::FromMinutes(10);

  // Create some files in an odd order. The files are "touched" back in time to
  // ensure that each file has a later timestamp on disk than the previous one.
  base::ScopedTempDir metrics_files;
  EXPECT_TRUE(metrics_files.CreateUniqueTempDir());
  WriteMetricsFileAtTime(metrics_files.path().AppendASCII(".foo.pma"),
                         allocator, base_time);
  WriteMetricsFileAtTime(metrics_files.path().AppendASCII("_bar.pma"),
                         allocator, base_time);

  histogram = base::Histogram::FactoryGet("h1", 1, 100, 10, 0);
  histogram->Add(1);
  WriteMetricsFileAtTime(metrics_files.path().AppendASCII("a1.pma"), allocator,
                         base_time + base::TimeDelta::FromMinutes(1));

  histogram = base::Histogram::FactoryGet("h2", 1, 100, 10, 0);
  histogram->Add(2);
  WriteMetricsFileAtTime(metrics_files.path().AppendASCII("c2.pma"), allocator,
                         base_time + base::TimeDelta::FromMinutes(2));

  histogram = base::Histogram::FactoryGet("h3", 1, 100, 10, 0);
  histogram->Add(3);
  WriteMetricsFileAtTime(metrics_files.path().AppendASCII("b3.pma"), allocator,
                         base_time + base::TimeDelta::FromMinutes(3));

  histogram = base::Histogram::FactoryGet("h4", 1, 100, 10, 0);
  histogram->Add(3);
  WriteMetricsFileAtTime(metrics_files.path().AppendASCII("d4.pma"), allocator,
                         base_time + base::TimeDelta::FromMinutes(4));

  base::TouchFile(metrics_files.path().AppendASCII("b3.pma"),
                  base_time + base::TimeDelta::FromMinutes(5),
                  base_time + base::TimeDelta::FromMinutes(5));

  WriteMetricsFileAtTime(metrics_files.path().AppendASCII("baz"), allocator,
                         base_time + base::TimeDelta::FromMinutes(6));

  // Register the file and allow the "checker" task to run.
  provider()->RegisterSource(metrics_files.path(),
                             FileMetricsProvider::SOURCE_HISTOGRAMS_ATOMIC_DIR,
                             FileMetricsProvider::ASSOCIATE_CURRENT_RUN,
                             kMetricsName);

  // Files could come out in the order: a1, c2, d4, b3. They are recognizeable
  // by the number of histograms contained within each.
  const uint32_t expect_order[] = {1, 2, 4, 3, 0};
  for (size_t i = 0; i < arraysize(expect_order); ++i) {
    // Record embedded snapshots via snapshot-manager.
    OnDidCreateMetricsLog();
    RunTasks();

    HistogramFlattenerDeltaRecorder flattener;
    base::HistogramSnapshotManager snapshot_manager(&flattener);
    snapshot_manager.StartDeltas();
    RecordHistogramSnapshots(&snapshot_manager);
    snapshot_manager.FinishDeltas();

    EXPECT_EQ(expect_order[i],
              flattener.GetRecordedDeltaHistogramNames().size())
        << i;
  }

  EXPECT_FALSE(base::PathExists(metrics_files.path().AppendASCII("a1.pma")));
  EXPECT_FALSE(base::PathExists(metrics_files.path().AppendASCII("c2.pma")));
  EXPECT_FALSE(base::PathExists(metrics_files.path().AppendASCII("b3.pma")));
  EXPECT_FALSE(base::PathExists(metrics_files.path().AppendASCII("d4.pma")));
  EXPECT_TRUE(base::PathExists(metrics_files.path().AppendASCII(".foo.pma")));
  EXPECT_TRUE(base::PathExists(metrics_files.path().AppendASCII("_bar.pma")));
  EXPECT_TRUE(base::PathExists(metrics_files.path().AppendASCII("baz")));
}

TEST_F(FileMetricsProviderTest, AccessInitialMetrics) {
  ASSERT_FALSE(PathExists(metrics_file()));
  CreateMetricsFileWithHistograms(2);

  // Register the file and allow the "checker" task to run.
  ASSERT_TRUE(PathExists(metrics_file()));
  provider()->RegisterSource(metrics_file(),
                             FileMetricsProvider::SOURCE_HISTOGRAMS_ATOMIC_FILE,
                             FileMetricsProvider::ASSOCIATE_PREVIOUS_RUN,
                             kMetricsName);

  // Record embedded snapshots via snapshot-manager.
  provider()->HasInitialStabilityMetrics();
  {
    HistogramFlattenerDeltaRecorder flattener;
    base::HistogramSnapshotManager snapshot_manager(&flattener);
    snapshot_manager.StartDeltas();
    provider()->RecordInitialHistogramSnapshots(&snapshot_manager);
    snapshot_manager.FinishDeltas();
    EXPECT_EQ(2U, flattener.GetRecordedDeltaHistogramNames().size());
  }

  // Second full run on the same file should produce nothing.
  provider()->OnDidCreateMetricsLog();
  RunTasks();
  {
    HistogramFlattenerDeltaRecorder flattener;
    base::HistogramSnapshotManager snapshot_manager(&flattener);
    snapshot_manager.StartDeltas();
    provider()->RecordInitialHistogramSnapshots(&snapshot_manager);
    snapshot_manager.FinishDeltas();
    EXPECT_EQ(0U, flattener.GetRecordedDeltaHistogramNames().size());
  }

  // A run for normal histograms should produce nothing.
  {
    HistogramFlattenerDeltaRecorder flattener;
    base::HistogramSnapshotManager snapshot_manager(&flattener);
    snapshot_manager.StartDeltas();
    provider()->RecordHistogramSnapshots(&snapshot_manager);
    snapshot_manager.FinishDeltas();
    EXPECT_EQ(0U, flattener.GetRecordedDeltaHistogramNames().size());
  }
}

}  // namespace metrics
