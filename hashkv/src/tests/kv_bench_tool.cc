//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifdef NUMA
#include <numa.h>
#endif
#ifndef OS_WIN
#include <unistd.h>
#endif
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <mach/host_info.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#endif
#ifdef __FreeBSD__
#include <sys/sysctl.h>
#endif
#include <gflags/gflags.h>

#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <thread>
#include <unordered_map>

// #include "define.hh"
#include "kvServer.hh"
// #include "leveldb/db.h"
// #include "leveldb/env.h"
// #include "leveldb/slice.h"
#include "util/mutexlock.h"
#include "util/random.h"
#include "util/slice.h"
#include "util/histogram.h"

#ifdef OS_WIN
#include <io.h>  // open/close
#endif

using GFLAGS_NAMESPACE::ParseCommandLineFlags;
using GFLAGS_NAMESPACE::RegisterFlagValidator;
using GFLAGS_NAMESPACE::SetUsageMessage;
using GFLAGS_NAMESPACE::SetVersionString;

DEFINE_string(
    benchmarks,
    "fillseq,"
    "fillseqdeterministic,"
    "fillsync,"
    "fillrandom,"
    "filluniquerandomdeterministic,"
    "overwrite,"
    "readrandom,"
    "newiterator,"
    "newiteratorwhilewriting,"
    "seekrandom,"
    "seekrandomwhilewriting,"
    "seekrandomwhilemerging,"
    "readseq,"
    "readreverse,"
    "compact,"
    "compactall,"
    "flush,"
    "compact0,"
    "compact1,"
    "waitforcompaction,"
    "multireadrandom,"
    "mixgraph,"
    "readseq,"
    "readtorowcache,"
    "readtocache,"
    "readreverse,"
    "readwhilewriting,"
    "readwhilemerging,"
    "readwhilescanning,"
    "readrandomwriterandom,"
    "updaterandom,"
    "xorupdaterandom,"
    "approximatesizerandom,"
    "randomwithverify,"
    "fill100K,"
    "crc32c,"
    "xxhash,"
    "xxhash64,"
    "xxh3,"
    "compress,"
    "uncompress,"
    "acquireload,"
    "fillseekseq,"
    "randomtransaction,"
    "randomreplacekeys,"
    "timeseries,"
    "getmergeoperands,",
    "readrandomoperands,"
    "backup,"
    "restore"

    "Comma-separated list of operations to run in the specified"
    " order. Available benchmarks:\n"
    "\tfillseq       -- write N values in sequential key"
    " order in async mode\n"
    "\tfillseqdeterministic       -- write N values in the specified"
    " key order and keep the shape of the LSM tree\n"
    "\tfillrandom    -- write N values in random key order in async"
    " mode\n"
    "\tfilluniquerandomdeterministic       -- write N values in a random"
    " key order and keep the shape of the LSM tree\n"
    "\toverwrite     -- overwrite N values in random key order in "
    "async mode\n"
    "\tfillsync      -- write N/1000 values in random key order in "
    "sync mode\n"
    "\tfill100K      -- write N/1000 100K values in random order in"
    " async mode\n"
    "\tdeleteseq     -- delete N keys in sequential order\n"
    "\tdeleterandom  -- delete N keys in random order\n"
    "\treadseq       -- read N times sequentially\n"
    "\treadtocache   -- 1 thread reading database sequentially\n"
    "\treadreverse   -- read N times in reverse order\n"
    "\treadrandom    -- read N times in random order\n"
    "\treadmissing   -- read N missing keys in random order\n"
    "\treadwhilewriting      -- 1 writer, N threads doing random "
    "reads\n"
    "\treadwhilemerging      -- 1 merger, N threads doing random "
    "reads\n"
    "\treadwhilescanning     -- 1 thread doing full table scan, "
    "N threads doing random reads\n"
    "\treadrandomwriterandom -- N threads doing random-read, "
    "random-write\n"
    "\tupdaterandom  -- N threads doing read-modify-write for random "
    "keys\n"
    "\txorupdaterandom  -- N threads doing read-XOR-write for "
    "random keys\n"
    "\tappendrandom  -- N threads doing read-modify-write with "
    "growing values\n"
    "\tmergerandom   -- same as updaterandom/appendrandom using merge"
    " operator. "
    "Must be used with merge_operator\n"
    "\treadrandommergerandom -- perform N random read-or-merge "
    "operations. Must be used with merge_operator\n"
    "\tnewiterator   -- repeated iterator creation\n"
    "\tseekrandom    -- N random seeks, call Next seek_nexts times "
    "per seek\n"
    "\tseekrandomwhilewriting -- seekrandom and 1 thread doing "
    "overwrite\n"
    "\tseekrandomwhilemerging -- seekrandom and 1 thread doing "
    "merge\n"
    "\tcrc32c        -- repeated crc32c of <block size> data\n"
    "\txxhash        -- repeated xxHash of <block size> data\n"
    "\txxhash64      -- repeated xxHash64 of <block size> data\n"
    "\txxh3          -- repeated XXH3 of <block size> data\n"
    "\tacquireload   -- load N*1000 times\n"
    "\tfillseekseq   -- write N values in sequential key, then read "
    "them by seeking to each key\n"
    "\trandomtransaction     -- execute N random transactions and "
    "verify correctness\n"
    "\trandomreplacekeys     -- randomly replaces N keys by deleting "
    "the old version and putting the new version\n\n"
    "\ttimeseries            -- 1 writer generates time series data "
    "and multiple readers doing random reads on id\n\n"
    "Meta operations:\n"
    "\tcompact     -- Compact the entire DB; If multiple, randomly choose one\n"
    "\tcompactall  -- Compact the entire DB\n"
    "\tcompact0  -- compact L0 into L1\n"
    "\tcompact1  -- compact L1 into L2\n"
    "\twaitforcompaction - pause until compaction is (probably) done\n"
    "\tflush - flush the memtable\n"
    "\tstats       -- Print DB stats\n"
    "\tresetstats  -- Reset DB stats\n"
    "\tlevelstats  -- Print the number of files and bytes per level\n"
    "\tmemstats  -- Print memtable stats\n"
    "\tsstables    -- Print sstable info\n"
    "\theapprofile -- Dump a heap profile (if supported by this port)\n"
    "\treplay      -- replay the trace file specified with trace_file\n"
    "\tgetmergeoperands -- Insert lots of merge records which are a list of "
    "sorted ints for a key and then compare performance of lookup for another "
    "key by doing a Get followed by binary searching in the large sorted list "
    "vs doing a GetMergeOperands and binary searching in the operands which "
    "are sorted sub-lists. The MergeOperator used is sortlist.h\n"
    "\treadrandomoperands -- read random keys using `GetMergeOperands()`. An "
    "operation includes a rare but possible retry in case it got "
    "`Status::Incomplete()`. This happens upon encountering more keys than "
    "have ever been seen by the thread (or eight initially)\n"
    "\tbackup --  Create a backup of the current DB and verify that a new "
    "backup is corrected. "
    "Rate limit can be specified through --backup_rate_limit\n"
    "\trestore -- Restore the DB from the latest backup available, rate limit "
    "can be specified through --restore_rate_limit\n");

// DEFINE_int64(disk_size, 1024*1024*1024, "hash kv disk size");

DEFINE_int64(num, 1000000, "Number of key/values to place in database");

// DEFINE_int64(numdistinct, 1000,
//              "Number of distinct keys to use. Used in RandomWithVerify to "
//              "read/write on fewer keys so that gets are more likely to find
//              the" " key and puts are more likely to update the same key");

// DEFINE_int64(merge_keys, -1,
//              "Number of distinct keys to use for MergeRandom and "
//              "ReadRandomMergeRandom. "
//              "If negative, there will be FLAGS_num keys.");
// DEFINE_int32(num_column_families, 1, "Number of Column Families to use.");

// DEFINE_int32(
//     num_hot_column_families, 0,
//     "Number of Hot Column Families. If more than 0, only write to this "
//     "number of column families. After finishing all the writes to them, "
//     "create new set of column families and insert to them. Only used "
//     "when num_column_families > 1.");

// DEFINE_string(column_family_distribution, "",
//               "Comma-separated list of percentages, where the ith element "
//               "indicates the probability of an op using the ith column
//               family. " "The number of elements must be
//               `num_hot_column_families` if " "specified; otherwise, it must
//               be `num_column_families`. The " "sum of elements must be 100.
//               E.g., if `num_column_families=4`, " "and
//               `num_hot_column_families=0`, a valid list could be "
//               "\"10,20,30,40\".");

DEFINE_int64(reads, -1,
             "Number of read operations to do.  "
             "If negative, do FLAGS_num reads.");

DEFINE_int64(deletes, -1,
             "Number of delete operations to do.  "
             "If negative, do FLAGS_num deletions.");

// DEFINE_int32(bloom_locality, 0, "Control bloom filter probes locality");

DEFINE_int64(seed, 0,
             "Seed base for random number generators. "
             "When 0 it is derived from the current time.");
static std::optional<int64_t> seed_base;

DEFINE_int32(threads, 1, "Number of concurrent threads to run.");

DEFINE_int32(duration, 0,
             "Time in seconds for the random-ops tests to run."
             " When 0 then num & reads determine the test duration");

DEFINE_string(value_size_distribution_type, "fixed",
              "Value size distribution type: fixed, uniform, normal");

DEFINE_int32(value_size, 100, "Size of each value in fixed distribution");
static unsigned int value_size = 100;

DEFINE_int32(value_size_min, 100, "Min size of random value");

DEFINE_int32(value_size_max, 102400, "Max size of random value");

// DEFINE_int32(seek_nexts, 0,
//              "How many times to call Next() after Seek() in "
//              "fillseekseq, seekrandom, seekrandomwhilewriting and "
//              "seekrandomwhilemerging");

// DEFINE_bool(reverse_iterator, false,
//             "When true use Prev rather than Next for iterators that do "
//             "Seek and then Next");

// DEFINE_bool(auto_prefix_mode, false, "Set auto_prefix_mode for seek
// benchmark");

// DEFINE_int64(max_scan_distance, 0,
//              "Used to define iterate_upper_bound (or iterate_lower_bound "
//              "if FLAGS_reverse_iterator is set to true) when value is
//              nonzero");

// DEFINE_bool(use_uint64_comparator, false, "use Uint64 user comparator");

// DEFINE_int64(batch_size, 1, "Batch size");

// static bool ValidateKeySize(const char* /*flagname*/, int32_t /*value*/) {
//   return true;
// }

// static bool ValidateUint32Range(const char* flagname, uint64_t value) {
//   if (value > std::numeric_limits<uint32_t>::max()) {
//     fprintf(stderr, "Invalid value for --%s: %lu, overflow\n", flagname,
//             (unsigned long)value);
//     return false;
//   }
//   return true;
// }

DEFINE_int32(key_size, 16, "size of each key");

// DEFINE_int32(user_timestamp_size, 0,
//              "number of bytes in a user-defined timestamp");

// DEFINE_int32(num_multi_db, 0,
//              "Number of DBs used in the benchmark. 0 means single DB.");

DEFINE_double(compression_ratio, 0.5,
              "Arrange to generate values that shrink to this fraction of "
              "their original size after compression");

DEFINE_double(
    overwrite_probability, 0.0,
    "Used in 'filluniquerandom' benchmark: for each write operation, "
    "we give a probability to perform an overwrite instead. The key used for "
    "the overwrite is randomly chosen from the last 'overwrite_window_size' "
    "keys previously inserted into the DB. "
    "Valid overwrite_probability values: [0.0, 1.0].");

DEFINE_uint32(overwrite_window_size, 1,
              "Used in 'filluniquerandom' benchmark. For each write operation,"
              " when the overwrite_probability flag is set by the user, the "
              "key used to perform an overwrite is randomly chosen from the "
              "last 'overwrite_window_size' keys previously inserted into DB. "
              "Warning: large values can affect throughput. "
              "Valid overwrite_window_size values: [1, kMaxUint32].");

// DEFINE_uint64(
//     disposable_entries_delete_delay, 0,
//     "Minimum delay in microseconds for the series of Deletes "
//     "to be issued. When 0 the insertion of the last disposable entry is "
//     "immediately followed by the issuance of the Deletes. "
//     "(only compatible with fillanddeleteuniquerandom benchmark).");

// DEFINE_uint64(disposable_entries_batch_size, 0,
//               "Number of consecutively inserted disposable KV entries "
//               "that will be deleted after 'delete_delay' microseconds. "
//               "A series of Deletes is always issued once all the "
//               "disposable KV entries it targets have been inserted "
//               "into the DB. When 0 no deletes are issued and a "
//               "regular 'filluniquerandom' benchmark occurs. "
//               "(only compatible with fillanddeleteuniquerandom benchmark)");

// DEFINE_int32(disposable_entries_value_size, 64,
//              "Size of the values (in bytes) of the entries targeted by "
//              "selective deletes. "
//              "(only compatible with fillanddeleteuniquerandom benchmark)");

// DEFINE_uint64(
//     persistent_entries_batch_size, 0,
//     "Number of KV entries being inserted right before the deletes "
//     "targeting the disposable KV entries are issued. These "
//     "persistent keys are not targeted by the deletes, and will always "
//     "remain valid in the DB. (only compatible with "
//     "--benchmarks='fillanddeleteuniquerandom' "
//     "and used when--disposable_entries_batch_size is > 0).");

// DEFINE_int32(persistent_entries_value_size, 64,
//              "Size of the values (in bytes) of the entries not targeted by "
//              "deletes. (only compatible with "
//              "--benchmarks='fillanddeleteuniquerandom' "
//              "and used when--disposable_entries_batch_size is > 0).");


DEFINE_double(read_random_exp_range, 0.0,
              "Read random's key will be generated using distribution of "
              "num * exp(-r) where r is uniform number from 0 to this value."
              "The larger the number is, the more skewed the reads are. "
              "Only used in readrandom and multireadrandom benchmarks.");

DEFINE_bool(histogram, false, "Print histogram of operation timings");

DEFINE_bool(confidence_interval_only, false,
            "Print 95% confidence interval upper and lower bounds only for "
            "aggregate stats.");

// DEFINE_bool(enable_numa, false,
//             "Make operations aware of NUMA architecture and bind memory "
//             "and cpus corresponding to nodes together. In NUMA, memory "
//             "in same node as CPUs are closer when compared to memory in "
//             "other nodes. Reads can be faster when the process is bound to "
//             "CPU and memory of same node. Use \"$numactl --hardware\" command
//             " "to see NUMA memory architecture.");

// DEFINE_int64(db_write_buffer_size,
//              ROCKSDB_NAMESPACE::Options().db_write_buffer_size,
//              "Number of bytes to buffer in all memtables before compacting");

// DEFINE_bool(cost_write_buffer_to_cache, false,
//             "The usage of memtable is costed to the block cache");

// DEFINE_int64(arena_block_size, ROCKSDB_NAMESPACE::Options().arena_block_size,
//              "The size, in bytes, of one block in arena memory allocation.");

// DEFINE_int64(write_buffer_size,
// ROCKSDB_NAMESPACE::Options().write_buffer_size,
//              "Number of bytes to buffer in memtable before compacting");

// DEFINE_int32(max_write_buffer_number,
//              ROCKSDB_NAMESPACE::Options().max_write_buffer_number,
//              "The number of in-memory memtables. Each memtable is of size"
//              " write_buffer_size bytes.");

// DEFINE_int32(min_write_buffer_number_to_merge,
//              ROCKSDB_NAMESPACE::Options().min_write_buffer_number_to_merge,
//              "The minimum number of write buffers that will be merged
//              together" "before writing to storage. This is cheap because it
//              is an" "in-memory merge. If this feature is not enabled, then
//              all these" "write buffers are flushed to L0 as separate files
//              and this " "increases read amplification because a get request
//              has to check" " in all of these files. Also, an in-memory merge
//              may result in" " writing less data to storage if there are
//              duplicate records " " in each of these individual write
//              buffers.");

// DEFINE_int32(max_write_buffer_number_to_maintain,
//              ROCKSDB_NAMESPACE::Options().max_write_buffer_number_to_maintain,
//              "The total maximum number of write buffers to maintain in memory
//              " "including copies of buffers that have already been flushed. "
//              "Unlike max_write_buffer_number, this parameter does not affect
//              " "flushing. This controls the minimum amount of write history "
//              "that will be available in memory for conflict checking when "
//              "Transactions are used. If this value is too low, some "
//              "transactions may fail at commit time due to not being able to "
//              "determine whether there were any write conflicts. Setting this
//              " "value to 0 will cause write buffers to be freed immediately "
//              "after they are flushed.  If this value is set to -1, "
//              "'max_write_buffer_number' will be used.");

// DEFINE_int64(max_write_buffer_size_to_maintain,
//              ROCKSDB_NAMESPACE::Options().max_write_buffer_size_to_maintain,
//              "The total maximum size of write buffers to maintain in memory "
//              "including copies of buffers that have already been flushed. "
//              "Unlike max_write_buffer_number, this parameter does not affect
//              " "flushing. This controls the minimum amount of write history "
//              "that will be available in memory for conflict checking when "
//              "Transactions are used. If this value is too low, some "
//              "transactions may fail at commit time due to not being able to "
//              "determine whether there were any write conflicts. Setting this
//              " "value to 0 will cause write buffers to be freed immediately "
//              "after they are flushed.  If this value is set to -1, "
//              "'max_write_buffer_number' will be used.");

// DEFINE_int32(max_background_jobs,
//              ROCKSDB_NAMESPACE::Options().max_background_jobs,
//              "The maximum number of concurrent background jobs that can occur
//              " "in parallel.");

// DEFINE_int32(num_bottom_pri_threads, 0,
//              "The number of threads in the bottom-priority thread pool (used
//              " "by universal compaction only).");

// DEFINE_int32(num_high_pri_threads, 0,
//              "The maximum number of concurrent background compactions"
//              " that can occur in parallel.");

// DEFINE_int32(num_low_pri_threads, 0,
//              "The maximum number of concurrent background compactions"
//              " that can occur in parallel.");

// DEFINE_int32(max_background_compactions,
//              ROCKSDB_NAMESPACE::Options().max_background_compactions,
//              "The maximum number of concurrent background compactions"
//              " that can occur in parallel.");

// DEFINE_uint64(subcompactions, 1,
//               "For CompactRange, set max_subcompactions for each compaction "
//               "job in this CompactRange, for auto compactions, this is "
//               "Maximum number of subcompactions to divide L0-L1 compactions "
//               "into.");
// static const bool FLAGS_subcompactions_dummy __attribute__((__unused__)) =
//     RegisterFlagValidator(&FLAGS_subcompactions, &ValidateUint32Range);

// DEFINE_int32(max_background_flushes,
//              ROCKSDB_NAMESPACE::Options().max_background_flushes,
//              "The maximum number of concurrent background flushes"
//              " that can occur in parallel.");

// static ROCKSDB_NAMESPACE::CompactionStyle FLAGS_compaction_style_e;
// DEFINE_int32(compaction_style,
//              (int32_t)ROCKSDB_NAMESPACE::Options().compaction_style,
//              "style of compaction: level-based, universal and fifo");

// static ROCKSDB_NAMESPACE::CompactionPri FLAGS_compaction_pri_e;
// DEFINE_int32(compaction_pri,
//              (int32_t)ROCKSDB_NAMESPACE::Options().compaction_pri,
//              "priority of files to compaction: by size or by data age");

// DEFINE_int32(universal_size_ratio, 0,
//              "Percentage flexibility while comparing file size "
//              "(for universal compaction only).");

// DEFINE_int32(universal_min_merge_width, 0,
//              "The minimum number of files in a single compaction run "
//              "(for universal compaction only).");

// DEFINE_int32(universal_max_merge_width, 0,
//              "The max number of files to compact in universal style "
//              "compaction");

// DEFINE_int32(universal_max_size_amplification_percent, 0,
//              "The max size amplification for universal style compaction");

// DEFINE_int32(universal_compression_size_percent, -1,
//              "The percentage of the database to compress for universal "
//              "compaction. -1 means compress everything.");

// DEFINE_bool(universal_allow_trivial_move, false,
//             "Allow trivial move in universal compaction.");

// DEFINE_bool(universal_incremental, false,
//             "Enable incremental compactions in universal compaction.");

// DEFINE_int64(cache_size, 32 << 20,  // 32MB
//              "Number of bytes to use as a cache of uncompressed data");

// DEFINE_int32(cache_numshardbits, -1,
//              "Number of shards for the block cache"
//              " is 2 ** cache_numshardbits. Negative means use default
//              settings." " This is applied only if FLAGS_cache_size is
//              non-negative.");

// DEFINE_double(cache_high_pri_pool_ratio, 0.0,
//               "Ratio of block cache reserve for high pri blocks. "
//               "If > 0.0, we also enable "
//               "cache_index_and_filter_blocks_with_high_priority.");

// DEFINE_double(cache_low_pri_pool_ratio, 0.0,
//               "Ratio of block cache reserve for low pri blocks.");

// DEFINE_string(cache_type, "lru_cache", "Type of block cache.");

// DEFINE_bool(use_compressed_secondary_cache, false,
//             "Use the CompressedSecondaryCache as the secondary cache.");

// DEFINE_int64(compressed_secondary_cache_size, 32 << 20,  // 32MB
//              "Number of bytes to use as a cache of data");

// DEFINE_int32(compressed_secondary_cache_numshardbits, 6,
//              "Number of shards for the block cache"
//              " is 2 ** compressed_secondary_cache_numshardbits."
//              " Negative means use default settings."
//              " This is applied only if FLAGS_cache_size is non-negative.");

// DEFINE_double(compressed_secondary_cache_high_pri_pool_ratio, 0.0,
//               "Ratio of block cache reserve for high pri blocks. "
//               "If > 0.0, we also enable "
//               "cache_index_and_filter_blocks_with_high_priority.");

// DEFINE_double(compressed_secondary_cache_low_pri_pool_ratio, 0.0,
//               "Ratio of block cache reserve for low pri blocks.");

// DEFINE_string(compressed_secondary_cache_compression_type, "lz4",
//               "The compression algorithm to use for large "
//               "values stored in CompressedSecondaryCache.");
// static enum ROCKSDB_NAMESPACE::CompressionType
//     FLAGS_compressed_secondary_cache_compression_type_e =
//         ROCKSDB_NAMESPACE::kLZ4Compression;

// DEFINE_uint32(
//     compressed_secondary_cache_compress_format_version, 2,
//     "compress_format_version can have two values: "
//     "compress_format_version == 1 -- decompressed size is not included"
//     " in the block header."
//     "compress_format_version == 2 -- decompressed size is included"
//     " in the block header in varint32 format.");

// DEFINE_int64(simcache_size, -1,
//              "Number of bytes to use as a simcache of "
//              "uncompressed data. Nagative value disables simcache.");

// DEFINE_bool(cache_index_and_filter_blocks, false,
//             "Cache index/filter blocks in block cache.");

// DEFINE_bool(use_cache_jemalloc_no_dump_allocator, false,
//             "Use JemallocNodumpAllocator for block/blob cache.");

// DEFINE_bool(use_cache_memkind_kmem_allocator, false,
//             "Use memkind kmem allocator for block/blob cache.");

// DEFINE_bool(partition_index_and_filters, false,
//             "Partition index and filter blocks.");

// DEFINE_bool(partition_index, false, "Partition index blocks");

// DEFINE_bool(index_with_first_key, false, "Include first key in the index");

// DEFINE_bool(
//     optimize_filters_for_memory,
//     ROCKSDB_NAMESPACE::BlockBasedTableOptions().optimize_filters_for_memory,
//     "Minimize memory footprint of filters");

// DEFINE_int64(
//     index_shortening_mode, 2,
//     "mode to shorten index: 0 for no shortening; 1 for only shortening "
//     "separaters; 2 for shortening shortening and successor");

// DEFINE_int64(metadata_block_size,
//              ROCKSDB_NAMESPACE::BlockBasedTableOptions().metadata_block_size,
//              "Max partition size when partitioning index/filters");

// The default reduces the overhead of reading time with flash. With HDD, which
// offers much less throughput, however, this number better to be set to 1.
DEFINE_int32(ops_between_duration_checks, 1000,
             "Check duration limit every x ops");

// DEFINE_bool(pin_l0_filter_and_index_blocks_in_cache, false,
//             "Pin index/filter blocks of L0 files in block cache.");

// DEFINE_bool(
//     pin_top_level_index_and_filter, false,
//     "Pin top-level index of partitioned index/filter blocks in block
//     cache.");

// DEFINE_int32(block_size,
//              static_cast<int32_t>(
//                  ROCKSDB_NAMESPACE::BlockBasedTableOptions().block_size),
//              "Number of bytes in a block.");

// DEFINE_int32(format_version,
//              static_cast<int32_t>(
//                  ROCKSDB_NAMESPACE::BlockBasedTableOptions().format_version),
//              "Format version of SST files.");

// DEFINE_int32(block_restart_interval,
//              ROCKSDB_NAMESPACE::BlockBasedTableOptions().block_restart_interval,
//              "Number of keys between restart points "
//              "for delta encoding of keys in data block.");

// DEFINE_int32(
//     index_block_restart_interval,
//     ROCKSDB_NAMESPACE::BlockBasedTableOptions().index_block_restart_interval,
//     "Number of keys between restart points "
//     "for delta encoding of keys in index block.");

// DEFINE_int32(read_amp_bytes_per_bit,
//              ROCKSDB_NAMESPACE::BlockBasedTableOptions().read_amp_bytes_per_bit,
//              "Number of bytes per bit to be used in block read-amp bitmap");

// DEFINE_bool(
//     enable_index_compression,
//     ROCKSDB_NAMESPACE::BlockBasedTableOptions().enable_index_compression,
//     "Compress the index block");

// DEFINE_bool(block_align,
//             ROCKSDB_NAMESPACE::BlockBasedTableOptions().block_align,
//             "Align data blocks on page size");

// DEFINE_int64(prepopulate_block_cache, 0,
//              "Pre-populate hot/warm blocks in block cache. 0 to disable and 1
//              " "to insert during flush");

// DEFINE_bool(use_data_block_hash_index, false,
//             "if use kDataBlockBinaryAndHash "
//             "instead of kDataBlockBinarySearch. "
//             "This is valid if only we use BlockTable");

// DEFINE_double(data_block_hash_table_util_ratio, 0.75,
//               "util ratio for data block hash index table. "
//               "This is only valid if use_data_block_hash_index is "
//               "set to true");

// DEFINE_int64(compressed_cache_size, -1,
//              "Number of bytes to use as a cache of compressed data.");

// DEFINE_int64(row_cache_size, 0,
//              "Number of bytes to use as a cache of individual rows"
//              " (0 = disabled).");

// DEFINE_int32(open_files, ROCKSDB_NAMESPACE::Options().max_open_files,
//              "Maximum number of files to keep open at the same time"
//              " (use default if == 0)");

// DEFINE_int32(file_opening_threads,
//              ROCKSDB_NAMESPACE::Options().max_file_opening_threads,
//              "If open_files is set to -1, this option set the number of "
//              "threads that will be used to open files during DB::Open()");

// DEFINE_int32(compaction_readahead_size, 0, "Compaction readahead size");

// DEFINE_int32(log_readahead_size, 0, "WAL and manifest readahead size");

// DEFINE_int32(random_access_max_buffer_size, 1024 * 1024,
//              "Maximum windows randomaccess buffer size");

// DEFINE_int32(writable_file_max_buffer_size, 1024 * 1024,
//              "Maximum write buffer for Writable File");

// DEFINE_int32(bloom_bits, -1,
//              "Bloom filter bits per key. Negative means use default."
//              "Zero disables.");

// DEFINE_bool(use_ribbon_filter, false, "Use Ribbon instead of Bloom filter");

// DEFINE_double(memtable_bloom_size_ratio, 0,
//               "Ratio of memtable size used for bloom filter. 0 means no bloom
//               " "filter.");
// DEFINE_bool(memtable_whole_key_filtering, false,
//             "Try to use whole key bloom filter in memtables.");
// DEFINE_bool(memtable_use_huge_page, false,
//             "Try to use huge page in memtables.");

// DEFINE_bool(whole_key_filtering,
//             ROCKSDB_NAMESPACE::BlockBasedTableOptions().whole_key_filtering,
//             "Use whole keys (in addition to prefixes) in SST bloom filter.");

DEFINE_bool(use_existing_db, false,
            "If true, do not destroy the existing database.  If you set this "
            "flag and also specify a benchmark that wants a fresh database, "
            "that benchmark will fail.");

DEFINE_bool(use_existing_keys, false,
            "If true, uses existing keys in the DB, "
            "rather than generating new ones. This involves some startup "
            "latency to load all keys into memory. It is supported for the "
            "same read/overwrite benchmarks as `-use_existing_db=true`, which "
            "must also be set for this flag to be enabled. When this flag is "
            "set, the value for `-num` will be ignored.");

// DEFINE_bool(show_table_properties, false,
//             "If true, then per-level table"
//             " properties will be printed on every stats-interval when"
//             " stats_interval is set and stats_per_interval is on.");

DEFINE_string(db, "", "Use the db with the following name.");

DEFINE_bool(progress_reports, true,
            "If true, db_bench will report number of finished operations.");

// // Read cache flags

// DEFINE_string(read_cache_path, "",
//               "If not empty string, a read cache will be used in this path");

// DEFINE_int64(read_cache_size, 4LL * 1024 * 1024 * 1024,
//              "Maximum size of the read cache");

// DEFINE_bool(read_cache_direct_write, true,
//             "Whether to use Direct IO for writing to the read cache");

// DEFINE_bool(read_cache_direct_read, true,
//             "Whether to use Direct IO for reading from read cache");

// DEFINE_bool(use_keep_filter, false, "Whether to use a noop compaction
// filter");

// static bool ValidateCacheNumshardbits(const char* flagname, int32_t value) {
//   if (value >= 20) {
//     fprintf(stderr, "Invalid value for --%s: %d, must be < 20\n", flagname,
//             value);
//     return false;
//   }
//   return true;
// }

// DEFINE_bool(verify_checksum, true,
//             "Verify checksum for every block read from storage");

// DEFINE_int32(checksum_type,
//              ROCKSDB_NAMESPACE::BlockBasedTableOptions().checksum,
//              "ChecksumType as an int");

// DEFINE_bool(statistics, false, "Database statistics");
// DEFINE_int32(stats_level,
// ROCKSDB_NAMESPACE::StatsLevel::kExceptDetailedTimers,
//              "stats level for statistics");
// DEFINE_string(statistics_string, "", "Serialized statistics string");
// static class std::shared_ptr<ROCKSDB_NAMESPACE::Statistics> dbstats;

DEFINE_int64(writes, -1,
             "Number of write operations to do. If negative, do --num reads.");

// DEFINE_bool(finish_after_writes, false,
//             "Write thread terminates after all writes are finished");

// DEFINE_bool(sync, false, "Sync all writes to disk");

// DEFINE_bool(use_fsync, false, "If true, issue fsync instead of fdatasync");

// DEFINE_bool(disable_wal, false, "If true, do not write WAL for write.");

// DEFINE_bool(manual_wal_flush, false,
//             "If true, buffer WAL until buffer is full or a manual
//             FlushWAL().");

// DEFINE_string(wal_compression, "none",
//               "Algorithm to use for WAL compression. none to disable.");
// static enum ROCKSDB_NAMESPACE::CompressionType FLAGS_wal_compression_e =
//     ROCKSDB_NAMESPACE::kNoCompression;

// DEFINE_string(wal_dir, "", "If not empty, use the given dir for WAL");

// DEFINE_string(truth_db, "/dev/shm/truth_db/dbbench",
//               "Truth key/values used when using verify");

// DEFINE_int32(num_levels, 7, "The total number of levels");

// DEFINE_int64(target_file_size_base,
//              ROCKSDB_NAMESPACE::Options().target_file_size_base,
//              "Target file size at level-1");

// DEFINE_int32(target_file_size_multiplier,
//              ROCKSDB_NAMESPACE::Options().target_file_size_multiplier,
//              "A multiplier to compute target level-N file size (N >= 2)");

// DEFINE_uint64(max_bytes_for_level_base,
//               ROCKSDB_NAMESPACE::Options().max_bytes_for_level_base,
//               "Max bytes for level-1");

// DEFINE_bool(level_compaction_dynamic_level_bytes, false,
//             "Whether level size base is dynamic");

// DEFINE_double(max_bytes_for_level_multiplier, 10,
//               "A multiplier to compute max bytes for level-N (N >= 2)");

// static std::vector<int> FLAGS_max_bytes_for_level_multiplier_additional_v;
// DEFINE_string(max_bytes_for_level_multiplier_additional, "",
//               "A vector that specifies additional fanout per level");

// DEFINE_int32(level0_stop_writes_trigger,
//              ROCKSDB_NAMESPACE::Options().level0_stop_writes_trigger,
//              "Number of files in level-0 that will trigger put stop.");

// DEFINE_int32(level0_slowdown_writes_trigger,
//              ROCKSDB_NAMESPACE::Options().level0_slowdown_writes_trigger,
//              "Number of files in level-0 that will slow down writes.");

// DEFINE_int32(level0_file_num_compaction_trigger,
//              ROCKSDB_NAMESPACE::Options().level0_file_num_compaction_trigger,
//              "Number of files in level-0 when compactions start.");

// DEFINE_uint64(periodic_compaction_seconds,
//               ROCKSDB_NAMESPACE::Options().periodic_compaction_seconds,
//               "Files older than this will be picked up for compaction and"
//               " rewritten to the same level");

// DEFINE_uint64(ttl_seconds, ROCKSDB_NAMESPACE::Options().ttl, "Set
// options.ttl");

// static bool ValidateInt32Percent(const char* flagname, int32_t value) {
//   if (value <= 0 || value >= 100) {
//     fprintf(stderr, "Invalid value for --%s: %d, 0< pct <100 \n", flagname,
//             value);
//     return false;
//   }
//   return true;
// }
// DEFINE_int32(readwritepercent, 90,
//              "Ratio of reads to reads/writes (expressed as percentage) for "
//              "the ReadRandomWriteRandom workload. The default value 90 means
//              " "90% operations out of all reads and writes operations are "
//              "reads. In other words, 9 gets for every 1 put.");

// DEFINE_int32(mergereadpercent, 70,
//              "Ratio of merges to merges&reads (expressed as percentage) for "
//              "the ReadRandomMergeRandom workload. The default value 70 means
//              " "70% out of all read and merge operations are merges. In other
//              " "words, 7 merges for every 3 gets.");

// DEFINE_int32(deletepercent, 2,
//              "Percentage of deletes out of reads/writes/deletes (used in "
//              "RandomWithVerify only). RandomWithVerify "
//              "calculates writepercent as (100 - FLAGS_readwritepercent - "
//              "deletepercent), so deletepercent must be smaller than (100 - "
//              "FLAGS_readwritepercent)");

// DEFINE_bool(optimize_filters_for_hits,
//             ROCKSDB_NAMESPACE::Options().optimize_filters_for_hits,
//             "Optimizes bloom filters for workloads for most lookups return "
//             "a value. For now this doesn't create bloom filters for the max "
//             "level of the LSM to reduce metadata that should fit in RAM. ");

// DEFINE_bool(paranoid_checks, ROCKSDB_NAMESPACE::Options().paranoid_checks,
//             "RocksDB will aggressively check consistency of the data.");

// DEFINE_bool(force_consistency_checks,
//             ROCKSDB_NAMESPACE::Options().force_consistency_checks,
//             "Runs consistency checks on the LSM every time a change is "
//             "applied.");

// DEFINE_bool(check_flush_compaction_key_order,
//             ROCKSDB_NAMESPACE::Options().check_flush_compaction_key_order,
//             "During flush or compaction, check whether keys inserted to "
//             "output files are in order.");

// DEFINE_uint64(delete_obsolete_files_period_micros, 0,
//               "Ignored. Left here for backward compatibility");

// DEFINE_int64(writes_before_delete_range, 0,
//              "Number of writes before DeleteRange is called regularly.");

// DEFINE_int64(writes_per_range_tombstone, 0,
//              "Number of writes between range tombstones");

// DEFINE_int64(range_tombstone_width, 100, "Number of keys in tombstone's
// range");

// DEFINE_int64(max_num_range_tombstones, 0,
//              "Maximum number of range tombstones to insert.");

// DEFINE_bool(expand_range_tombstones, false,
//             "Expand range tombstone into sequential regular tombstones.");

// // Transactions Options
// DEFINE_bool(optimistic_transaction_db, false,
//             "Open a OptimisticTransactionDB instance. "
//             "Required for randomtransaction benchmark.");

// DEFINE_bool(transaction_db, false,
//             "Open a TransactionDB instance. "
//             "Required for randomtransaction benchmark.");

// DEFINE_uint64(transaction_sets, 2,
//               "Number of keys each transaction will "
//               "modify (use in RandomTransaction only).  Max: 9999");

// DEFINE_bool(transaction_set_snapshot, false,
//             "Setting to true will have each transaction call SetSnapshot()"
//             " upon creation.");

// DEFINE_int32(transaction_sleep, 0,
//              "Max microseconds to sleep in between "
//              "reading and writing a value (used in RandomTransaction only).
//              ");

// DEFINE_uint64(transaction_lock_timeout, 100,
//               "If using a transaction_db, specifies the lock wait timeout in"
//               " milliseconds before failing a transaction waiting on a
//               lock");
// DEFINE_string(
//     options_file, "",
//     "The path to a RocksDB options file.  If specified, then db_bench will "
//     "run with the RocksDB options in the default column family of the "
//     "specified options file. "
//     "Note that with this setting, db_bench will ONLY accept the following "
//     "RocksDB options related command-line arguments, all other arguments "
//     "that are related to RocksDB options will be ignored:\n"
//     "\t--use_existing_db\n"
//     "\t--use_existing_keys\n"
//     "\t--statistics\n"
//     "\t--row_cache_size\n"
//     "\t--row_cache_numshardbits\n"
//     "\t--enable_io_prio\n"
//     "\t--dump_malloc_stats\n"
//     "\t--num_multi_db\n");

// // FIFO Compaction Options
// DEFINE_uint64(fifo_compaction_max_table_files_size_mb, 0,
//               "The limit of total table file sizes to trigger FIFO
//               compaction");

// DEFINE_bool(fifo_compaction_allow_compaction, true,
//             "Allow compaction in FIFO compaction.");

// DEFINE_uint64(fifo_compaction_ttl, 0, "TTL for the SST Files in seconds.");

// DEFINE_uint64(fifo_age_for_warm, 0, "age_for_warm for FIFO compaction.");

// // Stacked BlobDB Options
// DEFINE_bool(use_blob_db, false, "[Stacked BlobDB] Open a BlobDB instance.");

// DEFINE_bool(
//     blob_db_enable_gc,
//     ROCKSDB_NAMESPACE::blob_db::BlobDBOptions().enable_garbage_collection,
//     "[Stacked BlobDB] Enable BlobDB garbage collection.");

// DEFINE_double(
//     blob_db_gc_cutoff,
//     ROCKSDB_NAMESPACE::blob_db::BlobDBOptions().garbage_collection_cutoff,
//     "[Stacked BlobDB] Cutoff ratio for BlobDB garbage collection.");

// DEFINE_bool(blob_db_is_fifo,
//             ROCKSDB_NAMESPACE::blob_db::BlobDBOptions().is_fifo,
//             "[Stacked BlobDB] Enable FIFO eviction strategy in BlobDB.");

// DEFINE_uint64(blob_db_max_db_size,
//               ROCKSDB_NAMESPACE::blob_db::BlobDBOptions().max_db_size,
//               "[Stacked BlobDB] Max size limit of the directory where blob "
//               "files are stored.");

// DEFINE_uint64(blob_db_max_ttl_range, 0,
//               "[Stacked BlobDB] TTL range to generate BlobDB data (in "
//               "seconds). 0 means no TTL.");

// DEFINE_uint64(
//     blob_db_ttl_range_secs,
//     ROCKSDB_NAMESPACE::blob_db::BlobDBOptions().ttl_range_secs,
//     "[Stacked BlobDB] TTL bucket size to use when creating blob files.");

// DEFINE_uint64(
//     blob_db_min_blob_size,
//     ROCKSDB_NAMESPACE::blob_db::BlobDBOptions().min_blob_size,
//     "[Stacked BlobDB] Smallest blob to store in a file. Blobs "
//     "smaller than this will be inlined with the key in the LSM tree.");

// DEFINE_uint64(blob_db_bytes_per_sync,
//               ROCKSDB_NAMESPACE::blob_db::BlobDBOptions().bytes_per_sync,
//               "[Stacked BlobDB] Bytes to sync blob file at.");

// DEFINE_uint64(blob_db_file_size,
//               ROCKSDB_NAMESPACE::blob_db::BlobDBOptions().blob_file_size,
//               "[Stacked BlobDB] Target size of each blob file.");

// DEFINE_string(
//     blob_db_compression_type, "snappy",
//     "[Stacked BlobDB] Algorithm to use to compress blobs in blob files.");
// static enum ROCKSDB_NAMESPACE::CompressionType
//     FLAGS_blob_db_compression_type_e = ROCKSDB_NAMESPACE::kSnappyCompression;

// // Integrated BlobDB options
// DEFINE_bool(
//     enable_blob_files,
//     ROCKSDB_NAMESPACE::AdvancedColumnFamilyOptions().enable_blob_files,
//     "[Integrated BlobDB] Enable writing large values to separate blob
//     files.");

// DEFINE_uint64(min_blob_size,
//               ROCKSDB_NAMESPACE::AdvancedColumnFamilyOptions().min_blob_size,
//               "[Integrated BlobDB] The size of the smallest value to be
//               stored " "separately in a blob file.");

// DEFINE_uint64(blob_file_size,
//               ROCKSDB_NAMESPACE::AdvancedColumnFamilyOptions().blob_file_size,
//               "[Integrated BlobDB] The size limit for blob files.");

// DEFINE_string(blob_compression_type, "none",
//               "[Integrated BlobDB] The compression algorithm to use for large
//               " "values stored in blob files.");

// DEFINE_bool(enable_blob_garbage_collection,
//             ROCKSDB_NAMESPACE::AdvancedColumnFamilyOptions()
//                 .enable_blob_garbage_collection,
//             "[Integrated BlobDB] Enable blob garbage collection.");

// DEFINE_double(blob_garbage_collection_age_cutoff,
//               ROCKSDB_NAMESPACE::AdvancedColumnFamilyOptions()
//                   .blob_garbage_collection_age_cutoff,
//               "[Integrated BlobDB] The cutoff in terms of blob file age for "
//               "garbage collection.");

// DEFINE_double(blob_garbage_collection_force_threshold,
//               ROCKSDB_NAMESPACE::AdvancedColumnFamilyOptions()
//                   .blob_garbage_collection_force_threshold,
//               "[Integrated BlobDB] The threshold for the ratio of garbage in
//               " "the oldest blob files for forcing garbage collection.");

// DEFINE_uint64(blob_compaction_readahead_size,
//               ROCKSDB_NAMESPACE::AdvancedColumnFamilyOptions()
//                   .blob_compaction_readahead_size,
//               "[Integrated BlobDB] Compaction readahead for blob files.");

// DEFINE_int32(
//     blob_file_starting_level,
//     ROCKSDB_NAMESPACE::AdvancedColumnFamilyOptions().blob_file_starting_level,
//     "[Integrated BlobDB] The starting level for blob files.");

// DEFINE_bool(use_blob_cache, false, "[Integrated BlobDB] Enable blob cache.");

// DEFINE_bool(
//     use_shared_block_and_blob_cache, true,
//     "[Integrated BlobDB] Use a shared backing cache for both block "
//     "cache and blob cache. It only takes effect if use_blob_cache is
//     enabled.");

// DEFINE_uint64(
//     blob_cache_size, 8 << 20,
//     "[Integrated BlobDB] Number of bytes to use as a cache of blobs. It only
//     " "takes effect if the block and blob caches are different "
//     "(use_shared_block_and_blob_cache = false).");

// DEFINE_int32(blob_cache_numshardbits, 6,
//              "[Integrated BlobDB] Number of shards for the blob cache is 2 **
//              " "blob_cache_numshardbits. Negative means use default settings.
//              " "It only takes effect if blob_cache_size is greater than 0,
//              and " "the block and blob caches are different "
//              "(use_shared_block_and_blob_cache = false).");

// DEFINE_int32(prepopulate_blob_cache, 0,
//              "[Integrated BlobDB] Pre-populate hot/warm blobs in blob cache.
//              0 " "to disable and 1 to insert during flush.");

// // Secondary DB instance Options
// DEFINE_bool(use_secondary_db, false,
//             "Open a RocksDB secondary instance. A primary instance can be "
//             "running in another db_bench process.");

// DEFINE_string(secondary_path, "",
//               "Path to a directory used by the secondary instance to store "
//               "private files, e.g. info log.");

// DEFINE_int32(secondary_update_interval, 5,
//              "Secondary instance attempts to catch up with the primary every
//              " "secondary_update_interval seconds.");

// DEFINE_bool(report_bg_io_stats, false,
//             "Measure times spents on I/Os while in compactions. ");

// DEFINE_bool(use_stderr_info_logger, false,
//             "Write info logs to stderr instead of to LOG file. ");

// DEFINE_string(trace_file, "", "Trace workload to a file. ");

// DEFINE_double(trace_replay_fast_forward, 1.0,
//               "Fast forward trace replay, must > 0.0.");
// DEFINE_int32(block_cache_trace_sampling_frequency, 1,
//              "Block cache trace sampling frequency, termed s. It uses spatial
//              " "downsampling and samples accesses to one out of s blocks.");
// DEFINE_int64(
//     block_cache_trace_max_trace_file_size_in_bytes,
//     uint64_t{64} * 1024 * 1024 * 1024,
//     "The maximum block cache trace file size in bytes. Block cache accesses "
//     "will not be logged if the trace file size exceeds this threshold.
//     Default " "is 64 GB.");
// DEFINE_string(block_cache_trace_file, "", "Block cache trace file path.");
// DEFINE_int32(trace_replay_threads, 1,
//              "The number of threads to replay, must >=1.");

// DEFINE_bool(io_uring_enabled, true,
//             "If true, enable the use of IO uring if the platform supports
//             it");
// extern "C" bool RocksDbIOUringEnable() { return FLAGS_io_uring_enabled; }

// DEFINE_bool(adaptive_readahead, false,
//             "carry forward internal auto readahead size from one file to next
//             " "file at each level during iteration");

// DEFINE_bool(rate_limit_user_ops, false,
//             "When true use Env::IO_USER priority level to charge internal
//             rate " "limiter for reads associated with user operations.");

// DEFINE_bool(file_checksum, false,
//             "When true use FileChecksumGenCrc32cFactory for "
//             "file_checksum_gen_factory.");

// DEFINE_bool(rate_limit_auto_wal_flush, false,
//             "When true use Env::IO_USER priority level to charge internal
//             rate " "limiter for automatic WAL flush
//             (`Options::manual_wal_flush` == " "false) after the user write
//             operation.");

// DEFINE_bool(async_io, false,
//             "When set true, RocksDB does asynchronous reads for internal auto
//             " "readahead prefetching.");

// DEFINE_bool(optimize_multiget_for_io, true,
//             "When set true, RocksDB does asynchronous reads for SST files in
//             " "multiple levels for MultiGet.");

// DEFINE_bool(charge_compression_dictionary_building_buffer, false,
//             "Setting for "
//             "CacheEntryRoleOptions::charged of "
//             "CacheEntryRole::kCompressionDictionaryBuildingBuffer");

// DEFINE_bool(charge_filter_construction, false,
//             "Setting for "
//             "CacheEntryRoleOptions::charged of "
//             "CacheEntryRole::kFilterConstruction");

// DEFINE_bool(charge_table_reader, false,
//             "Setting for "
//             "CacheEntryRoleOptions::charged of "
//             "CacheEntryRole::kBlockBasedTableReader");

// DEFINE_bool(charge_file_metadata, false,
//             "Setting for "
//             "CacheEntryRoleOptions::charged of "
//             "CacheEntryRole::kFileMetadata");

// DEFINE_bool(charge_blob_cache, false,
//             "Setting for "
//             "CacheEntryRoleOptions::charged of "
//             "CacheEntryRole::kBlobCache");

// DEFINE_uint64(backup_rate_limit, 0ull,
//               "If non-zero, db_bench will rate limit reads and writes for DB
//               " "backup. This " "is the global rate in ops/second.");

// DEFINE_uint64(restore_rate_limit, 0ull,
//               "If non-zero, db_bench will rate limit reads and writes for DB
//               " "restore. This " "is the global rate in ops/second.");

// DEFINE_string(backup_dir, "",
//               "If not empty string, use the given dir for backup.");

// DEFINE_string(restore_dir, "",
//               "If not empty string, use the given dir for restore.");

// DEFINE_uint64(
//     initial_auto_readahead_size,
//     ROCKSDB_NAMESPACE::BlockBasedTableOptions().initial_auto_readahead_size,
//     "RocksDB does auto-readahead for iterators on noticing more than two
//     reads " "for a table file if user doesn't provide readahead_size. The
//     readahead " "size starts at initial_auto_readahead_size");

// DEFINE_uint64(
//     max_auto_readahead_size,
//     ROCKSDB_NAMESPACE::BlockBasedTableOptions().max_auto_readahead_size,
//     "Rocksdb implicit readahead starts at "
//     "BlockBasedTableOptions.initial_auto_readahead_size and doubles on every
//     " "additional read upto max_auto_readahead_size");

// DEFINE_uint64(
//     num_file_reads_for_auto_readahead,
//     ROCKSDB_NAMESPACE::BlockBasedTableOptions()
//         .num_file_reads_for_auto_readahead,
//     "Rocksdb implicit readahead is enabled if reads are sequential and "
//     "num_file_reads_for_auto_readahead indicates after how many sequential "
//     "reads into that file internal auto prefetching should be start.");

// static enum ROCKSDB_NAMESPACE::CompressionType StringToCompressionType(
//     const char* ctype) {
//   assert(ctype);

//   if (!strcasecmp(ctype, "none"))
//     return ROCKSDB_NAMESPACE::kNoCompression;
//   else if (!strcasecmp(ctype, "snappy"))
//     return ROCKSDB_NAMESPACE::kSnappyCompression;
//   else if (!strcasecmp(ctype, "zlib"))
//     return ROCKSDB_NAMESPACE::kZlibCompression;
//   else if (!strcasecmp(ctype, "bzip2"))
//     return ROCKSDB_NAMESPACE::kBZip2Compression;
//   else if (!strcasecmp(ctype, "lz4"))
//     return ROCKSDB_NAMESPACE::kLZ4Compression;
//   else if (!strcasecmp(ctype, "lz4hc"))
//     return ROCKSDB_NAMESPACE::kLZ4HCCompression;
//   else if (!strcasecmp(ctype, "xpress"))
//     return ROCKSDB_NAMESPACE::kXpressCompression;
//   else if (!strcasecmp(ctype, "zstd"))
//     return ROCKSDB_NAMESPACE::kZSTD;
//   else {
//     fprintf(stderr, "Cannot parse compression type '%s'\n", ctype);
//     exit(1);
//   }
// }

// static std::string ColumnFamilyName(size_t i) {
//   if (i == 0) {
//     return ROCKSDB_NAMESPACE::kDefaultColumnFamilyName;
//   } else {
//     char name[100];
//     snprintf(name, sizeof(name), "column_family_name_%06zu", i);
//     return std::string(name);
//   }
// }

// DEFINE_string(compression_type, "snappy",
//               "Algorithm to use to compress the database");
// static enum ROCKSDB_NAMESPACE::CompressionType FLAGS_compression_type_e =
//     ROCKSDB_NAMESPACE::kSnappyCompression;

// DEFINE_int64(sample_for_compression, 0, "Sample every N block for
// compression");

// DEFINE_int32(compression_level,
// ROCKSDB_NAMESPACE::CompressionOptions().level,
//              "Compression level. The meaning of this value is library-"
//              "dependent. If unset, we try to use the default for the library
//              " "specified in `--compression_type`");

// DEFINE_int32(compression_max_dict_bytes,
//              ROCKSDB_NAMESPACE::CompressionOptions().max_dict_bytes,
//              "Maximum size of dictionary used to prime the compression "
//              "library.");

// DEFINE_int32(compression_zstd_max_train_bytes,
//              ROCKSDB_NAMESPACE::CompressionOptions().zstd_max_train_bytes,
//              "Maximum size of training data passed to zstd's dictionary "
//              "trainer.");

// DEFINE_int32(min_level_to_compress, -1,
//              "If non-negative, compression starts"
//              " from this level. Levels with number < min_level_to_compress
//              are" " not compressed. Otherwise, apply compression_type to "
//              "all levels.");

// DEFINE_int32(compression_parallel_threads, 1,
//              "Number of threads for parallel compression.");

// DEFINE_uint64(compression_max_dict_buffer_bytes,
//               ROCKSDB_NAMESPACE::CompressionOptions().max_dict_buffer_bytes,
//               "Maximum bytes to buffer to collect samples for dictionary.");

// DEFINE_bool(compression_use_zstd_dict_trainer,
//             ROCKSDB_NAMESPACE::CompressionOptions().use_zstd_dict_trainer,
//             "If true, use ZSTD_TrainDictionary() to create dictionary, else"
//             "use ZSTD_FinalizeDictionary() to create dictionary");

// static bool ValidateTableCacheNumshardbits(const char* flagname,
//                                            int32_t value) {
//   if (0 >= value || value >= 20) {
//     fprintf(stderr, "Invalid value for --%s: %d, must be  0 < val < 20\n",
//             flagname, value);
//     return false;
//   }
//   return true;
// }
// DEFINE_int32(table_cache_numshardbits, 4, "");

// DEFINE_string(env_uri, "",
//               "URI for registry Env lookup. Mutually exclusive with
//               --fs_uri");
// DEFINE_string(fs_uri, "",
//               "URI for registry Filesystem lookup. Mutually exclusive"
//               " with --env_uri."
//               " Creates a default environment with the specified
//               filesystem.");
// DEFINE_string(simulate_hybrid_fs_file, "",
//               "File for Store Metadata for Simulate hybrid FS. Empty means "
//               "disable the feature. Now, if it is set, last_level_temperature
//               " "is set to kWarm.");
// DEFINE_int32(simulate_hybrid_hdd_multipliers, 1,
//              "In simulate_hybrid_fs_file or simulate_hdd mode, how many HDDs
//              " "are simulated.");
// DEFINE_bool(simulate_hdd, false, "Simulate read/write latency on HDD.");

// DEFINE_int64(
//     preclude_last_level_data_seconds, 0,
//     "Preclude the latest data from the last level. (Used for tiered
//     storage)");

// DEFINE_int64(preserve_internal_time_seconds, 0,
//              "Preserve the internal time information which stores with
//              SST.");

// static std::shared_ptr<ROCKSDB_NAMESPACE::Env> env_guard;

// static leveldb::Env* FLAGS_env = leveldb::Env::Default();

DEFINE_int64(stats_interval, 0,
             "Stats are reported every N operations when this is greater than "
             "zero. When 0 the interval grows over time.");

DEFINE_int64(stats_interval_seconds, 0,
             "Report stats every N seconds. This overrides stats_interval when"
             " both are > 0.");

// DEFINE_int32(stats_per_interval, 0,
//              "Reports additional stats per interval when this is greater than
//              " "0.");

DEFINE_uint64(slow_usecs, 1000000,
              "A message is printed for operations that take at least this "
              "many microseconds.");

// DEFINE_int64(report_interval_seconds, 0,
//              "If greater than zero, it will write simple stats in CSV format
//              " "to --report_file every N seconds");

// DEFINE_string(report_file, "report.csv",
//               "Filename where some simple stats are reported to (if "
//               "--report_interval_seconds is bigger than 0)");

// DEFINE_int32(thread_status_per_interval, 0,
//              "Takes and report a snapshot of the current status of each
//              thread" " when this is greater than 0.");

// DEFINE_int32(perf_level, ROCKSDB_NAMESPACE::PerfLevel::kDisable,
//              "Level of perf collection");

// DEFINE_uint64(soft_pending_compaction_bytes_limit, 64ull * 1024 * 1024 *
// 1024,
//               "Slowdown writes if pending compaction bytes exceed this
//               number");

// DEFINE_uint64(hard_pending_compaction_bytes_limit, 128ull * 1024 * 1024 *
// 1024,
//               "Stop writes if pending compaction bytes exceed this number");

// DEFINE_uint64(delayed_write_rate, 8388608u,
//               "Limited bytes allowed to DB when soft_rate_limit or "
//               "level0_slowdown_writes_trigger triggers");

// DEFINE_bool(enable_pipelined_write, true,
//             "Allow WAL and memtable writes to be pipelined");

// DEFINE_bool(
//     unordered_write, false,
//     "Enable the unordered write feature, which provides higher throughput but
//     " "relaxes the guarantees around atomic reads and immutable snapshots");

// DEFINE_bool(allow_concurrent_memtable_write, true,
//             "Allow multi-writers to update mem tables in parallel.");

// DEFINE_double(experimental_mempurge_threshold, 0.0,
//               "Maximum useful payload ratio estimate that triggers a mempurge
//               "
//               "(memtable garbage collection).");

// DEFINE_bool(inplace_update_support,
//             ROCKSDB_NAMESPACE::Options().inplace_update_support,
//             "Support in-place memtable update for smaller or same-size
//             values");

// DEFINE_uint64(inplace_update_num_locks,
//               ROCKSDB_NAMESPACE::Options().inplace_update_num_locks,
//               "Number of RW locks to protect in-place memtable updates");

// DEFINE_bool(enable_write_thread_adaptive_yield, true,
//             "Use a yielding spin loop for brief writer thread waits.");

// DEFINE_uint64(
//     write_thread_max_yield_usec, 100,
//     "Maximum microseconds for enable_write_thread_adaptive_yield
//     operation.");

// DEFINE_uint64(write_thread_slow_yield_usec, 3,
//               "The threshold at which a slow yield is considered a signal
//               that " "other processes or threads want the core.");

// DEFINE_uint64(rate_limiter_bytes_per_sec, 0, "Set options.rate_limiter
// value.");

// DEFINE_int64(rate_limiter_refill_period_us, 100 * 1000,
//              "Set refill period on rate limiter.");

// DEFINE_bool(rate_limiter_auto_tuned, false,
//             "Enable dynamic adjustment of rate limit according to demand for
//             " "background I/O");

// DEFINE_bool(sine_write_rate, false, "Use a sine wave write_rate_limit");

// DEFINE_uint64(
//     sine_write_rate_interval_milliseconds, 10000,
//     "Interval of which the sine wave write_rate_limit is recalculated");

// DEFINE_double(sine_a, 1, "A in f(x) = A sin(bx + c) + d");

// DEFINE_double(sine_b, 1, "B in f(x) = A sin(bx + c) + d");

// DEFINE_double(sine_c, 0, "C in f(x) = A sin(bx + c) + d");

// DEFINE_double(sine_d, 1, "D in f(x) = A sin(bx + c) + d");

// DEFINE_bool(rate_limit_bg_reads, false,
//             "Use options.rate_limiter on compaction reads");

// DEFINE_uint64(
//     benchmark_write_rate_limit, 0,
//     "If non-zero, db_bench will rate-limit the writes going into RocksDB.
//     This " "is the global rate in bytes/second.");

// // the parameters of mix_graph
// DEFINE_double(keyrange_dist_a, 0.0,
//               "The parameter 'a' of prefix average access distribution "
//               "f(x)=a*exp(b*x)+c*exp(d*x)");
// DEFINE_double(keyrange_dist_b, 0.0,
//               "The parameter 'b' of prefix average access distribution "
//               "f(x)=a*exp(b*x)+c*exp(d*x)");
// DEFINE_double(keyrange_dist_c, 0.0,
//               "The parameter 'c' of prefix average access distribution"
//               "f(x)=a*exp(b*x)+c*exp(d*x)");
// DEFINE_double(keyrange_dist_d, 0.0,
//               "The parameter 'd' of prefix average access distribution"
//               "f(x)=a*exp(b*x)+c*exp(d*x)");
// DEFINE_int64(keyrange_num, 1,
//              "The number of key ranges that are in the same prefix "
//              "group, each prefix range will have its key access
//              distribution");
// DEFINE_double(key_dist_a, 0.0,
//               "The parameter 'a' of key access distribution model
//               f(x)=a*x^b");
// DEFINE_double(key_dist_b, 0.0,
//               "The parameter 'b' of key access distribution model
//               f(x)=a*x^b");
// DEFINE_double(value_theta, 0.0,
//               "The parameter 'theta' of Generized Pareto Distribution "
//               "f(x)=(1/sigma)*(1+k*(x-theta)/sigma)^-(1/k+1)");
// // Use reasonable defaults based on the mixgraph paper
// DEFINE_double(value_k, 0.2615,
//               "The parameter 'k' of Generized Pareto Distribution "
//               "f(x)=(1/sigma)*(1+k*(x-theta)/sigma)^-(1/k+1)");
// // Use reasonable defaults based on the mixgraph paper
// DEFINE_double(value_sigma, 25.45,
//               "The parameter 'theta' of Generized Pareto Distribution "
//               "f(x)=(1/sigma)*(1+k*(x-theta)/sigma)^-(1/k+1)");
// DEFINE_double(iter_theta, 0.0,
//               "The parameter 'theta' of Generized Pareto Distribution "
//               "f(x)=(1/sigma)*(1+k*(x-theta)/sigma)^-(1/k+1)");
// // Use reasonable defaults based on the mixgraph paper
// DEFINE_double(iter_k, 2.517,
//               "The parameter 'k' of Generized Pareto Distribution "
//               "f(x)=(1/sigma)*(1+k*(x-theta)/sigma)^-(1/k+1)");
// // Use reasonable defaults based on the mixgraph paper
// DEFINE_double(iter_sigma, 14.236,
//               "The parameter 'sigma' of Generized Pareto Distribution "
//               "f(x)=(1/sigma)*(1+k*(x-theta)/sigma)^-(1/k+1)");
// DEFINE_double(mix_get_ratio, 1.0,
//               "The ratio of Get queries of mix_graph workload");
// DEFINE_double(mix_put_ratio, 0.0,
//               "The ratio of Put queries of mix_graph workload");
// DEFINE_double(mix_seek_ratio, 0.0,
//               "The ratio of Seek queries of mix_graph workload");
// DEFINE_int64(mix_max_scan_len, 10000, "The max scan length of Iterator");
// DEFINE_int64(mix_max_value_size, 1024, "The max value size of this
// workload"); DEFINE_double(
//     sine_mix_rate_noise, 0.0,
//     "Add the noise ratio to the sine rate, it is between 0.0 and 1.0");
// DEFINE_bool(sine_mix_rate, false,
//             "Enable the sine QPS control on the mix workload");
// DEFINE_uint64(
//     sine_mix_rate_interval_milliseconds, 10000,
//     "Interval of which the sine wave read_rate_limit is recalculated");
// DEFINE_int64(mix_accesses, -1,
//              "The total query accesses of mix_graph workload");

// DEFINE_uint64(
//     benchmark_read_rate_limit, 0,
//     "If non-zero, db_bench will rate-limit the reads from RocksDB. This "
//     "is the global rate in ops/second.");

// DEFINE_uint64(max_compaction_bytes,
//               ROCKSDB_NAMESPACE::Options().max_compaction_bytes,
//               "Max bytes allowed in one compaction");

// DEFINE_bool(readonly, false, "Run read only benchmarks.");

// DEFINE_bool(print_malloc_stats, false,
//             "Print malloc stats to stdout after benchmarks finish.");

// DEFINE_bool(disable_auto_compactions, false, "Do not auto trigger
// compactions");

// DEFINE_uint64(wal_ttl_seconds, 0, "Set the TTL for the WAL Files in
// seconds."); DEFINE_uint64(wal_size_limit_MB, 0,
//               "Set the size limit for the WAL Files in MB.");
// DEFINE_uint64(max_total_wal_size, 0, "Set total max WAL size");

// DEFINE_bool(mmap_read, ROCKSDB_NAMESPACE::Options().allow_mmap_reads,
//             "Allow reads to occur via mmap-ing files");

// DEFINE_bool(mmap_write, ROCKSDB_NAMESPACE::Options().allow_mmap_writes,
//             "Allow writes to occur via mmap-ing files");

// DEFINE_bool(use_direct_reads, ROCKSDB_NAMESPACE::Options().use_direct_reads,
//             "Use O_DIRECT for reading data");

// DEFINE_bool(use_direct_io_for_flush_and_compaction,
//             ROCKSDB_NAMESPACE::Options().use_direct_io_for_flush_and_compaction,
//             "Use O_DIRECT for background flush and compaction writes");

// DEFINE_bool(advise_random_on_open,
//             ROCKSDB_NAMESPACE::Options().advise_random_on_open,
//             "Advise random access on table file open");

// DEFINE_string(compaction_fadvice, "NORMAL",
//               "Access pattern advice when a file is compacted");
// static auto FLAGS_compaction_fadvice_e =
//     ROCKSDB_NAMESPACE::Options().access_hint_on_compaction_start;

// DEFINE_bool(use_tailing_iterator, false,
//             "Use tailing iterator to access a series of keys instead of
//             get");

// DEFINE_bool(use_adaptive_mutex,
// ROCKSDB_NAMESPACE::Options().use_adaptive_mutex,
//             "Use adaptive mutex");

// DEFINE_uint64(bytes_per_sync, ROCKSDB_NAMESPACE::Options().bytes_per_sync,
//               "Allows OS to incrementally sync SST files to disk while they
//               are" " being written, in the background. Issue one request for
//               every" " bytes_per_sync written. 0 turns it off.");

// DEFINE_uint64(wal_bytes_per_sync,
//               ROCKSDB_NAMESPACE::Options().wal_bytes_per_sync,
//               "Allows OS to incrementally sync WAL files to disk while they
//               are" " being written, in the background. Issue one request for
//               every" " wal_bytes_per_sync written. 0 turns it off.");

// DEFINE_bool(use_single_deletes, true,
//             "Use single deletes (used in RandomReplaceKeys only).");

// DEFINE_double(stddev, 2000.0,
//               "Standard deviation of normal distribution used for picking
//               keys" " (used in RandomReplaceKeys only).");

// DEFINE_int32(key_id_range, 100000,
//              "Range of possible value of key id (used in TimeSeries only).");

// DEFINE_string(expire_style, "none",
//               "Style to remove expired time entries. Can be one of the
//               options " "below: none (do not expired data), compaction_filter
//               (use a " "compaction filter to remove expired data), delete
//               (seek IDs and " "remove expired data) (used in TimeSeries
//               only).");

// DEFINE_uint64(
//     time_range, 100000,
//     "Range of timestamp that store in the database (used in TimeSeries"
//     " only).");

// DEFINE_int32(num_deletion_threads, 1,
//              "Number of threads to do deletion (used in TimeSeries and delete
//              " "expire_style only).");

// DEFINE_int32(max_successive_merges, 0,
//              "Maximum number of successive merge operations on a key in the "
//              "memtable");

// static bool ValidatePrefixSize(const char* flagname, int32_t value) {
//   if (value < 0 || value >= 2000000000) {
//     fprintf(stderr, "Invalid value for --%s: %d. 0<= PrefixSize
//     <=2000000000\n",
//             flagname, value);
//     return false;
//   }
//   return true;
// }

// DEFINE_int32(prefix_size, 0,
//              "control the prefix size for HashSkipList and plain table");
// DEFINE_int64(keys_per_prefix, 0,
//              "control average number of keys generated per prefix, 0 means no
//              " "special handling of the prefix, i.e. use the prefix comes
//              with " "the generated random number.");
// DEFINE_bool(total_order_seek, false,
//             "Enable total order seek regardless of index format.");
// DEFINE_bool(prefix_same_as_start, false,
//             "Enforce iterator to return keys with prefix same as seek key.");
// DEFINE_bool(
//     seek_missing_prefix, false,
//     "Iterator seek to keys with non-exist prefixes. Require prefix_size >
//     8");

// DEFINE_int32(memtable_insert_with_hint_prefix_size, 0,
//              "If non-zero, enable "
//              "memtable insert with hint with the given prefix size.");
// DEFINE_bool(enable_io_prio, false,
//             "Lower the background flush/compaction threads' IO priority");
// DEFINE_bool(enable_cpu_prio, false,
//             "Lower the background flush/compaction threads' CPU priority");
// DEFINE_bool(identity_as_first_hash, false,
//             "the first hash function of cuckoo table becomes an identity "
//             "function. This is only valid when key is 8 bytes");
// DEFINE_bool(dump_malloc_stats, true, "Dump malloc stats in LOG ");
// DEFINE_uint64(stats_dump_period_sec,
//               ROCKSDB_NAMESPACE::Options().stats_dump_period_sec,
//               "Gap between printing stats to log in seconds");
// DEFINE_uint64(stats_persist_period_sec,
//               ROCKSDB_NAMESPACE::Options().stats_persist_period_sec,
//               "Gap between persisting stats in seconds");
// DEFINE_bool(persist_stats_to_disk,
//             ROCKSDB_NAMESPACE::Options().persist_stats_to_disk,
//             "whether to persist stats to disk");
// DEFINE_uint64(stats_history_buffer_size,
//               ROCKSDB_NAMESPACE::Options().stats_history_buffer_size,
//               "Max number of stats snapshots to keep in memory");
// DEFINE_bool(avoid_flush_during_recovery,
//             ROCKSDB_NAMESPACE::Options().avoid_flush_during_recovery,
//             "If true, avoids flushing the recovered WAL data where
//             possible.");
// DEFINE_int64(multiread_stride, 0,
//              "Stride length for the keys in a MultiGet batch");
// DEFINE_bool(multiread_batched, false, "Use the new MultiGet API");

// DEFINE_string(memtablerep, "skip_list", "");
// DEFINE_int64(hash_bucket_count, 1024 * 1024, "hash bucket count");
// DEFINE_bool(use_plain_table, false,
//             "if use plain table instead of block-based table format");
// DEFINE_bool(use_cuckoo_table, false, "if use cuckoo table format");
// DEFINE_double(cuckoo_hash_ratio, 0.9, "Hash ratio for Cuckoo SST table.");
// DEFINE_bool(use_hash_search, false,
//             "if use kHashSearch instead of kBinarySearch. "
//             "This is valid if only we use BlockTable");
// DEFINE_string(merge_operator, "",
//               "The merge operator to use with the database."
//               "If a new merge operator is specified, be sure to use fresh"
//               " database The possible merge operators are defined in"
//               " utilities/merge_operators.h");
// DEFINE_int32(skip_list_lookahead, 0,
//              "Used with skip_list memtablerep; try linear search first for "
//              "this many steps from the previous position");
// DEFINE_bool(report_file_operations, false,
//             "if report number of file operations");
// DEFINE_bool(report_open_timing, false, "if report open timing");
// DEFINE_int32(readahead_size, 0, "Iterator readahead size");

// DEFINE_bool(read_with_latest_user_timestamp, true,
//             "If true, always use the current latest timestamp for read. If "
//             "false, choose a random timestamp from the past.");

// DEFINE_string(secondary_cache_uri, "",
//               "Full URI for creating a custom secondary cache object");
// static class std::shared_ptr<ROCKSDB_NAMESPACE::SecondaryCache>
// secondary_cache;

// static const bool FLAGS_prefix_size_dummy __attribute__((__unused__)) =
//     RegisterFlagValidator(&FLAGS_prefix_size, &ValidatePrefixSize);

// static const bool FLAGS_key_size_dummy __attribute__((__unused__)) =
//     RegisterFlagValidator(&FLAGS_key_size, &ValidateKeySize);

// static const bool FLAGS_cache_numshardbits_dummy __attribute__((__unused__))
// =
//     RegisterFlagValidator(&FLAGS_cache_numshardbits,
//                           &ValidateCacheNumshardbits);

// static const bool FLAGS_readwritepercent_dummy __attribute__((__unused__)) =
//     RegisterFlagValidator(&FLAGS_readwritepercent, &ValidateInt32Percent);

// DEFINE_int32(disable_seek_compaction, false,
//              "Not used, left here for backwards compatibility");

// DEFINE_bool(allow_data_in_errors,
//             ROCKSDB_NAMESPACE::Options().allow_data_in_errors,
//             "If true, allow logging data, e.g. key, value in LOG files.");

// static const bool FLAGS_deletepercent_dummy __attribute__((__unused__)) =
//     RegisterFlagValidator(&FLAGS_deletepercent, &ValidateInt32Percent);
// static const bool FLAGS_table_cache_numshardbits_dummy
//     __attribute__((__unused__)) = RegisterFlagValidator(
//         &FLAGS_table_cache_numshardbits, &ValidateTableCacheNumshardbits);

// DEFINE_uint32(write_batch_protection_bytes_per_key, 0,
//               "Size of per-key-value checksum in each write batch. Currently
//               " "only value 0 and 8 are supported.");

// DEFINE_uint32(
//     memtable_protection_bytes_per_key, 0,
//     "Enable memtable per key-value checksum protection. "
//     "Each entry in memtable will be suffixed by a per key-value checksum. "
//     "This options determines the size of such checksums. "
//     "Supported values: 0, 1, 2, 4, 8.");

// DEFINE_uint32(block_protection_bytes_per_key, 0,
//               "Enable block per key-value checksum protection. "
//               "Supported values: 0, 1, 2, 4, 8.");

// DEFINE_bool(build_info, false,
//             "Print the build info via GetRocksBuildInfoAsString");

// DEFINE_bool(track_and_verify_wals_in_manifest, false,
//             "If true, enable WAL tracking in the MANIFEST");

namespace HASHKV_NAMESPACE {

//! time相关
uint64_t NowMicros() {
  static constexpr uint64_t kUsecondsPerSecond = 1000000;
  struct ::timeval tv;
  ::gettimeofday(&tv, nullptr);
  return static_cast<uint64_t>(tv.tv_sec) * kUsecondsPerSecond + tv.tv_usec;
}
std::string TimeToString(uint64_t secondsSince1970) {
  const time_t seconds = (time_t)secondsSince1970;
  struct tm t;
  int maxsize = 64;
  std::string dummy;
  dummy.reserve(maxsize);
  dummy.resize(maxsize);
  char* p = &dummy[0];
  localtime_r(&seconds, &t);
  snprintf(p, maxsize, "%04d/%02d/%02d-%02d:%02d:%02d ", t.tm_year + 1900,
           t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return dummy;
}

//! DistributionType

enum DistributionType : unsigned char { kFixed = 0, kUniform, kNormal };

static enum DistributionType FLAGS_value_size_distribution_type_e = kFixed;

static enum DistributionType StringToDistributionType(const char* ctype) {
  assert(ctype);

  if (!strcasecmp(ctype, "fixed"))
    return kFixed;
  else if (!strcasecmp(ctype, "uniform"))
    return kUniform;
  else if (!strcasecmp(ctype, "normal"))
    return kNormal;

  fprintf(stdout, "Cannot parse distribution type '%s'\n", ctype);
  exit(1);
}

class BaseDistribution {
 public:
  BaseDistribution(unsigned int _min, unsigned int _max)
      : min_value_size_(_min), max_value_size_(_max) {}
  virtual ~BaseDistribution() {}

  unsigned int Generate() {
    auto val = Get();
    if (NeedTruncate()) {
      val = std::max(min_value_size_, val);
      val = std::min(max_value_size_, val);
    }
    return val;
  }

 private:
  virtual unsigned int Get() = 0;
  virtual bool NeedTruncate() { return true; }
  unsigned int min_value_size_;
  unsigned int max_value_size_;
};

class FixedDistribution : public BaseDistribution {
 public:
  FixedDistribution(unsigned int size)
      : BaseDistribution(size, size), size_(size) {}

 private:
  virtual unsigned int Get() override { return size_; }
  virtual bool NeedTruncate() override { return false; }
  unsigned int size_;
};

class NormalDistribution : public BaseDistribution,
                           public std::normal_distribution<double> {
 public:
  NormalDistribution(unsigned int _min, unsigned int _max)
      : BaseDistribution(_min, _max),
        // 99.7% values within the range [min, max].
        std::normal_distribution<double>(
            (double)(_min + _max) / 2.0 /*mean*/,
            (double)(_max - _min) / 6.0 /*stddev*/),
        gen_(rd_()) {}

 private:
  virtual unsigned int Get() override {
    return static_cast<unsigned int>((*this)(gen_));
  }
  std::random_device rd_;
  std::mt19937 gen_;
};

class UniformDistribution : public BaseDistribution,
                            public std::uniform_int_distribution<unsigned int> {
 public:
  UniformDistribution(unsigned int _min, unsigned int _max)
      : BaseDistribution(_min, _max),
        std::uniform_int_distribution<unsigned int>(_min, _max),
        gen_(rd_()) {}

 private:
  virtual unsigned int Get() override { return (*this)(gen_); }
  virtual bool NeedTruncate() override { return false; }
  std::random_device rd_;
  std::mt19937 gen_;
};

// Helper for quickly generating random data.
class RandomGenerator {
 private:
  std::string data_;
  unsigned int pos_;
  std::unique_ptr<BaseDistribution> dist_;

 public:
  RandomGenerator() {
    auto max_value_size = FLAGS_value_size_max;
    switch (FLAGS_value_size_distribution_type_e) {
      case kUniform:
        dist_.reset(new UniformDistribution(FLAGS_value_size_min,
                                            FLAGS_value_size_max));
        break;
      case kNormal:
        dist_.reset(
            new NormalDistribution(FLAGS_value_size_min, FLAGS_value_size_max));
        break;
      case kFixed:
      default:
        dist_.reset(new FixedDistribution(value_size));
        max_value_size = value_size;
    }
    // We use a limited amount of data over and over again and ensure
    // that it is larger than the compression window (32KB), and also
    // large enough to serve all typical value sizes we want to write.
    Random rnd(301);
    std::string piece;
    while (data_.size() < (unsigned)std::max(1048576, max_value_size)) {
      // Add a short fragment that is as compressible as specified
      // by FLAGS_compression_ratio.
      CompressibleString(&rnd, FLAGS_compression_ratio, 100, &piece);
      data_.append(piece);
    }
    pos_ = 0;
  }

  Slice CompressibleString(Random* rnd, double compressed_fraction, int len,
                           std::string* dst) {
    int raw = static_cast<int>(len * compressed_fraction);
    if (raw < 1) raw = 1;
    std::string raw_data = rnd->RandomBinaryString(raw);

    // Duplicate the random data until we have filled "len" bytes
    dst->clear();
    while (dst->size() < (unsigned int)len) {
      dst->append(raw_data);
    }
    dst->resize(len);
    return Slice(*dst);
  }

  Slice Generate(unsigned int len) {
    assert(len <= data_.size());
    if (pos_ + len > data_.size()) {
      pos_ = 0;
    }
    pos_ += len;
    return Slice(data_.data() + pos_ - len, len);
  }

  Slice Generate() {
    auto len = dist_->Generate();
    return Generate(len);
  }
};

static void AppendWithSpace(std::string* str, Slice msg) {
  if (msg.empty()) return;
  if (!str->empty()) {
    str->push_back(' ');
  }
  str->append(msg.data(), msg.size());
}

enum OperationType : unsigned char {
  kRead = 0,
  kWrite,
  kDelete,
  kSeek,
  kMerge,
  kUpdate,
  kCompress,
  kUncompress,
  kCrc,
  kHash,
  kOthers
};
static std::unordered_map<OperationType, std::string, std::hash<unsigned char>>
    OperationTypeString = {{kRead, "read"},         {kWrite, "write"},
                           {kDelete, "delete"},     {kSeek, "seek"},
                           {kMerge, "merge"},       {kUpdate, "update"},
                           {kCompress, "compress"}, {kCompress, "uncompress"},
                           {kCrc, "crc"},           {kHash, "hash"},
                           {kOthers, "op"}};

class CombinedStats;
class Stats {
 private:
  // SystemClock* clock_;
  int id_;
  uint64_t start_ = 0;
  uint64_t sine_interval_;
  uint64_t finish_;
  double seconds_;
  uint64_t done_;
  uint64_t last_report_done_;
  uint64_t next_report_;
  uint64_t bytes_;
  uint64_t last_op_finish_;
  uint64_t last_report_finish_;
  std::unordered_map<OperationType, std::shared_ptr<HistogramImpl>,
                     std::hash<unsigned char>>
      hist_;
  std::string message_;
  bool exclude_from_merge_;
  // ReporterAgent* reporter_agent_;  // does not own
  friend class CombinedStats;

 public:
  Stats() /*:clock_(FLAGS_env->GetSystemClock().get())*/ { Start(-1); }

  // void SetReporterAgent(ReporterAgent* reporter_agent) {
  //   reporter_agent_ = reporter_agent;
  // }

  void Start(int id) {
    id_ = id;
    next_report_ = FLAGS_stats_interval ? FLAGS_stats_interval : 100;
    last_op_finish_ = start_;
    hist_.clear();
    done_ = 0;
    last_report_done_ = 0;
    bytes_ = 0;
    seconds_ = 0;
    start_ = NowMicros();
    sine_interval_ = NowMicros();
    finish_ = start_;
    last_report_finish_ = start_;
    message_.clear();
    // When set, stats from this thread won't be merged with others.
    exclude_from_merge_ = false;
  }

  void Merge(const Stats& other) {
    if (other.exclude_from_merge_) return;

    for (auto it = other.hist_.begin(); it != other.hist_.end(); ++it) {
      auto this_it = hist_.find(it->first);
      if (this_it != hist_.end()) {
        this_it->second->Merge(*(other.hist_.at(it->first)));
      } else {
        hist_.insert({it->first, it->second});
      }
    }

    done_ += other.done_;
    bytes_ += other.bytes_;
    seconds_ += other.seconds_;
    if (other.start_ < start_) start_ = other.start_;
    if (other.finish_ > finish_) finish_ = other.finish_;

    // Just keep the messages from one thread.
    if (message_.empty()) message_ = other.message_;
  }

  void Stop() {
    finish_ = NowMicros();
    seconds_ = (finish_ - start_) * 1e-6;
  }

  void AddMessage(Slice msg) { AppendWithSpace(&message_, msg); }

  void SetId(int id) { id_ = id; }
  void SetExcludeFromMerge() { exclude_from_merge_ = true; }

  // void PrintThreadStatus() {
  //   std::vector<ThreadStatus> thread_list;
  //   FLAGS_env->GetThreadList(&thread_list);

  //   fprintf(stderr, "\n%18s %10s %12s %20s %13s %45s %12s %s\n", "ThreadID",
  //           "ThreadType", "cfName", "Operation", "ElapsedTime", "Stage",
  //           "State", "OperationProperties");

  //   int64_t current_time = 0;
  //   clock_->GetCurrentTime(&current_time).PermitUncheckedError();
  //   for (auto ts : thread_list) {
  //     fprintf(stderr, "%18" PRIu64 " %10s %12s %20s %13s %45s %12s",
  //             ts.thread_id,
  //             ThreadStatus::GetThreadTypeName(ts.thread_type).c_str(),
  //             ts.cf_name.c_str(),
  //             ThreadStatus::GetOperationName(ts.operation_type).c_str(),
  //             ThreadStatus::MicrosToString(ts.op_elapsed_micros).c_str(),
  //             ThreadStatus::GetOperationStageName(ts.operation_stage).c_str(),
  //             ThreadStatus::GetStateName(ts.state_type).c_str());

  //     auto op_properties = ThreadStatus::InterpretOperationProperties(
  //         ts.operation_type, ts.op_properties);
  //     for (const auto& op_prop : op_properties) {
  //       fprintf(stderr, " %s %" PRIu64 " |", op_prop.first.c_str(),
  //               op_prop.second);
  //     }
  //     fprintf(stderr, "\n");
  //   }
  // }

  void ResetSineInterval() { sine_interval_ = NowMicros(); }

  uint64_t GetSineInterval() { return sine_interval_; }

  uint64_t GetStart() { return start_; }

  void ResetLastOpTime() {
    // Set to now to avoid latency from calls to SleepForMicroseconds.
    last_op_finish_ = NowMicros();
  }

  void FinishedOps(/*DBWithColumnFamilies* db_with_cfh, DB* db,*/ int64_t num_ops,
                   enum OperationType op_type = kOthers) {
    // if (reporter_agent_) {
    //   reporter_agent_->ReportFinishedOps(num_ops);
    // }
    if (FLAGS_histogram) {
      uint64_t now = NowMicros();
      uint64_t micros = now - last_op_finish_;

      if (hist_.find(op_type) == hist_.end()) {
        auto hist_temp = std::make_shared<HistogramImpl>();
        hist_.insert({op_type, std::move(hist_temp)});
      }
      hist_[op_type]->Add(micros);

      if (micros >= FLAGS_slow_usecs && !FLAGS_stats_interval) {
        fprintf(stderr, "long op: %" PRIu64 " micros%30s\r", micros, "");
        fflush(stderr);
      }
      last_op_finish_ = now;
    }

    done_ += num_ops;
    if (done_ >= next_report_ && FLAGS_progress_reports) {
      if (!FLAGS_stats_interval) {
        if (next_report_ < 1000)
          next_report_ += 100;
        else if (next_report_ < 5000)
          next_report_ += 500;
        else if (next_report_ < 10000)
          next_report_ += 1000;
        else if (next_report_ < 50000)
          next_report_ += 5000;
        else if (next_report_ < 100000)
          next_report_ += 10000;
        else if (next_report_ < 500000)
          next_report_ += 50000;
        else
          next_report_ += 100000;
        fprintf(stderr, "... finished %" PRIu64 " ops%30s\r", done_, "");
      } else {
        uint64_t now = NowMicros();
        int64_t usecs_since_last = now - last_report_finish_;

        // Determine whether to print status where interval is either
        // each N operations or each N seconds.

        if (FLAGS_stats_interval_seconds &&
            usecs_since_last < (FLAGS_stats_interval_seconds * 1000000)) {
          // Don't check again for this many operations.
          next_report_ += FLAGS_stats_interval;

        } else {
          fprintf(stderr,
                  "%s ... thread %d: (%" PRIu64 ",%" PRIu64
                  ") ops and "
                  "(%.1f,%.1f) ops/second in (%.6f,%.6f) seconds\n",
                  TimeToString(now / 1000000).c_str(), id_,
                  done_ - last_report_done_, done_,
                  (done_ - last_report_done_) / (usecs_since_last / 1000000.0),
                  done_ / ((now - start_) / 1000000.0),
                  (now - last_report_finish_) / 1000000.0,
                  (now - start_) / 1000000.0);

          // if (id_ == 0 && FLAGS_stats_per_interval) {
          //   std::string stats;

          //   if (db_with_cfh && db_with_cfh->num_created.load()) {
          //     for (size_t i = 0; i < db_with_cfh->num_created.load(); ++i) {
          //       if (db->GetProperty(db_with_cfh->cfh[i], "rocksdb.cfstats",
          //                           &stats))
          //         fprintf(stderr, "%s\n", stats.c_str());
          //       if (FLAGS_show_table_properties) {
          //         for (int level = 0; level < FLAGS_num_levels; ++level) {
          //           if (db->GetProperty(
          //                   db_with_cfh->cfh[i],
          //                   "rocksdb.aggregated-table-properties-at-level" +
          //                       std::to_string(level),
          //                   &stats)) {
          //             if (stats.find("# entries=0") == std::string::npos) {
          //               fprintf(stderr, "Level[%d]: %s\n", level,
          //                       stats.c_str());
          //             }
          //           }
          //         }
          //       }
          //     }
          //   } else if (db) {
          //     if (db->GetProperty("rocksdb.stats", &stats)) {
          //       fprintf(stderr, "%s", stats.c_str());
          //     }
          //     if (db->GetProperty("rocksdb.num-running-compactions", &stats))
          //     {
          //       fprintf(stderr, "num-running-compactions: %s\n",
          //       stats.c_str());
          //     }
          //     if (db->GetProperty("rocksdb.num-running-flushes", &stats)) {
          //       fprintf(stderr, "num-running-flushes: %s\n\n",
          //       stats.c_str());
          //     }
          //     if (FLAGS_show_table_properties) {
          //       for (int level = 0; level < FLAGS_num_levels; ++level) {
          //         if (db->GetProperty(
          //                 "rocksdb.aggregated-table-properties-at-level" +
          //                     std::to_string(level),
          //                 &stats)) {
          //           if (stats.find("# entries=0") == std::string::npos) {
          //             fprintf(stderr, "Level[%d]: %s\n", level,
          //             stats.c_str());
          //           }
          //         }
          //       }
          //     }
          //   }
          // }

          next_report_ += FLAGS_stats_interval;
          last_report_finish_ = now;
          last_report_done_ = done_;
        }
      }
      // if (id_ == 0 && FLAGS_thread_status_per_interval) {
      //   PrintThreadStatus();
      // }
      fflush(stderr);
    }
  }

  void AddBytes(int64_t n) { bytes_ += n; }

  void Report(const std::string& name) {
    // Pretend at least one op was done in case we are running a benchmark
    // that does not call FinishedOps().
    if (done_ < 1) done_ = 1;

    std::string extra;
    double elapsed = (finish_ - start_) * 1e-6;
    if (bytes_ > 0) {
      // Rate is computed on actual elapsed time, not the sum of per-thread
      // elapsed times.
      char rate[100];
      snprintf(rate, sizeof(rate), "%6.1f MB/s",
               (bytes_ / 1048576.0) / elapsed);
      extra = rate;
    }
    AppendWithSpace(&extra, message_);
    double throughput = (double)done_ / elapsed;

    fprintf(stdout,
            "%-12s : %11.3f micros/op %ld ops/sec %.3f seconds %" PRIu64
            " operations;%s%s\n",
            name.c_str(), seconds_ * 1e6 / done_, (long)throughput, elapsed,
            done_, (extra.empty() ? "" : " "), extra.c_str());
    if (FLAGS_histogram) {
      for (auto it = hist_.begin(); it != hist_.end(); ++it) {
        fprintf(stdout, "Microseconds per %s:\n%s\n",
                OperationTypeString[it->first].c_str(),
                it->second->ToString().c_str());
      }
    }
    // if (FLAGS_report_file_operations) {
    //   auto* counted_fs =
    //       FLAGS_env->GetFileSystem()->CheckedCast<CountedFileSystem>();
    //   assert(counted_fs);
    //   fprintf(stdout, "%s", counted_fs->PrintCounters().c_str());
    //   counted_fs->ResetCounters();
    // }
    fflush(stdout);
  }
};

class CombinedStats {
 public:
  void AddStats(const Stats& stat) {
    uint64_t total_ops = stat.done_;
    uint64_t total_bytes_ = stat.bytes_;
    double elapsed;

    if (total_ops < 1) {
      total_ops = 1;
    }

    elapsed = (stat.finish_ - stat.start_) * 1e-6;
    throughput_ops_.emplace_back(total_ops / elapsed);

    if (total_bytes_ > 0) {
      double mbs = (total_bytes_ / 1048576.0);
      throughput_mbs_.emplace_back(mbs / elapsed);
    }
  }

  void Report(const std::string& bench_name) {
    if (throughput_ops_.size() < 2) {
      // skip if there are not enough samples
      return;
    }

    const char* name = bench_name.c_str();
    int num_runs = static_cast<int>(throughput_ops_.size());

    if (throughput_mbs_.size() == throughput_ops_.size()) {
      fprintf(stdout,
              "%s [AVG %d runs] : %d (\xC2\xB1 %d) ops/sec; %6.1f (\xC2\xB1 "
              "%.1f) MB/sec\n",
              name, num_runs, static_cast<int>(CalcAvg(throughput_ops_)),
              static_cast<int>(CalcConfidence95(throughput_ops_)),
              CalcAvg(throughput_mbs_), CalcConfidence95(throughput_mbs_));
    } else {
      fprintf(stdout, "%s [AVG %d runs] : %d (\xC2\xB1 %d) ops/sec\n", name,
              num_runs, static_cast<int>(CalcAvg(throughput_ops_)),
              static_cast<int>(CalcConfidence95(throughput_ops_)));
    }
  }

  void ReportWithConfidenceIntervals(const std::string& bench_name) {
    if (throughput_ops_.size() < 2) {
      // skip if there are not enough samples
      return;
    }

    const char* name = bench_name.c_str();
    int num_runs = static_cast<int>(throughput_ops_.size());

    int ops_avg = static_cast<int>(CalcAvg(throughput_ops_));
    int ops_confidence_95 = static_cast<int>(CalcConfidence95(throughput_ops_));

    if (throughput_mbs_.size() == throughput_ops_.size()) {
      double mbs_avg = CalcAvg(throughput_mbs_);
      double mbs_confidence_95 = CalcConfidence95(throughput_mbs_);
      fprintf(stdout,
              "%s [CI95 %d runs] : (%d, %d) ops/sec; (%.1f, %.1f) MB/sec\n",
              name, num_runs, ops_avg - ops_confidence_95,
              ops_avg + ops_confidence_95, mbs_avg - mbs_confidence_95,
              mbs_avg + mbs_confidence_95);
    } else {
      fprintf(stdout, "%s [CI95 %d runs] : (%d, %d) ops/sec\n", name, num_runs,
              ops_avg - ops_confidence_95, ops_avg + ops_confidence_95);
    }
  }

  void ReportFinal(const std::string& bench_name) {
    if (throughput_ops_.size() < 2) {
      // skip if there are not enough samples
      return;
    }

    const char* name = bench_name.c_str();
    int num_runs = static_cast<int>(throughput_ops_.size());

    if (throughput_mbs_.size() == throughput_ops_.size()) {
      // \xC2\xB1 is +/- character in UTF-8
      fprintf(stdout,
              "%s [AVG    %d runs] : %d (\xC2\xB1 %d) ops/sec; %6.1f (\xC2\xB1 "
              "%.1f) MB/sec\n"
              "%s [MEDIAN %d runs] : %d ops/sec; %6.1f MB/sec\n",
              name, num_runs, static_cast<int>(CalcAvg(throughput_ops_)),
              static_cast<int>(CalcConfidence95(throughput_ops_)),
              CalcAvg(throughput_mbs_), CalcConfidence95(throughput_mbs_), name,
              num_runs, static_cast<int>(CalcMedian(throughput_ops_)),
              CalcMedian(throughput_mbs_));
    } else {
      fprintf(stdout,
              "%s [AVG    %d runs] : %d (\xC2\xB1 %d) ops/sec\n"
              "%s [MEDIAN %d runs] : %d ops/sec\n",
              name, num_runs, static_cast<int>(CalcAvg(throughput_ops_)),
              static_cast<int>(CalcConfidence95(throughput_ops_)), name,
              num_runs, static_cast<int>(CalcMedian(throughput_ops_)));
    }
  }

 private:
  double CalcAvg(std::vector<double>& data) {
    double avg = 0;
    for (double x : data) {
      avg += x;
    }
    avg = avg / data.size();
    return avg;
  }

  // Calculates 95% CI assuming a normal distribution of samples.
  // Samples are not from a normal distribution, but it still
  // provides useful approximation.
  double CalcConfidence95(std::vector<double>& data) {
    assert(data.size() > 1);
    double avg = CalcAvg(data);
    double std_error = CalcStdDev(data, avg) / std::sqrt(data.size());

    // Z score for the 97.5 percentile
    // see https://en.wikipedia.org/wiki/1.96
    return 1.959964 * std_error;
  }

  double CalcMedian(std::vector<double>& data) {
    assert(data.size() > 0);
    std::sort(data.begin(), data.end());

    size_t mid = data.size() / 2;
    if (data.size() % 2 == 1) {
      // Odd number of entries
      return data[mid];
    } else {
      // Even number of entries
      return (data[mid] + data[mid - 1]) / 2;
    }
  }

  double CalcStdDev(std::vector<double>& data, double average) {
    assert(data.size() > 1);
    double squared_sum = 0.0;
    for (double x : data) {
      squared_sum += std::pow(x - average, 2);
    }

    // using samples count - 1 following Bessel's correction
    // see https://en.wikipedia.org/wiki/Bessel%27s_correction
    return std::sqrt(squared_sum / (data.size() - 1));
  }

  std::vector<double> throughput_ops_;
  std::vector<double> throughput_mbs_;
};

// State shared by all concurrent executions of the same benchmark.
struct SharedState {
  Mutex mu;
  CondVar cv;
  // std::mutex mu;
  int total;
  // int perf_level;
  // std::shared_ptr<RateLimiter> write_rate_limiter;
  // std::shared_ptr<RateLimiter> read_rate_limiter;

  // Each thread goes through the following states:
  //    (1) initializing
  //    (2) waiting for others to be initialized
  //    (3) running
  //    (4) done

  long num_initialized;
  long num_done;
  bool start;

  SharedState() : cv(&mu) /*, perf_level(FLAGS_perf_level)*/ {}
};

// Per-thread state for concurrent executions of the same benchmark.
struct ThreadState {
  int tid;        // 0..n-1 when running in n threads
  Random64 rand;  // Has different seeds for different threads
  Stats stats;
  SharedState* shared;

  explicit ThreadState(int index, int my_seed)
      : tid(index), rand(*seed_base + my_seed) {}
};

class Duration {
 public:
  Duration(uint64_t max_seconds, int64_t max_ops, int64_t ops_per_stage = 0) {
    max_seconds_ = max_seconds;
    max_ops_ = max_ops;
    ops_per_stage_ = (ops_per_stage > 0) ? ops_per_stage : max_ops;
    ops_ = 0;
    start_at_ = NowMicros();
  }

  int64_t GetStage() { return std::min(ops_, max_ops_ - 1) / ops_per_stage_; }

  bool Done(int64_t increment) {
    if (increment <= 0) increment = 1;  // avoid Done(0) and infinite loops
    ops_ += increment;

    if (max_seconds_) {
      // Recheck every appx 1000 ops (exact iff increment is factor of 1000)
      auto granularity = FLAGS_ops_between_duration_checks;
      if ((ops_ / granularity) != ((ops_ - increment) / granularity)) {
        uint64_t now = NowMicros();
        return ((now - start_at_) / 1000000) >= max_seconds_;
      } else {
        return false;
      }
    } else {
      return ops_ > max_ops_;
    }
  }

 private:
  uint64_t max_seconds_;
  int64_t max_ops_;
  int64_t ops_per_stage_;
  int64_t ops_;
  uint64_t start_at_;
};

class Benchmark {
 private:
  int64_t num_;
  int key_size_;
  int64_t reads_;
  int64_t deletes_;
  int64_t writes_;
  double read_random_exp_range_;
  // int64_t entries_per_batch_;
  int total_thread_count_;
  std::shared_ptr<DeviceManager> diskManager_;
  std::shared_ptr<KvServer> kvserver_;
  std::vector<std::string> keys_;

// Current the following isn't equivalent to OS_LINUX.
#if defined(__linux)
  static Slice TrimSpace(Slice s) {
    unsigned int start = 0;
    while (start < s.size() && isspace(s[start])) {
      start++;
    }
    unsigned int limit = static_cast<unsigned int>(s.size());
    while (limit > start && isspace(s[limit - 1])) {
      limit--;
    }
    return Slice(s.data() + start, limit - start);
  }
#endif

  void PrintEnvironment() {
    fprintf(stderr, "KV bench\n");
#if defined(__linux) || defined(__APPLE__) || defined(__FreeBSD__)
    time_t now = time(nullptr);
    char buf[52];
    // Lint complains about ctime() usage, so replace it with ctime_r(). The
    // requirement is to provide a buffer which is at least 26 bytes.
    fprintf(stderr, "Date:       %s",
            ctime_r(&now, buf));  // ctime_r() adds newline

#if defined(__linux)
    FILE* cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != nullptr) {
      char line[1000];
      int num_cpus = 0;
      std::string cpu_type;
      std::string cache_size;
      while (fgets(line, sizeof(line), cpuinfo) != nullptr) {
        const char* sep = strchr(line, ':');
        if (sep == nullptr) {
          continue;
        }
        Slice key = TrimSpace(Slice(line, sep - 1 - line));
        Slice val = TrimSpace(Slice(sep + 1));
        if (key == "model name") {
          ++num_cpus;
          cpu_type = val.ToString();
        } else if (key == "cache size") {
          cache_size = val.ToString();
        }
      }
      fclose(cpuinfo);
      fprintf(stderr, "CPU:        %d * %s\n", num_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#elif defined(__APPLE__)
    struct host_basic_info h;
    size_t hlen = HOST_BASIC_INFO_COUNT;
    if (host_info(mach_host_self(), HOST_BASIC_INFO, (host_info_t)&h,
                  (uint32_t*)&hlen) == KERN_SUCCESS) {
      std::string cpu_type;
      std::string cache_size;
      size_t hcache_size;
      hlen = sizeof(hcache_size);
      if (sysctlbyname("hw.cachelinesize", &hcache_size, &hlen, NULL, 0) == 0) {
        cache_size = std::to_string(hcache_size);
      }
      switch (h.cpu_type) {
        case CPU_TYPE_X86_64:
          cpu_type = "x86_64";
          break;
        case CPU_TYPE_ARM64:
          cpu_type = "arm64";
          break;
        default:
          break;
      }
      fprintf(stderr, "CPU:        %d * %s\n", h.max_cpus, cpu_type.c_str());
      fprintf(stderr, "CPUCache:   %s\n", cache_size.c_str());
    }
#elif defined(__FreeBSD__)
    int ncpus;
    size_t len = sizeof(ncpus);
    int mib[2] = {CTL_HW, HW_NCPU};
    if (sysctl(mib, 2, &ncpus, &len, nullptr, 0) == 0) {
      char cpu_type[16];
      len = sizeof(cpu_type) - 1;
      mib[1] = HW_MACHINE;
      if (sysctl(mib, 2, cpu_type, &len, nullptr, 0) == 0) cpu_type[len] = 0;

      fprintf(stderr, "CPU:        %d * %s\n", ncpus, cpu_type);
      // no programmatic way to get the cache line size except on PPC
    }
#endif
#endif
  }

  bool SanityCheck() {
    if (FLAGS_compression_ratio > 1) {
      fprintf(stderr, "compression_ratio should be between 0 and 1\n");
      return false;
    }
    return true;
  }

  void PrintHeader() {
    PrintEnvironment();
    fprintf(stdout, "Keys:       %d bytes each\n", FLAGS_key_size);
    auto avg_value_size = FLAGS_value_size;
    if (FLAGS_value_size_distribution_type_e == kFixed) {
      fprintf(stdout,
              "Values:     %d bytes each (%d bytes after compression)\n",
              avg_value_size,
              static_cast<int>(avg_value_size * FLAGS_compression_ratio + 0.5));
    } else {
      avg_value_size = (FLAGS_value_size_min + FLAGS_value_size_max) / 2;
      fprintf(stdout,
              "Values:     %d avg bytes each (%d bytes after compression)\n",
              avg_value_size,
              static_cast<int>(avg_value_size * FLAGS_compression_ratio + 0.5));
      fprintf(stdout, "Values Distribution: %s (min: %d, max: %d)\n",
              FLAGS_value_size_distribution_type.c_str(), FLAGS_value_size_min,
              FLAGS_value_size_max);
    }
    fprintf(stdout, "Entries:    %" PRIu64 "\n", num_);
    // fprintf(stdout, "Prefix:    %d bytes\n", FLAGS_prefix_size);
    // fprintf(stdout, "Keys per prefix:    %" PRIu64 "\n", keys_per_prefix_);
    fprintf(stdout, "RawSize:    %.1f MB (estimated)\n",
            ((static_cast<int64_t>(FLAGS_key_size + avg_value_size) * num_) /
             1048576.0));
    fprintf(
        stdout, "FileSize:   %.1f MB (estimated)\n",
        (((FLAGS_key_size + avg_value_size * FLAGS_compression_ratio) * num_) /
         1048576.0));
    // fprintf(stdout, "Write rate: %" PRIu64 " bytes/second\n",
    //         FLAGS_benchmark_write_rate_limit);
    // fprintf(stdout, "Read rate: %" PRIu64 " ops/second\n",
    //         FLAGS_benchmark_read_rate_limit);

    // auto compression = CompressionTypeToString(FLAGS_compression_type_e);
    // fprintf(stdout, "Compression: %s\n", compression.c_str());
    // fprintf(stdout, "Compression sampling rate: %" PRId64 "\n",
    //         FLAGS_sample_for_compression);
    // fprintf(stdout, "Perf Level: %d\n", FLAGS_perf_level);

    // PrintWarnings(compression.c_str());
    fprintf(stdout, "------------------------------------------------\n");
  }

  enum WriteMode { RANDOM, SEQUENTIAL, UNIQUE_RANDOM };
  void WriteSeq(ThreadState* thread) { DoWrite(thread, SEQUENTIAL); }

  void WriteRandom(ThreadState* thread) { DoWrite(thread, RANDOM); }

  int64_t GetRandomKey(Random64* rand) {
    uint64_t rand_int = rand->Next();
    int64_t key_rand;
    if (read_random_exp_range_ == 0) {
      key_rand = rand_int % FLAGS_num;
    } else {
      const uint64_t kBigInt = static_cast<uint64_t>(1U) << 62;
      long double order = -static_cast<long double>(rand_int % kBigInt) /
                          static_cast<long double>(kBigInt) *
                          read_random_exp_range_;
      long double exp_ran = std::exp(order);
      uint64_t rand_num =
          static_cast<int64_t>(exp_ran * static_cast<long double>(FLAGS_num));
      // Map to a different number to avoid locality.
      const uint64_t kBigPrime = 0x5bd1e995;
      // Overflow is like %(2^64). Will have little impact of results.
      key_rand = static_cast<int64_t>((rand_num * kBigPrime) % FLAGS_num);
    }
    return key_rand;
  }

  void ReadRandom(ThreadState* thread) {
    int64_t read = 0;
    int64_t found = 0;
    int64_t bytes = 0;
    int num_keys = 0;
    int64_t key_rand = 0;
    // ReadOptions options = read_options_;
    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);
    PinnableSlice pinnable_val;

    Duration duration(FLAGS_duration, reads_);
    while (!duration.Done(1)) {
      // DBWithColumnFamilies* db_with_cfh = SelectDBWithCfh(thread);
      key_rand = GetRandomKey(&thread->rand);
      GenerateKeyFromInt(key_rand, FLAGS_num, &key);
      read++;
      // Status s;
      pinnable_val.Reset();

      // ColumnFamilyHandle* cfh;
      // s = db_with_cfh->db->Get(options, cfh, key, &pinnable_val, ts_ptr);

      //TODO
      // if (s.ok()) {
      //   found++;
      //   bytes += key.size() + pinnable_val.size();
      // } else if (!s.IsNotFound()) {
      //   fprintf(stderr, "Get returned an error: %s\n", s.ToString().c_str());
      //   abort();
      // }

      thread->stats.FinishedOps(1, kRead);
    }

    char msg[100];
    snprintf(msg, sizeof(msg), "(%" PRIu64 " of %" PRIu64 " found)\n", found,
             read);

    thread->stats.AddBytes(bytes);
    thread->stats.AddMessage(msg);
  }

  class KeyGenerator {
   public:
    KeyGenerator(Random64* rand, WriteMode mode, uint64_t num,
                 uint64_t /*num_per_set*/ = 64 * 1024)
        : rand_(rand), mode_(mode), num_(num), next_(0) {
      if (mode_ == UNIQUE_RANDOM) {
        // NOTE: if memory consumption of this approach becomes a concern,
        // we can either break it into pieces and only random shuffle a section
        // each time. Alternatively, use a bit map implementation
        // (https://reviews.facebook.net/differential/diff/54627/)
        values_.resize(num_);
        for (uint64_t i = 0; i < num_; ++i) {
          values_[i] = i;
        }
        RandomShuffle(values_.begin(), values_.end(),
                      static_cast<uint32_t>(*seed_base));
      }
    }

    uint64_t Next() {
      switch (mode_) {
        case SEQUENTIAL:
          return next_++;
        case RANDOM:
          return rand_->Next() % num_;
        case UNIQUE_RANDOM:
          assert(next_ < num_);
          return values_[next_++];
      }
      assert(false);
      return std::numeric_limits<uint64_t>::max();
    }

    // Only available for UNIQUE_RANDOM mode.
    uint64_t Fetch(uint64_t index) {
      assert(mode_ == UNIQUE_RANDOM);
      assert(index < values_.size());
      return values_[index];
    }

   private:
    Random64* rand_;
    WriteMode mode_;
    const uint64_t num_;
    uint64_t next_;
    std::vector<uint64_t> values_;
  };

  //! thread
  struct ThreadArg {
    Benchmark* bm;
    SharedState* shared;
    ThreadState* thread;
    void (Benchmark::*method)(ThreadState*);
  };

  static void ThreadBody(void* v) {
    ThreadArg* arg = reinterpret_cast<ThreadArg*>(v);
    SharedState* shared = arg->shared;
    ThreadState* thread = arg->thread;
    {
      MutexLock l(&shared->mu);
      shared->num_initialized++;
      if (shared->num_initialized >= shared->total) {
        shared->cv.SignalAll();
      }
      while (!shared->start) {
        shared->cv.Wait();
      }
    }

    // SetPerfLevel(static_cast<PerfLevel>(shared->perf_level));
    // perf_context.EnablePerLevelPerfContext();
    thread->stats.Start(thread->tid);
    (arg->bm->*(arg->method))(thread);
    // if (FLAGS_perf_level > ROCKSDB_NAMESPACE::PerfLevel::kDisable) {
    //   thread->stats.AddMessage(std::string("PERF_CONTEXT:\n") +
    //                            get_perf_context()->ToString());
    // }
    thread->stats.Stop();

    {
      MutexLock l(&shared->mu);
      shared->num_done++;
      if (shared->num_done >= shared->total) {
        shared->cv.SignalAll();
      }
    }
  }

  Stats RunBenchmark(int n, std::string name,
                     void (Benchmark::*method)(ThreadState*)) {
    SharedState shared;
    shared.total = n;
    shared.num_initialized = 0;
    shared.num_done = 0;
    shared.start = false;
    ThreadArg* arg = new ThreadArg[n];

    for (int i = 0; i < n; i++) {
      arg[i].bm = this;
      arg[i].method = method;
      arg[i].shared = &shared;
      total_thread_count_++;
      arg[i].thread = new ThreadState(i, total_thread_count_);
      // arg[i].thread->stats.SetReporterAgent(reporter_agent.get());
      arg[i].thread->shared = &shared;
      // FLAGS_env->StartThread(ThreadBody, &arg[i]);
      std::thread new_thread(ThreadBody, &arg[i]);
      new_thread.detach();
    }

    shared.mu.Lock();
    while (shared.num_initialized < n) {
      shared.cv.Wait();
    }

    shared.start = true;
    shared.cv.SignalAll();
    while (shared.num_done < n) {
      shared.cv.Wait();
    }
    shared.mu.Unlock();

    // Stats for some threads can be excluded.
    Stats merge_stats;
    for (int i = 0; i < n; i++) {
      merge_stats.Merge(arg[i].thread->stats);
    }
    merge_stats.Report(name);

    for (int i = 0; i < n; i++) {
      delete arg[i].thread;
    }
    delete[] arg;

    return merge_stats;
  }

  void DoWrite(ThreadState* thread, WriteMode write_mode) {
    const int test_duration = write_mode == RANDOM ? FLAGS_duration : 0;
    const int64_t num_ops = writes_ == 0 ? num_ : writes_;

    // size_t num_key_gens = 1;
    // if (db_.db == nullptr) {
    //   num_key_gens = multi_dbs_.size();
    // }
    // std::vector<std::unique_ptr<KeyGenerator>> key_gens(num_key_gens);
    std::unique_ptr<KeyGenerator> key_gen;
    int64_t max_ops = num_ops;
    int64_t ops_per_stage = max_ops;
    // if (FLAGS_num_column_families > 1 && FLAGS_num_hot_column_families > 0) {
    //   ops_per_stage = (max_ops - 1) / (FLAGS_num_column_families /
    //                                    FLAGS_num_hot_column_families) +
    //                   1;
    // }

    Duration duration(test_duration, max_ops, ops_per_stage);
    const uint64_t num_per_key_gen = num_ /*+ max_num_range_tombstones_*/;
    // for (size_t i = 0; i < num_key_gens; i++) {
    //   key_gens[i].reset(new KeyGenerator(&(thread->rand), write_mode,
    //                                      num_per_key_gen, ops_per_stage));
    // }
    key_gen.reset(new KeyGenerator(&(thread->rand), write_mode, num_per_key_gen,
                                   ops_per_stage));

    if (num_ != FLAGS_num) {
      char msg[100];
      snprintf(msg, sizeof(msg), "(%" PRIu64 " ops)", num_);
      thread->stats.AddMessage(msg);
    }

    RandomGenerator gen;
    // WriteBatch batch(/*reserved_bytes=*/0, /*max_bytes=*/0,
    //                  FLAGS_write_batch_protection_bytes_per_key,
    //                  user_timestamp_size_);
    // Status s;
    int64_t bytes = 0;

    std::unique_ptr<const char[]> key_guard;
    Slice key = AllocateKey(&key_guard);
    double p = 0.0;
    uint64_t num_overwrites = 0, num_unique_keys = 0;
    // If user set overwrite_probability flag,
    // check if value is in [0.0,1.0].
    if (FLAGS_overwrite_probability > 0.0) {
      p = FLAGS_overwrite_probability > 1.0 ? 1.0 : FLAGS_overwrite_probability;
      // If overwrite set by user, and UNIQUE_RANDOM mode on,
      // the overwrite_window_size must be > 0.
      if (write_mode == UNIQUE_RANDOM && FLAGS_overwrite_window_size == 0) {
        fprintf(stderr,
                "Overwrite_window_size must be  strictly greater than 0.\n");
        ErrorExit();
      }
    }

    // Default_random_engine provides slightly
    // improved throughput over mt19937.
    std::default_random_engine overwrite_gen{
        static_cast<unsigned int>(*seed_base)};
    std::bernoulli_distribution overwrite_decider(p);

    // Inserted key window is filled with the last N
    // keys previously inserted into the DB (with
    // N=FLAGS_overwrite_window_size).
    // We use a deque struct because:
    // - random access is O(1)
    // - insertion/removal at beginning/end is also O(1).
    std::deque<int64_t> inserted_key_window;
    Random64 reservoir_id_gen(*seed_base);

    // --- Variables used in disposable/persistent keys simulation:
    // The following variables are used when
    // disposable_entries_batch_size is >0. We simualte a workload
    // where the following sequence is repeated multiple times:
    // "A set of keys S1 is inserted ('disposable entries'), then after
    // some delay another set of keys S2 is inserted ('persistent entries')
    // and the first set of keys S1 is deleted. S2 artificially represents
    // the insertion of hypothetical results from some undefined computation
    // done on the first set of keys S1. The next sequence can start as soon
    // as the last disposable entry in the set S1 of this sequence is
    // inserted, if the delay is non negligible"
    // bool skip_for_loop = false, is_disposable_entry = true;
    // std::vector<uint64_t> disposable_entries_index(num_key_gens, 0);
    // std::vector<uint64_t> persistent_ent_and_del_index(num_key_gens, 0);
    // const uint64_t kNumDispAndPersEntries =
    //     FLAGS_disposable_entries_batch_size +
    //     FLAGS_persistent_entries_batch_size;
    // if (kNumDispAndPersEntries > 0) {
    //   if ((write_mode != UNIQUE_RANDOM) || (writes_per_range_tombstone_ > 0)
    //   ||
    //       (p > 0.0)) {
    //     fprintf(
    //         stderr,
    //         "Disposable/persistent deletes are not compatible with overwrites
    //         " "and DeleteRanges; and are only supported in
    //         filluniquerandom.\n");
    //     ErrorExit();
    //   }
    //   if (FLAGS_disposable_entries_value_size < 0 ||
    //       FLAGS_persistent_entries_value_size < 0) {
    //     fprintf(
    //         stderr,
    //         "disposable_entries_value_size and persistent_entries_value_size"
    //         "have to be positive.\n");
    //     ErrorExit();
    //   }
    // }
    // Random rnd_disposable_entry(static_cast<uint32_t>(*seed_base));
    // std::string random_value;
    // Queue that stores scheduled timestamp of disposable entries deletes,
    // along with starting index of disposable entry keys to delete.
    // std::vector<std::queue<std::pair<uint64_t, uint64_t>>>
    // disposable_entries_q(
    //     num_key_gens);
    // --- End of variables used in disposable/persistent keys simulation.

    // std::vector<std::unique_ptr<const char[]>> expanded_key_guards;
    // std::vector<Slice> expanded_keys;
    // if (FLAGS_expand_range_tombstones) {
    //   expanded_key_guards.resize(range_tombstone_width_);
    //   for (auto& expanded_key_guard : expanded_key_guards) {
    //     expanded_keys.emplace_back(AllocateKey(&expanded_key_guard));
    //   }
    // }

    // std::unique_ptr<char[]> ts_guard;
    // if (user_timestamp_size_ > 0) {
    //   ts_guard.reset(new char[user_timestamp_size_]);
    // }

    int64_t stage = 0;
    // int64_t num_written = 0;
    // int64_t next_seq_db_at = num_ops;
    // size_t id = 0;
    // int64_t num_range_deletions = 0;

    while ((num_per_key_gen != 0) && !duration.Done(1 /*entries_per_batch_*/)) {
      if (duration.GetStage() != stage) {
        stage = duration.GetStage();

        // TODO
        // if (db_.db != nullptr) {
        //   db_.CreateNewCf(open_options_, stage);
        // } else {
        //   for (auto& db : multi_dbs_) {
        //     db.CreateNewCf(open_options_, stage);
        //   }
        // }
      }

      // for (int64_t j = 0; j < 1 /*entries_per_batch_*/; j++) {
      int64_t rand_num = 0;
      if ((write_mode == UNIQUE_RANDOM) && (p > 0.0)) {
        if ((inserted_key_window.size() > 0) &&
            overwrite_decider(overwrite_gen)) {
          num_overwrites++;
          rand_num = inserted_key_window[reservoir_id_gen.Next() %
                                         inserted_key_window.size()];
        } else {
          num_unique_keys++;
          rand_num = key_gen->Next();
          if (inserted_key_window.size() < FLAGS_overwrite_window_size) {
            inserted_key_window.push_back(rand_num);
          } else {
            inserted_key_window.pop_front();
            inserted_key_window.push_back(rand_num);
          }
        }
      } else {
        rand_num = key_gen->Next();
      }
      GenerateKeyFromInt(rand_num, FLAGS_num, &key);
      Slice val;
      val = gen.Generate();
      // }
      // 写db
      bool ret = kvserver_->putValue(const_cast<char*>(key.data()), key.size(),
                                     const_cast<char*>(val.data()), val.size());
      thread->stats.FinishedOps(1 /*entries_per_batch_*/, kWrite);
      if (!ret) {
        fprintf(stderr, "put error\n");
        ErrorExit();
      }
    }
    if ((write_mode == UNIQUE_RANDOM) && (p > 0.0)) {
      fprintf(stdout,
              "Number of unique keys inserted: %" PRIu64
              ".\nNumber of overwrites: %" PRIu64 "\n",
              num_unique_keys, num_overwrites);
    }
    thread->stats.AddBytes(bytes);
  }

 public:
  Benchmark()
      : num_(FLAGS_num),
        key_size_(FLAGS_key_size),
        reads_(FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads),
        writes_(FLAGS_writes < 0 ? FLAGS_num : FLAGS_writes),
        read_random_exp_range_(0.0),
        total_thread_count_(0),
        diskManager_(nullptr),
        kvserver_(nullptr) {
    // data disks
    // DiskInfo disk1(0, FLAGS_db, 1024*1024*1024);
    // std::vector<DiskInfo> disks;
    // disks.push_back(disk1);
    // diskManager = new DeviceManager(disks);

    // std::shared_ptr<DeviceManager> diskManager;
    // std::shared_ptr<KvServer> kvserver;
    // TODO
    // if (!FLAGS_use_existing_db) {
    //   Options options;
    //   options.env = FLAGS_env;
    //   if (!FLAGS_wal_dir.empty()) {
    //     options.wal_dir = FLAGS_wal_dir;
    //   }
    //   if (use_blob_db_) {
    //     // Stacked BlobDB
    //     blob_db::DestroyBlobDB(FLAGS_db, options, blob_db::BlobDBOptions());
    //   }
    //   DestroyDB(FLAGS_db, options);
    //   if (!FLAGS_wal_dir.empty()) {
    //     FLAGS_env->DeleteDir(FLAGS_wal_dir);
    //   }

    //   if (FLAGS_num_multi_db > 1) {
    //     FLAGS_env->CreateDir(FLAGS_db);
    //     if (!FLAGS_wal_dir.empty()) {
    //       FLAGS_env->CreateDir(FLAGS_wal_dir);
    //     }
    //   }
    // }
  }

  ~Benchmark() {
    // TODO:DeleteDBs();
  }

  Slice AllocateKey(std::unique_ptr<const char[]>* key_guard) {
    char* data = new char[key_size_];
    const char* const_data = data;
    key_guard->reset(const_data);
    return Slice(key_guard->get(), key_size_);
  }

  // Generate key according to the given specification and random number.
  // The resulting key will have the following format:
  //   - If keys_per_prefix_ is positive, extra trailing bytes are either cut
  //     off or padded with '0'.
  //     The prefix value is derived from key value.
  //     ----------------------------
  //     | prefix 00000 | key 00000 |
  //     ----------------------------
  //
  //   - If keys_per_prefix_ is 0, the key is simply a binary representation of
  //     random number followed by trailing '0's
  //     ----------------------------
  //     |        key 00000         |
  //     ----------------------------
  void GenerateKeyFromInt(uint64_t v, int64_t num_keys, Slice* key) {
    if (!keys_.empty()) {
      assert(FLAGS_use_existing_keys);
      assert(keys_.size() == static_cast<size_t>(num_keys));
      assert(v < static_cast<uint64_t>(num_keys));
      *key = keys_[v];
      return;
    }
    char* start = const_cast<char*>(key->data());
    char* pos = start;
    // if (keys_per_prefix_ > 0) {
    //   int64_t num_prefix = num_keys / keys_per_prefix_;
    //   int64_t prefix = v % num_prefix;
    //   int bytes_to_fill = std::min(prefix_size_, 8);
    //   // TODO
    //   // if (port::kLittleEndian) {
    //   //   for (int i = 0; i < bytes_to_fill; ++i) {
    //   //     pos[i] = (prefix >> ((bytes_to_fill - i - 1) << 3)) & 0xFF;
    //   //   }
    //   // } else {
    //   //   memcpy(pos, static_cast<void*>(&prefix), bytes_to_fill);
    //   // }

    //   for (int i = 0; i < bytes_to_fill; ++i) {
    //     pos[i] = (prefix >> ((bytes_to_fill - i - 1) << 3)) & 0xFF;
    //   }

    //   if (prefix_size_ > 8) {
    //     // fill the rest with 0s
    //     memset(pos + 8, '0', prefix_size_ - 8);
    //   }
    //   pos += prefix_size_;
    // }

    int bytes_to_fill = std::min(key_size_ - static_cast<int>(pos - start), 8);
    // TODO:
    // if (port::kLittleEndian) {
    //   for (int i = 0; i < bytes_to_fill; ++i) {
    //     pos[i] = (v >> ((bytes_to_fill - i - 1) << 3)) & 0xFF;
    //   }
    // } else {
    //   memcpy(pos, static_cast<void*>(&v), bytes_to_fill);
    // }
    for (int i = 0; i < bytes_to_fill; ++i) {
      pos[i] = (v >> ((bytes_to_fill - i - 1) << 3)) & 0xFF;
    }
    pos += bytes_to_fill;
    if (key_size_ > pos - start) {
      memset(pos, '0', key_size_ - (pos - start));
    }
  }

  void ErrorExit() {
    // TODO:DeleteDBs();
    exit(1);
  }

  void Run() {
    if (!SanityCheck()) {
      ErrorExit();
    }
    // Open(&open_options_);
    PrintHeader();
    std::stringstream benchmark_stream(FLAGS_benchmarks);
    std::string name;
    // std::unique_ptr<ExpiredTimeFilter> filter;
    while (std::getline(benchmark_stream, name, ',')) {
      // Sanitize parameters
      num_ = FLAGS_num;
      reads_ = (FLAGS_reads < 0 ? FLAGS_num : FLAGS_reads);
      writes_ = (FLAGS_writes < 0 ? FLAGS_num : FLAGS_writes);
      deletes_ = (FLAGS_deletes < 0 ? FLAGS_num : FLAGS_deletes);
      value_size = FLAGS_value_size;
      key_size_ = FLAGS_key_size;
      // entries_per_batch_ = FLAGS_batch_size;
      // writes_before_delete_range_ = FLAGS_writes_before_delete_range;
      // writes_per_range_tombstone_ = FLAGS_writes_per_range_tombstone;
      // range_tombstone_width_ = FLAGS_range_tombstone_width;
      // max_num_range_tombstones_ = FLAGS_max_num_range_tombstones;
      // write_options_ = WriteOptions();
      read_random_exp_range_ = FLAGS_read_random_exp_range;
      // if (FLAGS_sync) {
      //   write_options_.sync = true;
      // }
      // write_options_.disableWAL = FLAGS_disable_wal;
      // write_options_.rate_limiter_priority =
      //     FLAGS_rate_limit_auto_wal_flush ? Env::IO_USER : Env::IO_TOTAL;
      // read_options_ = ReadOptions(FLAGS_verify_checksum, true);
      // read_options_.total_order_seek = FLAGS_total_order_seek;
      // read_options_.prefix_same_as_start = FLAGS_prefix_same_as_start;
      // read_options_.rate_limiter_priority =
      //     FLAGS_rate_limit_user_ops ? Env::IO_USER : Env::IO_TOTAL;
      // read_options_.tailing = FLAGS_use_tailing_iterator;
      // read_options_.readahead_size = FLAGS_readahead_size;
      // read_options_.adaptive_readahead = FLAGS_adaptive_readahead;
      // read_options_.async_io = FLAGS_async_io;
      // read_options_.optimize_multiget_for_io =
      // FLAGS_optimize_multiget_for_io;

      void (Benchmark::*method)(ThreadState*) = nullptr;
      // void (Benchmark::*post_process_method)() = nullptr;

      bool fresh_db = false;
      int num_threads = FLAGS_threads;

      int num_repeat = 1;
      int num_warmup = 0;
      if (!name.empty() && *name.rbegin() == ']') {
        auto it = name.find('[');
        if (it == std::string::npos) {
          fprintf(stderr, "unknown benchmark arguments '%s'\n", name.c_str());
          ErrorExit();
        }
        std::string args = name.substr(it + 1);
        args.resize(args.size() - 1);
        name.resize(it);

        std::string bench_arg;
        std::stringstream args_stream(args);
        while (std::getline(args_stream, bench_arg, '-')) {
          if (bench_arg.empty()) {
            continue;
          }
          if (bench_arg[0] == 'X') {
            // Repeat the benchmark n times
            std::string num_str = bench_arg.substr(1);
            num_repeat = std::stoi(num_str);
          } else if (bench_arg[0] == 'W') {
            // Warm up the benchmark for n times
            std::string num_str = bench_arg.substr(1);
            num_warmup = std::stoi(num_str);
          }
        }
      }
      if (name == "fillseq") {
        fresh_db = true;
        method = &Benchmark::WriteSeq;
      } else if (name == "fillrandom") {
        fresh_db = true;
        method = &Benchmark::WriteRandom;
        // } else if (name == "filluniquerandom" ||
        //            name == "fillanddeleteuniquerandom") {
        //   fresh_db = true;
        //   if (num_threads > 1) {
        //     fprintf(stderr,
        //             "filluniquerandom and fillanddeleteuniquerandom "
        //             "multithreaded not supported, use 1 thread");
        //     num_threads = 1;
        //   }
        //   method = &Benchmark::WriteUniqueRandom;
      } else if (name == "overwrite") {
        method = &Benchmark::WriteRandom;
        // } else if (name == "fillsync") {
        //   fresh_db = true;
        //   num_ /= 1000;
        //   write_options_.sync = true;
        //   method = &Benchmark::WriteRandom;
        // } else if (name == "fill100K") {
        //   fresh_db = true;
        //   num_ /= 1000;
        //   value_size = 100 * 1000;
        //   method = &Benchmark::WriteRandom;
      } else if (name == "readseq") {
        // TODO:
        // method = &Benchmark::ReadSequential;
        // } else if (name == "readtorowcache") {
        //   if (!FLAGS_use_existing_keys || !FLAGS_row_cache_size) {
        //     fprintf(stderr,
        //             "Please set use_existing_keys to true and specify a "
        //             "row cache size in readtorowcache benchmark\n");
        //     ErrorExit();
        //   }
        //   method = &Benchmark::ReadToRowCache;
        // } else if (name == "readtocache") {
        //   method = &Benchmark::ReadSequential;
        //   num_threads = 1;
        //   reads_ = num_;
        // } else if (name == "readreverse") {
        //   method = &Benchmark::ReadReverse;
      } else if (name == "readrandom") {
        // if (FLAGS_multiread_stride) {
        //   fprintf(stderr, "entries_per_batch = %" PRIi64 "\n",
        //           entries_per_batch_);
        // }
        // TODO:
        method = &Benchmark::ReadRandom;
      }
      // } else if (name == "readrandomfast") {
      //   method = &Benchmark::ReadRandomFast;
      // } else if (name == "multireadrandom") {
      //   fprintf(stderr, "entries_per_batch = %" PRIi64 "\n",
      //           entries_per_batch_);
      //   method = &Benchmark::MultiReadRandom;
      // } else if (name == "multireadwhilewriting") {
      //   fprintf(stderr, "entries_per_batch = %" PRIi64 "\n",
      //           entries_per_batch_);
      //   num_threads++;
      //   method = &Benchmark::MultiReadWhileWriting;
      // } else if (name == "approximatesizerandom") {
      //   fprintf(stderr, "entries_per_batch = %" PRIi64 "\n",
      //           entries_per_batch_);
      //   method = &Benchmark::ApproximateSizeRandom;
      // } else if (name == "mixgraph") {
      //   method = &Benchmark::MixGraph;
      // } else if (name == "readmissing") {
      //   ++key_size_;
      //   method = &Benchmark::ReadRandom;
      // } else if (name == "newiterator") {
      //   method = &Benchmark::IteratorCreation;
      // } else if (name == "newiteratorwhilewriting") {
      //   num_threads++;  // Add extra thread for writing
      //   method = &Benchmark::IteratorCreationWhileWriting;
      // } else if (name == "seekrandom") {
      //   method = &Benchmark::SeekRandom;
      // } else if (name == "seekrandomwhilewriting") {
      //   num_threads++;  // Add extra thread for writing
      //   method = &Benchmark::SeekRandomWhileWriting;
      // } else if (name == "seekrandomwhilemerging") {
      //   num_threads++;  // Add extra thread for merging
      //   method = &Benchmark::SeekRandomWhileMerging;
      // } else if (name == "readrandomsmall") {
      //   reads_ /= 1000;
      //   method = &Benchmark::ReadRandom;
      // } else if (name == "deleteseq") {
      //   method = &Benchmark::DeleteSeq;
      // } else if (name == "deleterandom") {
      //   method = &Benchmark::DeleteRandom;
      // } else if (name == "readwhilewriting") {
      //   num_threads++;  // Add extra thread for writing
      //   method = &Benchmark::ReadWhileWriting;
      // } else if (name == "readwhilemerging") {
      //   num_threads++;  // Add extra thread for writing
      //   method = &Benchmark::ReadWhileMerging;
      // } else if (name == "readwhilescanning") {
      //   num_threads++;  // Add extra thread for scaning
      //   method = &Benchmark::ReadWhileScanning;
      // } else if (name == "readrandomwriterandom") {
      //   method = &Benchmark::ReadRandomWriteRandom;
      // } else if (name == "readrandommergerandom") {
      //   if (FLAGS_merge_operator.empty()) {
      //     fprintf(stdout, "%-12s : skipped (--merge_operator is
      //     unknown)\n",
      //             name.c_str());
      //     ErrorExit();
      //   }
      //   method = &Benchmark::ReadRandomMergeRandom;
      // } else if (name == "updaterandom") {
      //   method = &Benchmark::UpdateRandom;
      // } else if (name == "xorupdaterandom") {
      //   method = &Benchmark::XORUpdateRandom;
      // } else if (name == "appendrandom") {
      //   method = &Benchmark::AppendRandom;
      // } else if (name == "mergerandom") {
      //   if (FLAGS_merge_operator.empty()) {
      //     fprintf(stdout, "%-12s : skipped (--merge_operator is
      //     unknown)\n",
      //             name.c_str());
      //     exit(1);
      //   }
      //   method = &Benchmark::MergeRandom;
      // } else if (name == "randomwithverify") {
      //   method = &Benchmark::RandomWithVerify;
      // } else if (name == "fillseekseq") {
      //   method = &Benchmark::WriteSeqSeekSeq;
      // } else if (name == "compact") {
      //   method = &Benchmark::Compact;
      // } else if (name == "compactall") {
      //   CompactAll();
      // } else if (name == "compact0") {
      //   CompactLevel(0);
      // } else if (name == "compact1") {
      //   CompactLevel(1);
      // } else if (name == "waitforcompaction") {
      //   WaitForCompaction();
      // } else if (name == "flush") {
      //   Flush();
      // } else if (name == "crc32c") {
      //   method = &Benchmark::Crc32c;
      // } else if (name == "xxhash") {
      //   method = &Benchmark::xxHash;
      // } else if (name == "xxhash64") {
      //   method = &Benchmark::xxHash64;
      // } else if (name == "xxh3") {
      //   method = &Benchmark::xxh3;
      // } else if (name == "acquireload") {
      //   method = &Benchmark::AcquireLoad;
      // } else if (name == "compress") {
      //   method = &Benchmark::Compress;
      // } else if (name == "uncompress") {
      //   method = &Benchmark::Uncompress;
      // } else if (name == "randomtransaction") {
      //   method = &Benchmark::RandomTransaction;
      //   post_process_method = &Benchmark::RandomTransactionVerify;
      // } else if (name == "randomreplacekeys") {
      //   fresh_db = true;
      //   method = &Benchmark::RandomReplaceKeys;
      // } else if (name == "timeseries") {
      //   timestamp_emulator_.reset(new TimestampEmulator());
      //   if (FLAGS_expire_style == "compaction_filter") {
      //     filter.reset(new ExpiredTimeFilter(timestamp_emulator_));
      //     fprintf(stdout, "Compaction filter is used to remove expired
      //     data"); open_options_.compaction_filter = filter.get();
      //   }
      //   fresh_db = true;
      //   method = &Benchmark::TimeSeries;
      // } else if (name == "block_cache_entry_stats") {
      //   // DB::Properties::kBlockCacheEntryStats
      //   PrintStats("rocksdb.block-cache-entry-stats");
      // } else if (name == "stats") {
      //   PrintStats("rocksdb.stats");
      // } else if (name == "resetstats") {
      //   ResetStats();
      // } else if (name == "verify") {
      //   VerifyDBFromDB(FLAGS_truth_db);
      // } else if (name == "levelstats") {
      //   PrintStats("rocksdb.levelstats");
      // } else if (name == "memstats") {
      //   std::vector<std::string> keys{"rocksdb.num-immutable-mem-table",
      //                                 "rocksdb.cur-size-active-mem-table",
      //                                 "rocksdb.cur-size-all-mem-tables",
      //                                 "rocksdb.size-all-mem-tables",
      //                                 "rocksdb.num-entries-active-mem-table",
      //                                 "rocksdb.num-entries-imm-mem-tables"};
      //   PrintStats(keys);
      // } else if (name == "sstables") {
      //   PrintStats("rocksdb.sstables");
      // } else if (name == "stats_history") {
      //   PrintStatsHistory();
      // } else if (name == "replay") {
      //   if (num_threads > 1) {
      //     fprintf(stderr, "Multi-threaded replay is not yet supported\n");
      //     ErrorExit();
      //   }
      //   if (FLAGS_trace_file == "") {
      //     fprintf(stderr, "Please set --trace_file to be replayed from\n");
      //     ErrorExit();
      //   }
      //   method = &Benchmark::Replay;
      // } else if (name == "getmergeoperands") {
      //   method = &Benchmark::GetMergeOperands;
      // } else if (name == "verifychecksum") {
      //   method = &Benchmark::VerifyChecksum;
      // } else if (name == "verifyfilechecksums") {
      //   method = &Benchmark::VerifyFileChecksums;
      // } else if (name == "readrandomoperands") {
      //   read_operands_ = true;
      //   method = &Benchmark::ReadRandom;
      // } else if (name == "backup") {
      //   method = &Benchmark::Backup;
      // } else if (name == "restore") {
      //   method = &Benchmark::Restore;
      // } else if (!name.empty()) {  // No error message for empty name
      //   fprintf(stderr, "unknown benchmark '%s'\n", name.c_str());
      //   ErrorExit();
      // }

      if (fresh_db) {
        if (FLAGS_use_existing_db) {
          fprintf(stdout, "%-12s : skipped (--use_existing_db is true)\n",
                  name.c_str());
          method = nullptr;
        } else {
          ConfigManager::getInstance().setConfigPath("config.ini");
          // data disks
          DiskInfo disk1(0, FLAGS_db.c_str(), 1024 * 1024 * 1024);
          std::vector<DiskInfo> disks;
          disks.push_back(disk1);
          diskManager_.reset(new DeviceManager(disks));
          kvserver_.reset(new KvServer(diskManager_.get()));
        }
      }

      if (method != nullptr) {
        fprintf(stdout, "DB path: [%s]\n", FLAGS_db.c_str());

        if (num_warmup > 0) {
          printf("Warming up benchmark by running %d times\n", num_warmup);
        }

        for (int i = 0; i < num_warmup; i++) {
          printf("run bench mark\n");
          RunBenchmark(num_threads, name, method);
        }

        if (num_repeat > 1) {
          printf("Running benchmark for %d times\n", num_repeat);
        }

        CombinedStats combined_stats;
        for (int i = 0; i < num_repeat; i++) {
          Stats stats = RunBenchmark(num_threads, name, method);
          combined_stats.AddStats(stats);
          if (FLAGS_confidence_interval_only) {
            combined_stats.ReportWithConfidenceIntervals(name);
          } else {
            combined_stats.Report(name);
          }
        }
        if (num_repeat > 1) {
          combined_stats.ReportFinal(name);
        }
      }

      // if (secondary_update_thread_) {
      //   secondary_update_stopped_.store(1, std::memory_order_relaxed);
      //   secondary_update_thread_->join();
      //   secondary_update_thread_.reset();
      // }

      // if (name != "replay" && FLAGS_trace_file != "") {
      //   Status s = db_.db->EndTrace();
      //   if (!s.ok()) {
      //     fprintf(stderr, "Encountered an error ending the trace, %s\n",
      //             s.ToString().c_str());
      //   }
      // }
      // if (!FLAGS_block_cache_trace_file.empty()) {
      //   Status s = db_.db->EndBlockCacheTrace();
      //   if (!s.ok()) {
      //     fprintf(stderr,
      //             "Encountered an error ending the block cache tracing,
      //             %s\n", s.ToString().c_str());
      //   }
      // }

      // if (FLAGS_statistics) {
      //   fprintf(stdout, "STATISTICS:\n%s\n", dbstats->ToString().c_str());
      // }
      // if (FLAGS_simcache_size >= 0) {
      //   fprintf(
      //       stdout, "SIMULATOR CACHE STATISTICS:\n%s\n",
      //       static_cast_with_check<SimCache>(cache_.get())->ToString().c_str());
      // }

      // if (FLAGS_use_secondary_db) {
      //   fprintf(stdout, "Secondary instance updated  %" PRIu64 " times.\n",
      //           secondary_db_updates_);
      // }
    }
  }
};
int db_bench_tool(int argc, char** argv) {
  // ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  // ConfigOptions config_options;
  static bool initialized = false;
  if (!initialized) {
    SetUsageMessage(std::string("\nUSAGE:\n") + std::string(argv[0]) +
                    " [OPTIONS]...");
    initialized = true;
  }
  ParseCommandLineFlags(&argc, &argv, true);

  if (!FLAGS_seed) {
    uint64_t now = NowMicros();
    seed_base = static_cast<int64_t>(now);
    fprintf(stdout, "Set seed to %" PRIu64 " because --seed was 0\n",
            *seed_base);
  } else {
    seed_base = FLAGS_seed;
  }

  if (FLAGS_use_existing_keys && !FLAGS_use_existing_db) {
    fprintf(stderr,
            "`-use_existing_db` must be true for `-use_existing_keys` to be "
            "settable\n");
    exit(1);
  }

  FLAGS_value_size_distribution_type_e =
      StringToDistributionType(FLAGS_value_size_distribution_type.c_str());

  // Note options sanitization may increase thread pool sizes according to
  // max_background_flushes/max_background_compactions/max_background_jobs
  // FLAGS_env->SetBackgroundThreads(FLAGS_num_high_pri_threads,
  //                                 ROCKSDB_NAMESPACE::Env::Priority::HIGH);
  // FLAGS_env->SetBackgroundThreads(FLAGS_num_bottom_pri_threads,
  //                                 ROCKSDB_NAMESPACE::Env::Priority::BOTTOM);
  // FLAGS_env->SetBackgroundThreads(FLAGS_num_low_pri_threads,
  //                                 ROCKSDB_NAMESPACE::Env::Priority::LOW);

  // Choose a location for the test database if none given with --db=<path>
  // if (FLAGS_db.empty()) {
  //   std::string default_db_path;
  //   FLAGS_env->GetTestDirectory(&default_db_path);
  //   default_db_path += "/dbbench";
  //   FLAGS_db = default_db_path;
  // }

  // if (FLAGS_backup_dir.empty()) {
  //   FLAGS_backup_dir = FLAGS_db + "/backup";
  // }

  // if (FLAGS_restore_dir.empty()) {
  //   FLAGS_restore_dir = FLAGS_db + "/restore";
  // }

  if (FLAGS_stats_interval_seconds > 0) {
    // When both are set then FLAGS_stats_interval determines the frequency
    // at which the timer is checked for FLAGS_stats_interval_seconds
    FLAGS_stats_interval = 1000;
  }

  // if (FLAGS_seek_missing_prefix && FLAGS_prefix_size <= 8) {
  //   fprintf(stderr, "prefix_size > 8 required by --seek_missing_prefix\n");
  //   exit(1);
  // }

  Benchmark benchmark;
  benchmark.Run();

  return 0;
}
}  // namespace HASHKV_NAMESPACE
