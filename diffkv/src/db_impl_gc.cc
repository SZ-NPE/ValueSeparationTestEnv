#include "db_impl.h"

#include "test_util/sync_point.h"

#include <iostream>
#include "atomic"
#include "blob_file_iterator.h"
#include "blob_gc_job.h"
#include "blob_gc_picker.h"

// std::atomic<uint64_t> gc_total{0};

namespace rocksdb {
namespace titandb {

void TitanDBImpl::MaybeScheduleGC() {
  mutex_.AssertHeld();

  if (db_options_.disable_background_gc) return;

  if (shuting_down_.load(std::memory_order_acquire)) return;

  while (unscheduled_gc_ > 0 &&
         bg_gc_scheduled_ < db_options_.max_background_gc) {
    unscheduled_gc_--;
    bg_gc_scheduled_++;
    env_->Schedule(&TitanDBImpl::BGWorkGC, this, Env::Priority::USER, this);
  }
}

void TitanDBImpl::BGWorkGC(void* db) {
  reinterpret_cast<TitanDBImpl*>(db)->BackgroundCallGC();
}

void TitanDBImpl::BackgroundCallGC() {
  TEST_SYNC_POINT("TitanDBImpl::BackgroundCallGC:BeforeGCRunning");
  {
    MutexLock l(&mutex_);
    assert(bg_gc_scheduled_ > 0);
    while (drop_cf_requests_ > 0) {
      bg_cv_.Wait();
    }
    bg_gc_running_++;

    TEST_SYNC_POINT("TitanDBImpl::BackgroundCallGC:BeforeBackgroundGC");
    if (!gc_queue_.empty()) {
      uint32_t column_family_id = PopFirstFromGCQueue();
      LogBuffer log_buffer(InfoLogLevel::INFO_LEVEL,
                           db_options_.info_log.get());
      BackgroundGC(&log_buffer, column_family_id);
      {
        mutex_.Unlock();
        log_buffer.FlushBufferToLog();
        LogFlush(db_options_.info_log.get());
        mutex_.Lock();
      }
    }

    bg_gc_running_--;
    bg_gc_scheduled_--;
    MaybeScheduleGC();
    if (bg_gc_scheduled_ == 0 || bg_gc_running_ == 0) {
      // Signal DB destructor if bg_gc_scheduled_ drop to 0.
      // Signal drop CF requests_ if bg_gc_running_ drop to 0.
      // If none of this is true, there is no need to signal since nobody is
      // waiting for it.
      bg_cv_.SignalAll();
    }
    // IMPORTANT: there should be no code after calling SignalAll. This call may
    // signal the DB destructor that it's OK to proceed with destruction. In
    // that case, all DB variables will be deallocated and referencing them
    // will cause trouble.
  }
}

Status TitanDBImpl::BackgroundGC(LogBuffer* log_buffer,
                                 uint32_t column_family_id) {
  mutex_.AssertHeld();
  uint64_t gc_time = 0;
  StopWatch gc_sw(env_, stats_.get(), BLOB_DB_GC_MICROS);

  std::unique_ptr<BlobGC> blob_gc;
  std::unique_ptr<ColumnFamilyHandle> cfh;
  Status s;
  {
    std::shared_ptr<BlobStorage> blob_storage;
    // Skip CFs that have been dropped.
    if (!blob_file_set_->IsColumnFamilyObsolete(column_family_id)) {
      blob_storage = blob_file_set_->GetBlobStorage(column_family_id).lock();
    } else {
      TEST_SYNC_POINT_CALLBACK("TitanDBImpl::BackgroundGC:CFDropped", nullptr);
      ROCKS_LOG_BUFFER(log_buffer, "GC skip dropped colum family [%s].",
                       cf_info_[column_family_id].name.c_str());
    }
    if (blob_storage != nullptr) {
      const auto& cf_options = blob_storage->cf_options();
      std::shared_ptr<BlobGCPicker> blob_gc_picker =
          std::make_shared<BasicBlobGCPicker>(db_options_, cf_options,
                                              stats_.get());
      blob_gc = blob_gc_picker->PickBlobGC(blob_storage.get());

      if (blob_gc) {
        cfh = db_impl_->GetColumnFamilyHandleUnlocked(column_family_id);
        assert(column_family_id == cfh->GetID());
        blob_gc->SetColumnFamily(cfh.get());
      }
    }

    // TODO(@DorianZheng) Make sure enough room for GC

    if (UNLIKELY(!blob_gc)) {
      // Nothing to do
      ROCKS_LOG_BUFFER(log_buffer, "Titan GC nothing to do");
    } else {
      BlobGCJob blob_gc_job(blob_gc.get(), db_, &mutex_, db_options_, env_,
                            env_options_, blob_manager_.get(),
                            blob_file_set_.get(), log_buffer, &shuting_down_,
                            stats_.get(), &builders_[column_family_id]);
      s = blob_gc_job.Prepare();
      if (s.ok()) {
        // std::cerr<<"run gc"<<std::endl;
        mutex_.Unlock();
        {
        TitanStopWatch sw(env_, gc_time);
        s = blob_gc_job.Run();
        }
        mutex_.Lock();
      }
      if (s.ok()) {
        TitanStopWatch sw(env_, gc_time);
        s = blob_gc_job.Finish();
      }
      blob_gc->ReleaseGcFiles();
      
      uint64_t total_size = 0;
      uint64_t live_size = 0;
      GetIntProperty("rocksdb.titandb.live-blob-file-size",&total_size);
      GetIntProperty("rocksdb.titandb.live-blob-size",&live_size);
      if(block_for_size_.load()&&total_size<db_options_.block_write_size){
        MutexLock l(&size_mutex_);
        block_for_size_.store(false);
        size_cv_.SignalAll();
      }

      auto cf_options = blob_storage->cf_options();
      bool wisc_gc = !cf_options.level_merge && total_size> (uint64_t) (100+100*cf_options.blob_file_discardable_ratio)<<30;
      // bool wisc_gc = !cf_options.level_merge && total_size>live_size && (double)(total_size-live_size)/total_size > blob_storage->cf_options().blob_file_discardable_ratio;

      bool bg_gc = (bg_gc_scheduled_ - 1 + gc_queue_.size() <
           2 * static_cast<uint32_t>(db_options_.max_background_gc)) && (block_for_size_.load() || blob_gc->trigger_next());

      if (bg_gc || wisc_gc) {
        // RecordTick(stats_.get(), TitanStats::GC_TRIGGER_NEXT, 1);
        // There is still data remained to be GCed
        // and the queue is not overwhelmed
        // then put this cf to GC queue for next GC
        
        // blob_storage->ComputeGCScore();
        AddToGCQueue(blob_gc->column_family_handle()->GetID());
      }
    }

    if (s.ok()) {
      // RecordTick(stats_.get(), TitanStats::GC_SUCCESS, 1);
      // Done
    } else {
      SetBGError(s);
      // RecordTick(stats_.get(), TitanStats::GC_FAIL, 1);
      ROCKS_LOG_WARN(db_options_.info_log, "Titan GC error: %s",
                     s.ToString().c_str());
    }

    TEST_SYNC_POINT("TitanDBImpl::BackgroundGC:Finish");
  }
  // gc_total += gc_time;
  return s;
}

Status TitanDBImpl::TEST_StartGC(uint32_t column_family_id) {
  // BackgroundCallGC
  Status s;
  LogBuffer log_buffer(InfoLogLevel::INFO_LEVEL, db_options_.info_log.get());
  {
    MutexLock l(&mutex_);
    // Prevent CF being dropped while GC is running.
    while (drop_cf_requests_ > 0) {
      bg_cv_.Wait();
    }
    bg_gc_running_++;
    bg_gc_scheduled_++;

    s = BackgroundGC(&log_buffer, column_family_id);

    {
      mutex_.Unlock();
      log_buffer.FlushBufferToLog();
      LogFlush(db_options_.info_log.get());
      mutex_.Lock();
    }

    bg_gc_running_--;
    bg_gc_scheduled_--;
    if (bg_gc_scheduled_ == 0 || bg_gc_running_ == 0) {
      bg_cv_.SignalAll();
    }
  }
  return s;
}

}  // namespace titandb
}  // namespace rocksdb
