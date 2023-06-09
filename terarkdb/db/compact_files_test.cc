//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "db/db_impl.h"
#include "port/port.h"
#include "rocksdb/db.h"
#include "rocksdb/env.h"
#include "rocksdb/terark_namespace.h"
#include "util/string_util.h"
#include "util/sync_point.h"
#include "util/testharness.h"

namespace TERARKDB_NAMESPACE {

class CompactFilesTest : public testing::Test {
 public:
  CompactFilesTest() {
    env_ = Env::Default();
    db_name_ = test::PerThreadDBPath("compact_files_test");
  }

  std::string db_name_;
  Env* env_;
};

// A class which remembers the name of each flushed file.
class FlushedFileCollector : public EventListener {
 public:
  FlushedFileCollector() {}
  ~FlushedFileCollector() {}

  virtual void OnFlushCompleted(DB* /*db*/, const FlushJobInfo& info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    flushed_files_.push_back(info.file_path);
  }

  std::vector<std::string> GetFlushedFiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    for (auto fname : flushed_files_) {
      result.push_back(fname);
    }
    return result;
  }
  void ClearFlushedFiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    flushed_files_.clear();
  }

 private:
  std::vector<std::string> flushed_files_;
  std::mutex mutex_;
};

TEST_F(CompactFilesTest, L0ConflictsFiles) {
  Options options;
  // to trigger compaction more easily
  const int kWriteBufferSize = 10000;
  const int kLevel0Trigger = 2;
  options.create_if_missing = true;
  options.compaction_style = kCompactionStyleLevel;
  // Small slowdown and stop trigger for experimental purpose.
  options.level0_slowdown_writes_trigger = 20;
  options.level0_stop_writes_trigger = 20;
  options.level0_stop_writes_trigger = 20;
  options.write_buffer_size = kWriteBufferSize;
  options.level0_file_num_compaction_trigger = kLevel0Trigger;
  options.compression = kNoCompression;

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  TERARKDB_NAMESPACE::SyncPoint::GetInstance()->LoadDependency({
      {"CompactFilesImpl:0", "BackgroundCallCompaction:0"},
      {"BackgroundCallCompaction:1", "CompactFilesImpl:1"},
  });
  TERARKDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // create couple files
  // Background compaction starts and waits in BackgroundCallCompaction:0
  for (int i = 0; i < kLevel0Trigger * 4; ++i) {
    db->Put(WriteOptions(), ToString(i), "");
    db->Put(WriteOptions(), ToString(100 - i), "");
    db->Flush(FlushOptions());
  }

  TERARKDB_NAMESPACE::ColumnFamilyMetaData meta;
  db->GetColumnFamilyMetaData(&meta);
  std::string file1;
  for (auto& file : meta.levels[0].files) {
    ASSERT_EQ(0, meta.levels[0].level);
    if (file1 == "") {
      file1 = file.db_path + "/" + file.name;
    } else {
      std::string file2 = file.db_path + "/" + file.name;
      // Another thread starts a compact files and creates an L0 compaction
      // The background compaction then notices that there is an L0 compaction
      // already in progress and doesn't do an L0 compaction
      // Once the background compaction finishes, the compact files finishes
      ASSERT_OK(db->CompactFiles(TERARKDB_NAMESPACE::CompactionOptions(),
                                 {file1, file2}, 0));
      break;
    }
  }
  TERARKDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();
  delete db;
}

TEST_F(CompactFilesTest, ObsoleteFiles) {
  Options options;
  // to trigger compaction more easily
  const int kWriteBufferSize = 65536;
  options.create_if_missing = true;
  // Disable RocksDB background compaction.
  options.compaction_style = kCompactionStyleNone;
  options.level0_slowdown_writes_trigger = (1 << 30);
  options.level0_stop_writes_trigger = (1 << 30);
  options.write_buffer_size = kWriteBufferSize;
  options.max_write_buffer_number = 2;
  options.compression = kNoCompression;
  options.enable_lazy_compaction = false;
  options.blob_size = -1;

  // Add listener
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  // create couple files
  for (int i = 1000; i < 2000; ++i) {
    db->Put(WriteOptions(), ToString(i),
            std::string(kWriteBufferSize / 10, 'a' + (i % 26)));
  }

  auto l0_files = collector->GetFlushedFiles();
  ASSERT_OK(db->CompactFiles(CompactionOptions(), l0_files, 1));
  reinterpret_cast<DBImpl*>(db)->TEST_WaitForCompact();

  // verify all compaction input files are deleted
  for (auto fname : l0_files) {
    ASSERT_EQ(Status::NotFound(), env_->FileExists(fname));
  }
  delete db;
}

TEST_F(CompactFilesTest, NotCutOutputOnLevel0) {
  Options options;
  options.create_if_missing = true;
  // Disable RocksDB background compaction.
  options.compaction_style = kCompactionStyleNone;
  options.level0_slowdown_writes_trigger = 1000;
  options.level0_stop_writes_trigger = 1000;
  options.write_buffer_size = 65536;
  options.max_write_buffer_number = 2;
  options.compression = kNoCompression;
  options.max_compaction_bytes = 5000;
  options.blob_size = -1;

  // Add listener
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  // create couple files
  for (int i = 0; i < 500; ++i) {
    db->Put(WriteOptions(), ToString(i), std::string(1000, 'a' + (i % 26)));
  }
  reinterpret_cast<DBImpl*>(db)->TEST_WaitForFlushMemTable();
  auto l0_files_1 = collector->GetFlushedFiles();
  collector->ClearFlushedFiles();
  for (int i = 0; i < 500; ++i) {
    db->Put(WriteOptions(), ToString(i), std::string(1000, 'a' + (i % 26)));
  }
  reinterpret_cast<DBImpl*>(db)->TEST_WaitForFlushMemTable();
  auto l0_files_2 = collector->GetFlushedFiles();
  ASSERT_OK(db->CompactFiles(CompactionOptions(), l0_files_1, 0));
  ASSERT_OK(db->CompactFiles(CompactionOptions(), l0_files_2, 0));
  // no assertion failure
  delete db;
}

TEST_F(CompactFilesTest, CapturingPendingFiles) {
  Options options;
  options.create_if_missing = true;
  // Disable RocksDB background compaction.
  options.compaction_style = kCompactionStyleNone;
  // Always do full scans for obsolete files (needed to reproduce the issue).
  options.delete_obsolete_files_period_micros = 0;

  // Add listener.
  FlushedFileCollector* collector = new FlushedFileCollector();
  options.listeners.emplace_back(collector);

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  assert(s.ok());
  assert(db);

  // Create 5 files.
  for (int i = 0; i < 5; ++i) {
    db->Put(WriteOptions(), "key" + ToString(i), "value");
    db->Flush(FlushOptions());
  }

  auto l0_files = collector->GetFlushedFiles();
  EXPECT_EQ(5, l0_files.size());

  TERARKDB_NAMESPACE::SyncPoint::GetInstance()->LoadDependency({
      {"CompactFilesImpl:2", "CompactFilesTest.CapturingPendingFiles:0"},
      {"CompactFilesTest.CapturingPendingFiles:1", "CompactFilesImpl:3"},
  });
  TERARKDB_NAMESPACE::SyncPoint::GetInstance()->EnableProcessing();

  // Start compacting files.
  TERARKDB_NAMESPACE::port::Thread compaction_thread(
      [&] { EXPECT_OK(db->CompactFiles(CompactionOptions(), l0_files, 1)); });

  // In the meantime flush another file.
  TEST_SYNC_POINT("CompactFilesTest.CapturingPendingFiles:0");
  db->Put(WriteOptions(), "key5", "value");
  db->Flush(FlushOptions());
  TEST_SYNC_POINT("CompactFilesTest.CapturingPendingFiles:1");

  compaction_thread.join();

  TERARKDB_NAMESPACE::SyncPoint::GetInstance()->DisableProcessing();

  delete db;

  // Make sure we can reopen the DB.
  s = DB::Open(options, db_name_, &db);
  ASSERT_TRUE(s.ok());
  assert(db);
  delete db;
}

TEST_F(CompactFilesTest, CompactionFilterWithGetSv) {
  class FilterWithGet : public CompactionFilter {
   public:
    virtual bool Filter(int /*level*/, const Slice& /*key*/,
                        const Slice& /*value*/, std::string* /*new_value*/,
                        bool* /*value_changed*/) const override {
      if (db_ == nullptr) {
        return true;
      }
      std::string res;
      db_->Get(ReadOptions(), "", &res);
      return true;
    }

    void SetDB(DB* db) { db_ = db; }

    virtual const char* Name() const override { return "FilterWithGet"; }

   private:
    DB* db_;
  };

  std::shared_ptr<FilterWithGet> cf(new FilterWithGet());

  Options options;
  options.create_if_missing = true;
  options.compaction_filter = cf.get();

  DB* db = nullptr;
  DestroyDB(db_name_, options);
  Status s = DB::Open(options, db_name_, &db);
  ASSERT_OK(s);

  cf->SetDB(db);

  // Write one L0 file
  db->Put(WriteOptions(), "K1", "V1");
  db->Flush(FlushOptions());

  // Compact all L0 files using CompactFiles
  TERARKDB_NAMESPACE::ColumnFamilyMetaData meta;
  db->GetColumnFamilyMetaData(&meta);
  for (auto& file : meta.levels[0].files) {
    std::string fname = file.db_path + "/" + file.name;
    ASSERT_OK(
        db->CompactFiles(TERARKDB_NAMESPACE::CompactionOptions(), {fname}, 0));
  }

  delete db;
}

TEST_F(CompactFilesTest, SentinelCompressionType) {
  if (!Zlib_Supported()) {
    fprintf(stderr, "zlib compression not supported, skip this test\n");
    return;
  }
  if (!Snappy_Supported()) {
    fprintf(stderr, "snappy compression not supported, skip this test\n");
    return;
  }
  // Check that passing `CompressionType::kDisableCompressionOption` to
  // `CompactFiles` causes it to use the column family compression options.
  for (auto compaction_style : {CompactionStyle::kCompactionStyleLevel,
                                CompactionStyle::kCompactionStyleUniversal,
                                CompactionStyle::kCompactionStyleNone}) {
    DestroyDB(db_name_, Options());
    Options options;
    options.compaction_style = compaction_style;
    // L0: Snappy, L1: ZSTD, L2: Snappy
    options.compression_per_level = {CompressionType::kSnappyCompression,
                                     CompressionType::kZlibCompression,
                                     CompressionType::kSnappyCompression};
    options.create_if_missing = true;
    FlushedFileCollector* collector = new FlushedFileCollector();
    options.listeners.emplace_back(collector);
    DB* db = nullptr;
    ASSERT_OK(DB::Open(options, db_name_, &db));

    db->Put(WriteOptions(), "key", "val");
    db->Flush(FlushOptions());

    auto l0_files = collector->GetFlushedFiles();
    ASSERT_EQ(1, l0_files.size());

    // L0->L1 compaction, so output should be ZSTD-compressed
    CompactionOptions compaction_opts;
    compaction_opts.compression = CompressionType::kDisableCompressionOption;
    ASSERT_OK(db->CompactFiles(compaction_opts, l0_files, 1));

    std::unique_ptr<TERARKDB_NAMESPACE::TablePropertiesCollectionIterator>
        all_tables_props_iter(db->NewPropertiesOfAllTablesIterator());
    ASSERT_OK(all_tables_props_iter->status());
    for (all_tables_props_iter->SeekToFirst(); all_tables_props_iter->Valid();
         all_tables_props_iter->Next()) {
      ASSERT_EQ(CompressionTypeToString(CompressionType::kZlibCompression),
                all_tables_props_iter->properties()->compression_name);
    }
    all_tables_props_iter.reset();
    delete db;
  }
}

}  // namespace TERARKDB_NAMESPACE

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#else
#include <stdio.h>

int main(int /*argc*/, char** /*argv*/) {
  fprintf(stderr,
          "SKIPPED as DBImpl::CompactFiles is not supported in ROCKSDB_LITE\n");
  return 0;
}

#endif  // !ROCKSDB_LITE
