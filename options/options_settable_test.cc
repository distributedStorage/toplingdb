//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <cstring>

#include "options/cf_options.h"
#include "options/db_options.h"
#include "options/options_helper.h"
#include "rocksdb/convenience.h"
#include "test_util/testharness.h"

#ifndef GFLAGS
bool FLAGS_enable_print = false;
#else
#include "util/gflags_compat.h"
using GFLAGS_NAMESPACE::ParseCommandLineFlags;
DEFINE_bool(enable_print, false, "Print options generated to console.");
#endif  // GFLAGS

namespace ROCKSDB_NAMESPACE {

// Verify options are settable from options strings.
// We take the approach that depends on compiler behavior that copy constructor
// won't touch implicit padding bytes, so that the test is fragile.
// As a result, we only run the tests to verify new fields in options are
// settable through string on limited platforms as it depends on behavior of
// compilers.
#ifndef ROCKSDB_LITE
#if defined OS_LINUX || defined OS_WIN
#ifndef __clang__
#ifndef ROCKSDB_UBSAN_RUN

class OptionsSettableTest : public testing::Test {
 public:
  OptionsSettableTest() {}
};

const char kSpecialChar = 'z';
using OffsetGap = std::vector<std::pair<size_t, size_t>>;

void FillWithSpecialChar(char* start_ptr, size_t total_size,
                         const OffsetGap& excluded,
                         char special_char = kSpecialChar) {
  size_t offset = 0;
  // The excluded vector contains pairs of bytes, (first, second).
  // The first bytes are all set to the special char (represented as 'c' below).
  // The second bytes are simply skipped (padding bytes).
  // ccccc[skipped]cccccccc[skiped]cccccccc[skipped]
  for (auto& pair : excluded) {
    std::memset(start_ptr + offset, special_char, pair.first - offset);
    offset = pair.first + pair.second;
  }
  // The rest of the structure is filled with the special characters.
  // ccccc[skipped]cccccccc[skiped]cccccccc[skipped]cccccccccccccccc
  std::memset(start_ptr + offset, special_char, total_size - offset);
}

int NumUnsetBytes(char* start_ptr, size_t total_size,
                  const OffsetGap& excluded) {
  int total_unset_bytes_base = 0;
  size_t offset = 0;
  for (auto& pair : excluded) {
    // The first part of the structure contains memory spaces that can be
    // set (pair.first), and memory spaces that cannot be set (pair.second).
    // Therefore total_unset_bytes_base only agregates bytes set to kSpecialChar
    // in the pair.first bytes, but skips the pair.second bytes (padding bytes).
    for (char* ptr = start_ptr + offset; ptr < start_ptr + pair.first; ptr++) {
      if (*ptr == kSpecialChar) {
        total_unset_bytes_base++;
      }
    }
    offset = pair.first + pair.second;
  }
  // Then total_unset_bytes_base aggregates the bytes
  // set to kSpecialChar in the rest of the structure
  for (char* ptr = start_ptr + offset; ptr < start_ptr + total_size; ptr++) {
    if (*ptr == kSpecialChar) {
      total_unset_bytes_base++;
    }
  }
  return total_unset_bytes_base;
}

// Return true iff two structs are the same except excluded fields.
bool CompareBytes(char* start_ptr1, char* start_ptr2, size_t total_size,
                  const OffsetGap& excluded) {
  size_t offset = 0;
  for (auto& pair : excluded) {
    for (; offset < pair.first; offset++) {
      if (*(start_ptr1 + offset) != *(start_ptr2 + offset)) {
        return false;
      }
    }
    offset = pair.first + pair.second;
  }
  for (; offset < total_size; offset++) {
    if (*(start_ptr1 + offset) != *(start_ptr2 + offset)) {
      return false;
    }
  }
  return true;
}

// If the test fails, likely a new option is added to BlockBasedTableOptions
// but it cannot be set through GetBlockBasedTableOptionsFromString(), or the
// test is not updated accordingly.
// After adding an option, we need to make sure it is settable by
// GetBlockBasedTableOptionsFromString() and add the option to the input string
// passed to the GetBlockBasedTableOptionsFromString() in this test.
// If it is a complicated type, you also need to add the field to
// kBbtoExcluded, and maybe add customized verification for it.
TEST_F(OptionsSettableTest, BlockBasedTableOptionsAllFieldsSettable) {
  // Items in the form of <offset, size>. Need to be in ascending order
  // and not overlapping. Need to update if new option to be excluded is added
  // (e.g, pointer-type)
  const OffsetGap kBbtoExcluded = {
      {offsetof(struct BlockBasedTableOptions, flush_block_policy_factory),
       sizeof(std::shared_ptr<FlushBlockPolicyFactory>)},
      {offsetof(struct BlockBasedTableOptions, block_cache),
       sizeof(std::shared_ptr<Cache>)},
      {offsetof(struct BlockBasedTableOptions, persistent_cache),
       sizeof(std::shared_ptr<PersistentCache>)},
      {offsetof(struct BlockBasedTableOptions, block_cache_compressed),
       sizeof(std::shared_ptr<Cache>)},
      {offsetof(struct BlockBasedTableOptions, cache_usage_options),
       sizeof(CacheUsageOptions)},
      {offsetof(struct BlockBasedTableOptions, filter_policy),
       sizeof(std::shared_ptr<const FilterPolicy>)},
  };

  // In this test, we catch a new option of BlockBasedTableOptions that is not
  // settable through GetBlockBasedTableOptionsFromString().
  // We count padding bytes of the option struct, and assert it to be the same
  // as unset bytes of an option struct initialized by
  // GetBlockBasedTableOptionsFromString().

  char* bbto_ptr = new char[sizeof(BlockBasedTableOptions)];

  // Count padding bytes by setting all bytes in the memory to a special char,
  // copy a well constructed struct to this memory and see how many special
  // bytes left.
  BlockBasedTableOptions* bbto = new (bbto_ptr) BlockBasedTableOptions();
  FillWithSpecialChar(bbto_ptr, sizeof(BlockBasedTableOptions), kBbtoExcluded);
  // It based on the behavior of compiler that padding bytes are not changed
  // when copying the struct. It's prone to failure when compiler behavior
  // changes. We verify there is unset bytes to detect the case.
  *bbto = BlockBasedTableOptions();
  int unset_bytes_base =
      NumUnsetBytes(bbto_ptr, sizeof(BlockBasedTableOptions), kBbtoExcluded);
  ASSERT_GT(unset_bytes_base, 0);
  bbto->~BlockBasedTableOptions();

  // Construct the base option passed into
  // GetBlockBasedTableOptionsFromString().
  bbto = new (bbto_ptr) BlockBasedTableOptions();
  FillWithSpecialChar(bbto_ptr, sizeof(BlockBasedTableOptions), kBbtoExcluded);
  // This option is not setable:
  bbto->use_delta_encoding = true;
  bbto->use_raw_size_as_estimated_file_size = true; // ToplingDB specific
  bbto->enable_get_random_keys = true; // ToplingDB specific

  char* new_bbto_ptr = new char[sizeof(BlockBasedTableOptions)];
  BlockBasedTableOptions* new_bbto =
      new (new_bbto_ptr) BlockBasedTableOptions();
  FillWithSpecialChar(new_bbto_ptr, sizeof(BlockBasedTableOptions),
                      kBbtoExcluded);

  // Need to update the option string if a new option is added.
  ASSERT_OK(GetBlockBasedTableOptionsFromString(
      *bbto,
      "cache_index_and_filter_blocks=1;"
      "cache_index_and_filter_blocks_with_high_priority=true;"
      "metadata_cache_options={top_level_index_pinning=kFallback;"
      "partition_pinning=kAll;"
      "unpartitioned_pinning=kFlushedAndSimilar;};"
      "pin_l0_filter_and_index_blocks_in_cache=1;"
      "pin_top_level_index_and_filter=1;"
      "index_type=kHashSearch;"
      "data_block_index_type=kDataBlockBinaryAndHash;"
      "index_shortening=kNoShortening;"
      "data_block_hash_table_util_ratio=0.75;"
      "checksum=kxxHash;no_block_cache=1;"
      "block_cache=1M;block_cache_compressed=1k;block_size=1024;"
      "block_size_deviation=8;block_restart_interval=4; "
      "metadata_block_size=1024;"
      "partition_filters=false;"
      "optimize_filters_for_memory=true;"
      "index_block_restart_interval=4;"
      "filter_policy=bloomfilter:4:true;whole_key_filtering=1;detect_filter_"
      "construct_corruption=false;"
      "format_version=1;"
      "verify_compression=true;read_amp_bytes_per_bit=0;"
      "enable_index_compression=false;"
      "block_align=true;"
      "max_auto_readahead_size=0;"
      "prepopulate_block_cache=kDisable;"
      "initial_auto_readahead_size=0;"
      "num_file_reads_for_auto_readahead=0",
      new_bbto));

  ASSERT_EQ(unset_bytes_base,
            NumUnsetBytes(new_bbto_ptr, sizeof(BlockBasedTableOptions),
                          kBbtoExcluded));

  ASSERT_TRUE(new_bbto->block_cache.get() != nullptr);
  ASSERT_TRUE(new_bbto->block_cache_compressed.get() != nullptr);
  ASSERT_TRUE(new_bbto->filter_policy.get() != nullptr);

  bbto->~BlockBasedTableOptions();
  new_bbto->~BlockBasedTableOptions();

  delete[] bbto_ptr;
  delete[] new_bbto_ptr;
}

// If the test fails, likely a new option is added to DBOptions
// but it cannot be set through GetDBOptionsFromString(), or the test is not
// updated accordingly.
// After adding an option, we need to make sure it is settable by
// GetDBOptionsFromString() and add the option to the input string passed to
// DBOptionsFromString()in this test.
// If it is a complicated type, you also need to add the field to
// kDBOptionsExcluded, and maybe add customized verification for it.
TEST_F(OptionsSettableTest, DBOptionsAllFieldsSettable) {
  const OffsetGap kDBOptionsExcluded = {
      {offsetof(struct DBOptions, env), sizeof(Env*)},
      {offsetof(struct DBOptions, rate_limiter),
       sizeof(std::shared_ptr<RateLimiter>)},
      {offsetof(struct DBOptions, sst_file_manager),
       sizeof(std::shared_ptr<SstFileManager>)},
      {offsetof(struct DBOptions, info_log), sizeof(std::shared_ptr<Logger>)},
      {offsetof(struct DBOptions, statistics),
       sizeof(std::shared_ptr<Statistics>)},
      {offsetof(struct DBOptions, db_paths), sizeof(std::vector<DbPath>)},
      {offsetof(struct DBOptions, db_log_dir), sizeof(std::string)},
      {offsetof(struct DBOptions, wal_dir), sizeof(std::string)},
      {offsetof(struct DBOptions, write_buffer_manager),
       sizeof(std::shared_ptr<WriteBufferManager>)},
      {offsetof(struct DBOptions, listeners),
       sizeof(std::vector<std::shared_ptr<EventListener>>)},
      {offsetof(struct DBOptions, row_cache), sizeof(std::shared_ptr<Cache>)},
      {offsetof(struct DBOptions, wal_filter), sizeof(const WalFilter*)},
      {offsetof(struct DBOptions, file_checksum_gen_factory),
       sizeof(std::shared_ptr<FileChecksumGenFactory>)},
      {offsetof(struct DBOptions, db_host_id), sizeof(std::string)},
      {offsetof(struct DBOptions, checksum_handoff_file_types),
       sizeof(FileTypeSet)},
      {offsetof(struct DBOptions, compaction_service),
       sizeof(std::shared_ptr<CompactionService>)},
      {offsetof(struct DBOptions, wbwi_factory),
       sizeof(std::shared_ptr<class WriteBatchWithIndexFactory>)},
  };

  char* options_ptr = new char[sizeof(DBOptions)];

  // Count padding bytes by setting all bytes in the memory to a special char,
  // copy a well constructed struct to this memory and see how many special
  // bytes left.
  DBOptions* options = new (options_ptr) DBOptions();
  FillWithSpecialChar(options_ptr, sizeof(DBOptions), kDBOptionsExcluded);
  // It based on the behavior of compiler that padding bytes are not changed
  // when copying the struct. It's prone to failure when compiler behavior
  // changes. We verify there is unset bytes to detect the case.
  *options = DBOptions();
  int unset_bytes_base =
      NumUnsetBytes(options_ptr, sizeof(DBOptions), kDBOptionsExcluded);
  ASSERT_GT(unset_bytes_base, 0);
  options->~DBOptions();

  options = new (options_ptr) DBOptions();
  FillWithSpecialChar(options_ptr, sizeof(DBOptions), kDBOptionsExcluded);
  options->allow_fdatasync = true; // ToplingDB specific

  char* new_options_ptr = new char[sizeof(DBOptions)];
  DBOptions* new_options = new (new_options_ptr) DBOptions();
  FillWithSpecialChar(new_options_ptr, sizeof(DBOptions), kDBOptionsExcluded);

  // Need to update the option string if a new option is added.
  ASSERT_OK(
      GetDBOptionsFromString(*options,
                             "wal_bytes_per_sync=4295048118;"
                             "delete_obsolete_files_period_micros=4294967758;"
                             "WAL_ttl_seconds=4295008036;"
                             "WAL_size_limit_MB=4295036161;"
                             "max_write_batch_group_size_bytes=1048576;"
                             "wal_dir=path/to/wal_dir;"
                             "db_write_buffer_size=2587;"
                             "max_subcompactions=64330;"
                             "max_level1_subcompactions=64330;"
                             "table_cache_numshardbits=28;"
                             "max_open_files=72;"
                             "max_file_opening_threads=35;"
                             "max_background_jobs=8;"
                             "max_background_compactions=33;"
                             "use_fsync=true;"
                             "use_adaptive_mutex=false;"
                             "max_total_wal_size=4295005604;"
                             "compaction_readahead_size=0;"
                             "keep_log_file_num=4890;"
                             "skip_stats_update_on_db_open=false;"
                             "skip_checking_sst_file_sizes_on_db_open=false;"
                             "max_manifest_file_size=4295009941;"
                             "db_log_dir=path/to/db_log_dir;"
                             "writable_file_max_buffer_size=1048576;"
                             "paranoid_checks=true;"
                             "flush_verify_memtable_count=true;"
                             "track_and_verify_wals_in_manifest=true;"
                             "verify_sst_unique_id_in_manifest=true;"
                             "is_fd_close_on_exec=false;"
                             "bytes_per_sync=4295013613;"
                             "strict_bytes_per_sync=true;"
                             "enable_thread_tracking=false;"
                             "recycle_log_file_num=0;"
                             "create_missing_column_families=true;"
                             "log_file_time_to_roll=3097;"
                             "max_background_flushes=35;"
                             "create_if_missing=false;"
                             "error_if_exists=true;"
                             "delayed_write_rate=4294976214;"
                             "manifest_preallocation_size=1222;"
                             "allow_mmap_writes=false;"
                             "stats_dump_period_sec=70127;"
                             "stats_persist_period_sec=54321;"
                             "persist_stats_to_disk=true;"
                             "stats_history_buffer_size=14159;"
                             "allow_fallocate=true;"
                             "allow_mmap_reads=false;"
                             "use_direct_reads=false;"
                             "use_direct_io_for_flush_and_compaction=false;"
                             "max_log_file_size=4607;"
                             "random_access_max_buffer_size=1048576;"
                             "advise_random_on_open=true;"
                             "fail_if_options_file_error=false;"
                             "enable_pipelined_write=false;"
                             "unordered_write=false;"
                             "allow_concurrent_memtable_write=true;"
                             "wal_recovery_mode=kPointInTimeRecovery;"
                             "enable_write_thread_adaptive_yield=true;"
                             "write_thread_slow_yield_usec=5;"
                             "write_thread_max_yield_usec=1000;"
                             "access_hint_on_compaction_start=NONE;"
                             "info_log_level=DEBUG_LEVEL;"
                             "dump_malloc_stats=false;"
                             "allow_2pc=false;"
                             "avoid_flush_during_recovery=false;"
                             "avoid_flush_during_shutdown=false;"
                             "allow_ingest_behind=false;"
                             "concurrent_prepare=false;"
                             "two_write_queues=false;"
                             "manual_wal_flush=false;"
                             "wal_compression=kZSTD;"
                             "seq_per_batch=false;"
                             "atomic_flush=false;"
                             "avoid_unnecessary_blocking_io=false;"
                             "log_readahead_size=0;"
                             "write_dbid_to_manifest=false;"
                             "best_efforts_recovery=false;"
                             "max_bgerror_resume_count=2;"
                             "bgerror_resume_retry_interval=1000000;"
                             "db_host_id=hostname;"
                             "lowest_used_cache_tier=kNonVolatileBlockTier;"
                             "allow_data_in_errors=false;"
                             "enforce_single_del_contracts=false;",
                             new_options));

  ASSERT_EQ(unset_bytes_base, NumUnsetBytes(new_options_ptr, sizeof(DBOptions),
                                            kDBOptionsExcluded));

  options->~DBOptions();
  new_options->~DBOptions();

  delete[] options_ptr;
  delete[] new_options_ptr;
}

// If the test fails, likely a new option is added to ColumnFamilyOptions
// but it cannot be set through GetColumnFamilyOptionsFromString(), or the
// test is not updated accordingly.
// After adding an option, we need to make sure it is settable by
// GetColumnFamilyOptionsFromString() and add the option to the input
// string passed to GetColumnFamilyOptionsFromString() in this test.
// If it is a complicated type, you also need to add the field to
// kColumnFamilyOptionsExcluded, and maybe add customized verification
// for it.
TEST_F(OptionsSettableTest, ColumnFamilyOptionsAllFieldsSettable) {
  // options in the excluded set need to appear in the same order as in
  // ColumnFamilyOptions.
  const OffsetGap kColumnFamilyOptionsExcluded = {
      {offsetof(struct ColumnFamilyOptions, inplace_callback),
       sizeof(UpdateStatus(*)(char*, uint32_t*, Slice, std::string*))},
      {offsetof(struct ColumnFamilyOptions,
                memtable_insert_with_hint_prefix_extractor),
       sizeof(std::shared_ptr<const SliceTransform>)},
      {offsetof(struct ColumnFamilyOptions, compression_per_level),
       sizeof(std::vector<CompressionType>)},
      {offsetof(struct ColumnFamilyOptions,
                max_bytes_for_level_multiplier_additional),
       sizeof(std::vector<int>)},
      {offsetof(struct ColumnFamilyOptions, memtable_factory),
       sizeof(std::shared_ptr<MemTableRepFactory>)},
      {offsetof(struct ColumnFamilyOptions,
                table_properties_collector_factories),
       sizeof(ColumnFamilyOptions::TablePropertiesCollectorFactories)},
      {offsetof(struct ColumnFamilyOptions, preclude_last_level_data_seconds),
       sizeof(uint64_t)},
      {offsetof(struct ColumnFamilyOptions, preserve_internal_time_seconds),
       sizeof(uint64_t)},
      {offsetof(struct ColumnFamilyOptions, blob_cache),
       sizeof(std::shared_ptr<Cache>)},
      {offsetof(struct ColumnFamilyOptions, comparator), sizeof(Comparator*)},
      {offsetof(struct ColumnFamilyOptions, merge_operator),
       sizeof(std::shared_ptr<MergeOperator>)},
      {offsetof(struct ColumnFamilyOptions, compaction_filter),
       sizeof(const CompactionFilter*)},
      {offsetof(struct ColumnFamilyOptions, compaction_filter_factory),
       sizeof(std::shared_ptr<CompactionFilterFactory>)},
      {offsetof(struct ColumnFamilyOptions, prefix_extractor),
       sizeof(std::shared_ptr<const SliceTransform>)},
      {offsetof(struct ColumnFamilyOptions, snap_refresh_nanos),
       sizeof(uint64_t)},
      {offsetof(struct ColumnFamilyOptions, table_factory),
       sizeof(std::shared_ptr<TableFactory>)},
      {offsetof(struct ColumnFamilyOptions, cf_paths),
       sizeof(std::vector<DbPath>)},
      {offsetof(struct ColumnFamilyOptions, compaction_thread_limiter),
       sizeof(std::shared_ptr<ConcurrentTaskLimiter>)},
      {offsetof(struct ColumnFamilyOptions, sst_partitioner_factory),
       sizeof(std::shared_ptr<SstPartitionerFactory>)},
      {offsetof(struct ColumnFamilyOptions, compaction_executor_factory),
       sizeof(std::shared_ptr<class CompactionExecutorFactory>)},
      {offsetof(struct ColumnFamilyOptions, html_user_key_coder),
       sizeof(std::shared_ptr<class AnyPlugin>)},
  };

  char* options_ptr = new char[sizeof(ColumnFamilyOptions)];

  // Count padding bytes by setting all bytes in the memory to a special char,
  // copy a well constructed struct to this memory and see how many special
  // bytes left.
  FillWithSpecialChar(options_ptr, sizeof(ColumnFamilyOptions),
                      kColumnFamilyOptionsExcluded);

  // Invoke a user-defined constructor in the hope that it does not overwrite
  // padding bytes. Note that previously we relied on the implicitly-defined
  // copy-assignment operator (i.e., `*options = ColumnFamilyOptions();`) here,
  // which did in fact modify padding bytes.
  ColumnFamilyOptions* options = new (options_ptr) ColumnFamilyOptions();

  int unset_bytes_base = NumUnsetBytes(options_ptr, sizeof(ColumnFamilyOptions),
                                       kColumnFamilyOptionsExcluded);
  ASSERT_GT(unset_bytes_base, 0);
  options->~ColumnFamilyOptions();

  options = new (options_ptr) ColumnFamilyOptions();
  FillWithSpecialChar(options_ptr, sizeof(ColumnFamilyOptions),
                      kColumnFamilyOptionsExcluded);

  // Following options are not settable through
  // GetColumnFamilyOptionsFromString():
  options->compaction_options_universal = CompactionOptionsUniversal();
  options->num_levels = 42;  // Initialize options for MutableCF
  options->compaction_filter = nullptr;
  options->sst_partitioner_factory = nullptr;
  options->compaction_executor_factory = nullptr; // ToplingDB specific
  options->html_user_key_coder = nullptr; // ToplingDB specific

  char* new_options_ptr = new char[sizeof(ColumnFamilyOptions)];
  ColumnFamilyOptions* new_options =
      new (new_options_ptr) ColumnFamilyOptions();
  FillWithSpecialChar(new_options_ptr, sizeof(ColumnFamilyOptions),
                      kColumnFamilyOptionsExcluded);

  // Need to update the option string if a new option is added.
  ASSERT_OK(GetColumnFamilyOptionsFromString(
      *options,
      "compaction_filter_factory=mpudlojcujCompactionFilterFactory;"
      "table_factory=PlainTable;"
      "prefix_extractor=rocksdb.CappedPrefix.13;"
      "comparator=leveldb.BytewiseComparator;"
      "compression_per_level=kBZip2Compression:kBZip2Compression:"
      "kBZip2Compression:kNoCompression:kZlibCompression:kBZip2Compression:"
      "kSnappyCompression;"
      "max_bytes_for_level_base=986;"
      "bloom_locality=8016;"
      "target_file_size_base=4294976376;"
      "memtable_huge_page_size=2557;"
      "max_successive_merges=5497;"
      "max_sequential_skip_in_iterations=4294971408;"
      "arena_block_size=1893;"
      "target_file_size_multiplier=35;"
      "min_write_buffer_number_to_merge=9;"
      "max_write_buffer_number=84;"
      "write_buffer_size=1653;"
      "max_compaction_bytes=64;"
      "ignore_max_compaction_bytes_for_input=true;"
      "max_bytes_for_level_multiplier=60;"
      "memtable_factory=SkipListFactory;"
      "compression=kNoCompression;"
      "compression_opts=5:6:7:8:9:10:true:11:false;"
      "bottommost_compression_opts=4:5:6:7:8:9:true:10:true;"
      "bottommost_compression=kDisableCompressionOption;"
      "level0_stop_writes_trigger=33;"
      "num_levels=99;"
      "level0_slowdown_writes_trigger=22;"
      "level0_file_num_compaction_trigger=14;"
      "compaction_filter=urxcqstuwnCompactionFilter;"
      "soft_pending_compaction_bytes_limit=0;"
      "max_write_buffer_number_to_maintain=84;"
      "max_write_buffer_size_to_maintain=2147483648;"
      "merge_operator=aabcxehazrMergeOperator;"
      "memtable_prefix_bloom_size_ratio=0.4642;"
      "memtable_whole_key_filtering=true;"
      "memtable_insert_with_hint_prefix_extractor=rocksdb.CappedPrefix.13;"
      "check_flush_compaction_key_order=false;"
      "paranoid_file_checks=true;"
      "force_consistency_checks=true;"
      "inplace_update_num_locks=7429;"
      "experimental_mempurge_threshold=0.0001;"
      "optimize_filters_for_hits=false;"
      "level_compaction_dynamic_level_bytes=false;"
      "level_compaction_dynamic_file_size=true;"
      "inplace_update_support=false;"
      "compaction_style=kCompactionStyleFIFO;"
      "compaction_pri=kMinOverlappingRatio;"
      "hard_pending_compaction_bytes_limit=0;"
      "disable_auto_compactions=false;"
      "report_bg_io_stats=true;"
      "ttl=60;"
      "periodic_compaction_seconds=3600;"
      "sample_for_compression=0;"
      "enable_blob_files=true;"
      "min_blob_size=256;"
      "blob_file_size=1000000;"
      "blob_compression_type=kBZip2Compression;"
      "enable_blob_garbage_collection=true;"
      "blob_garbage_collection_age_cutoff=0.5;"
      "blob_garbage_collection_force_threshold=0.75;"
      "blob_compaction_readahead_size=262144;"
      "blob_file_starting_level=1;"
      "prepopulate_blob_cache=kDisable;"
      "bottommost_temperature=kWarm;"
      "last_level_temperature=kWarm;"
      "preclude_last_level_data_seconds=86400;"
      "preserve_internal_time_seconds=86400;"
      "compaction_options_fifo={max_table_files_size=3;allow_"
      "compaction=false;age_for_warm=1;};"
      "blob_cache=1M;"
      "memtable_protection_bytes_per_key=2;",
      new_options));

  ASSERT_NE(new_options->blob_cache.get(), nullptr);

  ASSERT_EQ(unset_bytes_base,
            NumUnsetBytes(new_options_ptr, sizeof(ColumnFamilyOptions),
                          kColumnFamilyOptionsExcluded));

  ColumnFamilyOptions rnd_filled_options = *new_options;

  options->~ColumnFamilyOptions();
  new_options->~ColumnFamilyOptions();

  delete[] options_ptr;
  delete[] new_options_ptr;

  // Test copying to mutabable and immutable options and copy back the mutable
  // part.
  const OffsetGap kMutableCFOptionsExcluded = {
      {offsetof(struct MutableCFOptions, prefix_extractor),
       sizeof(std::shared_ptr<const SliceTransform>)},
      {offsetof(struct MutableCFOptions,
                max_bytes_for_level_multiplier_additional),
       sizeof(std::vector<int>)},
      {offsetof(struct MutableCFOptions, compression_per_level),
       sizeof(std::vector<CompressionType>)},
      {offsetof(struct MutableCFOptions, max_file_size),
       sizeof(std::vector<uint64_t>)},
  };

  // For all memory used for options, pre-fill every char. Otherwise, the
  // padding bytes might be different so that byte-wise comparison doesn't
  // general equal results even if objects are equal.
  const char kMySpecialChar = 'x';
  char* mcfo1_ptr = new char[sizeof(MutableCFOptions)];
  FillWithSpecialChar(mcfo1_ptr, sizeof(MutableCFOptions),
                      kMutableCFOptionsExcluded, kMySpecialChar);
  char* mcfo2_ptr = new char[sizeof(MutableCFOptions)];
  FillWithSpecialChar(mcfo2_ptr, sizeof(MutableCFOptions),
                      kMutableCFOptionsExcluded, kMySpecialChar);

  // A clean column family options is constructed after filling the same special
  // char as the initial one. So that the padding bytes are the same.
  char* cfo_clean_ptr = new char[sizeof(ColumnFamilyOptions)];
  FillWithSpecialChar(cfo_clean_ptr, sizeof(ColumnFamilyOptions),
                      kColumnFamilyOptionsExcluded);
  rnd_filled_options.num_levels = 66;
  ColumnFamilyOptions* cfo_clean = new (cfo_clean_ptr) ColumnFamilyOptions();

  MutableCFOptions* mcfo1 =
      new (mcfo1_ptr) MutableCFOptions(rnd_filled_options);
  ColumnFamilyOptions cfo_back = BuildColumnFamilyOptions(*cfo_clean, *mcfo1);
  MutableCFOptions* mcfo2 = new (mcfo2_ptr) MutableCFOptions(cfo_back);

  ASSERT_TRUE(CompareBytes(mcfo1_ptr, mcfo2_ptr, sizeof(MutableCFOptions),
                           kMutableCFOptionsExcluded));

  cfo_clean->~ColumnFamilyOptions();
  mcfo1->~MutableCFOptions();
  mcfo2->~MutableCFOptions();
  delete[] mcfo1_ptr;
  delete[] mcfo2_ptr;
  delete[] cfo_clean_ptr;
}
#endif  // !ROCKSDB_UBSAN_RUN
#endif  // !__clang__
#endif  // OS_LINUX || OS_WIN
#endif  // !ROCKSDB_LITE

}  // namespace ROCKSDB_NAMESPACE

int main(int argc, char** argv) {
  ROCKSDB_NAMESPACE::port::InstallStackTraceHandler();
  ::testing::InitGoogleTest(&argc, argv);
#ifdef GFLAGS
  ParseCommandLineFlags(&argc, &argv, true);
#endif  // GFLAGS
  return RUN_ALL_TESTS();
}
