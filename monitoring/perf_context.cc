//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//

#include <sstream>

#include "monitoring/perf_context_imp.h"

namespace ROCKSDB_NAMESPACE {

#if defined(NPERF_CONTEXT)
// Should not be used because the counters are not thread-safe.
// Put here just to make get_perf_context() simple without ifdef.
PerfContext perf_context;
#else
  ROCKSDB_STATIC_TLS ROCKSDB_RAW_TLS PerfContext* p_perf_context;
  // not need ROCKSDB_STATIC_TLS
  static thread_local std::unique_ptr<PerfContext> g_del_perf_context;
  PerfContext* init_perf_context() noexcept {
    // tls is always init at first use, this function is a must
    auto ptr = p_perf_context = new PerfContext;
    g_del_perf_context.reset(ptr);
    return ptr;
  }
#endif

PerfContext* get_perf_context() { return &perf_context; }

PerfContext::~PerfContext() {
#if !defined(NPERF_CONTEXT) && !defined(OS_SOLARIS)
  ClearPerLevelPerfContext();
#endif
}

PerfContext::PerfContext() noexcept { Reset(); }

PerfContext::PerfContext(const PerfContext&) = default;

PerfContext::PerfContext(PerfContext&&) noexcept = default;

PerfContext& PerfContext::operator=(const PerfContext&) = default;

void PerfContext::Reset() {
#ifndef NPERF_CONTEXT
  user_key_comparison_count = 0;
  block_cache_hit_count = 0;
  block_read_count = 0;
  block_read_byte = 0;
  block_read_time = 0;
  block_cache_index_hit_count = 0;
  block_cache_standalone_handle_count = 0;
  block_cache_real_handle_count = 0;
  index_block_read_count = 0;
  block_cache_filter_hit_count = 0;
  filter_block_read_count = 0;
  compression_dict_block_read_count = 0;
  secondary_cache_hit_count = 0;
  compressed_sec_cache_insert_real_count = 0;
  compressed_sec_cache_insert_dummy_count = 0;
  compressed_sec_cache_uncompressed_bytes = 0;
  compressed_sec_cache_compressed_bytes = 0;
  block_checksum_time = 0;
  block_decompress_time = 0;
  get_read_bytes = 0;
  multiget_read_bytes = 0;
  iter_read_bytes = 0;

  blob_cache_hit_count = 0;
  blob_read_count = 0;
  blob_read_byte = 0;
  blob_read_time = 0;
  blob_checksum_time = 0;
  blob_decompress_time = 0;

  internal_key_skipped_count = 0;
  internal_delete_skipped_count = 0;
  internal_recent_skipped_count = 0;
  internal_merge_count = 0;
  internal_range_del_reseek_count = 0;
  write_wal_time = 0;

  get_snapshot_time = 0;
  get_from_memtable_time = 0;
  get_from_memtable_count = 0;
  get_post_process_time = 0;
  get_from_output_files_time = 0;
  seek_on_memtable_time = 0;
  seek_on_memtable_count = 0;
  next_on_memtable_count = 0;
  prev_on_memtable_count = 0;
  seek_child_seek_time = 0;
  seek_child_seek_count = 0;
  seek_min_heap_time = 0;
  seek_internal_seek_time = 0;
  find_next_user_entry_time = 0;
  write_pre_and_post_process_time = 0;
  write_memtable_time = 0;
  write_delay_time = 0;
  write_thread_wait_nanos = 0;
  write_scheduling_flushes_compactions_time = 0;
  db_mutex_lock_nanos = 0;
  db_condition_wait_nanos = 0;
  merge_operator_time_nanos = 0;
  read_index_block_nanos = 0;
  read_filter_block_nanos = 0;
  new_table_block_iter_nanos = 0;
  new_table_iterator_nanos = 0;
  block_seek_nanos = 0;
  find_table_nanos = 0;
  bloom_memtable_hit_count = 0;
  bloom_memtable_miss_count = 0;
  bloom_sst_hit_count = 0;
  bloom_sst_miss_count = 0;
  key_lock_wait_time = 0;
  key_lock_wait_count = 0;

  env_new_sequential_file_nanos = 0;
  env_new_random_access_file_nanos = 0;
  env_new_writable_file_nanos = 0;
  env_reuse_writable_file_nanos = 0;
  env_new_random_rw_file_nanos = 0;
  env_new_directory_nanos = 0;
  env_file_exists_nanos = 0;
  env_get_children_nanos = 0;
  env_get_children_file_attributes_nanos = 0;
  env_delete_file_nanos = 0;
  env_create_dir_nanos = 0;
  env_create_dir_if_missing_nanos = 0;
  env_delete_dir_nanos = 0;
  env_get_file_size_nanos = 0;
  env_get_file_modification_time_nanos = 0;
  env_rename_file_nanos = 0;
  env_link_file_nanos = 0;
  env_lock_file_nanos = 0;
  env_unlock_file_nanos = 0;
  env_new_logger_nanos = 0;
  get_cpu_nanos = 0;
  iter_next_cpu_nanos = 0;
  iter_prev_cpu_nanos = 0;
  iter_seek_cpu_nanos = 0;
  number_async_seek = 0;
  level_to_perf_context.resize(0);
#endif
}

#define PERF_CONTEXT_OUTPUT(counter)             \
  if (!exclude_zero_counters || (counter > 0)) { \
    ss << #counter << " = " << counter << ", ";  \
  }

#define PERF_CONTEXT_BY_LEVEL_OUTPUT_ONE_COUNTER(counter)         \
  if (per_level_perf_context_enabled) {                           \
    ss << #counter << " = ";                                      \
    const size_t num_levels = level_to_perf_context.size();       \
    for (size_t level = 0; level < num_levels; ++level) {         \
      const auto& perf = level_to_perf_context[level];            \
      if (!exclude_zero_counters || (perf.counter > 0)) {         \
        ss << perf.counter << "@level" << level << ", ";          \
      }                                                           \
    }                                                             \
  }

void PerfContextByLevel::Reset() {
#ifndef NPERF_CONTEXT
  bloom_filter_useful = 0;
  bloom_filter_full_positive = 0;
  bloom_filter_full_true_positive = 0;
  user_key_return_count = 0;
  get_from_table_nanos = 0;
  block_cache_hit_count = 0;
  block_cache_miss_count = 0;
#endif
}

std::string PerfContext::ToString(bool exclude_zero_counters) const {
#ifdef NPERF_CONTEXT
  (void)exclude_zero_counters;
  return "";
#else
  std::ostringstream ss;
  PERF_CONTEXT_OUTPUT(user_key_comparison_count);
  PERF_CONTEXT_OUTPUT(block_cache_hit_count);
  PERF_CONTEXT_OUTPUT(block_read_count);
  PERF_CONTEXT_OUTPUT(block_read_byte);
  PERF_CONTEXT_OUTPUT(block_read_time);
  PERF_CONTEXT_OUTPUT(block_cache_index_hit_count);
  PERF_CONTEXT_OUTPUT(block_cache_standalone_handle_count);
  PERF_CONTEXT_OUTPUT(block_cache_real_handle_count);
  PERF_CONTEXT_OUTPUT(index_block_read_count);
  PERF_CONTEXT_OUTPUT(block_cache_filter_hit_count);
  PERF_CONTEXT_OUTPUT(filter_block_read_count);
  PERF_CONTEXT_OUTPUT(compression_dict_block_read_count);
  PERF_CONTEXT_OUTPUT(secondary_cache_hit_count);
  PERF_CONTEXT_OUTPUT(compressed_sec_cache_insert_real_count);
  PERF_CONTEXT_OUTPUT(compressed_sec_cache_insert_dummy_count);
  PERF_CONTEXT_OUTPUT(compressed_sec_cache_uncompressed_bytes);
  PERF_CONTEXT_OUTPUT(compressed_sec_cache_compressed_bytes);
  PERF_CONTEXT_OUTPUT(block_checksum_time);
  PERF_CONTEXT_OUTPUT(block_decompress_time);
  PERF_CONTEXT_OUTPUT(get_read_bytes);
  PERF_CONTEXT_OUTPUT(multiget_read_bytes);
  PERF_CONTEXT_OUTPUT(iter_read_bytes);
  PERF_CONTEXT_OUTPUT(blob_cache_hit_count);
  PERF_CONTEXT_OUTPUT(blob_read_count);
  PERF_CONTEXT_OUTPUT(blob_read_byte);
  PERF_CONTEXT_OUTPUT(blob_read_time);
  PERF_CONTEXT_OUTPUT(blob_checksum_time);
  PERF_CONTEXT_OUTPUT(blob_decompress_time);
  PERF_CONTEXT_OUTPUT(internal_key_skipped_count);
  PERF_CONTEXT_OUTPUT(internal_delete_skipped_count);
  PERF_CONTEXT_OUTPUT(internal_recent_skipped_count);
  PERF_CONTEXT_OUTPUT(internal_merge_count);
  PERF_CONTEXT_OUTPUT(internal_range_del_reseek_count);
  PERF_CONTEXT_OUTPUT(write_wal_time);
  PERF_CONTEXT_OUTPUT(get_snapshot_time);
  PERF_CONTEXT_OUTPUT(get_from_memtable_time);
  PERF_CONTEXT_OUTPUT(get_from_memtable_count);
  PERF_CONTEXT_OUTPUT(get_post_process_time);
  PERF_CONTEXT_OUTPUT(get_from_output_files_time);
  PERF_CONTEXT_OUTPUT(seek_on_memtable_time);
  PERF_CONTEXT_OUTPUT(seek_on_memtable_count);
  PERF_CONTEXT_OUTPUT(next_on_memtable_count);
  PERF_CONTEXT_OUTPUT(prev_on_memtable_count);
  PERF_CONTEXT_OUTPUT(seek_child_seek_time);
  PERF_CONTEXT_OUTPUT(seek_child_seek_count);
  PERF_CONTEXT_OUTPUT(seek_min_heap_time);
  PERF_CONTEXT_OUTPUT(seek_internal_seek_time);
  PERF_CONTEXT_OUTPUT(find_next_user_entry_time);
  PERF_CONTEXT_OUTPUT(write_pre_and_post_process_time);
  PERF_CONTEXT_OUTPUT(write_memtable_time);
  PERF_CONTEXT_OUTPUT(write_thread_wait_nanos);
  PERF_CONTEXT_OUTPUT(write_scheduling_flushes_compactions_time);
  PERF_CONTEXT_OUTPUT(db_mutex_lock_nanos);
  PERF_CONTEXT_OUTPUT(db_condition_wait_nanos);
  PERF_CONTEXT_OUTPUT(merge_operator_time_nanos);
  PERF_CONTEXT_OUTPUT(write_delay_time);
  PERF_CONTEXT_OUTPUT(read_index_block_nanos);
  PERF_CONTEXT_OUTPUT(read_filter_block_nanos);
  PERF_CONTEXT_OUTPUT(new_table_block_iter_nanos);
  PERF_CONTEXT_OUTPUT(new_table_iterator_nanos);
  PERF_CONTEXT_OUTPUT(block_seek_nanos);
  PERF_CONTEXT_OUTPUT(find_table_nanos);
  PERF_CONTEXT_OUTPUT(bloom_memtable_hit_count);
  PERF_CONTEXT_OUTPUT(bloom_memtable_miss_count);
  PERF_CONTEXT_OUTPUT(bloom_sst_hit_count);
  PERF_CONTEXT_OUTPUT(bloom_sst_miss_count);
  PERF_CONTEXT_OUTPUT(key_lock_wait_time);
  PERF_CONTEXT_OUTPUT(key_lock_wait_count);
  PERF_CONTEXT_OUTPUT(env_new_sequential_file_nanos);
  PERF_CONTEXT_OUTPUT(env_new_random_access_file_nanos);
  PERF_CONTEXT_OUTPUT(env_new_writable_file_nanos);
  PERF_CONTEXT_OUTPUT(env_reuse_writable_file_nanos);
  PERF_CONTEXT_OUTPUT(env_new_random_rw_file_nanos);
  PERF_CONTEXT_OUTPUT(env_new_directory_nanos);
  PERF_CONTEXT_OUTPUT(env_file_exists_nanos);
  PERF_CONTEXT_OUTPUT(env_get_children_nanos);
  PERF_CONTEXT_OUTPUT(env_get_children_file_attributes_nanos);
  PERF_CONTEXT_OUTPUT(env_delete_file_nanos);
  PERF_CONTEXT_OUTPUT(env_create_dir_nanos);
  PERF_CONTEXT_OUTPUT(env_create_dir_if_missing_nanos);
  PERF_CONTEXT_OUTPUT(env_delete_dir_nanos);
  PERF_CONTEXT_OUTPUT(env_get_file_size_nanos);
  PERF_CONTEXT_OUTPUT(env_get_file_modification_time_nanos);
  PERF_CONTEXT_OUTPUT(env_rename_file_nanos);
  PERF_CONTEXT_OUTPUT(env_link_file_nanos);
  PERF_CONTEXT_OUTPUT(env_lock_file_nanos);
  PERF_CONTEXT_OUTPUT(env_unlock_file_nanos);
  PERF_CONTEXT_OUTPUT(env_new_logger_nanos);
  PERF_CONTEXT_OUTPUT(get_cpu_nanos);
  PERF_CONTEXT_OUTPUT(iter_next_cpu_nanos);
  PERF_CONTEXT_OUTPUT(iter_prev_cpu_nanos);
  PERF_CONTEXT_OUTPUT(iter_seek_cpu_nanos);
  PERF_CONTEXT_OUTPUT(number_async_seek);
  PERF_CONTEXT_BY_LEVEL_OUTPUT_ONE_COUNTER(bloom_filter_useful);
  PERF_CONTEXT_BY_LEVEL_OUTPUT_ONE_COUNTER(bloom_filter_full_positive);
  PERF_CONTEXT_BY_LEVEL_OUTPUT_ONE_COUNTER(bloom_filter_full_true_positive);
  PERF_CONTEXT_BY_LEVEL_OUTPUT_ONE_COUNTER(user_key_return_count);
  PERF_CONTEXT_BY_LEVEL_OUTPUT_ONE_COUNTER(get_from_table_nanos);
  PERF_CONTEXT_BY_LEVEL_OUTPUT_ONE_COUNTER(block_cache_hit_count);
  PERF_CONTEXT_BY_LEVEL_OUTPUT_ONE_COUNTER(block_cache_miss_count);

  std::string str = ss.str();
  str.erase(str.find_last_not_of(", ") + 1);
  return str;
#endif
}

void PerfContext::EnablePerLevelPerfContext() {
  per_level_perf_context_enabled = true;
}

void PerfContext::DisablePerLevelPerfContext() {
  per_level_perf_context_enabled = false;
}

void PerfContext::ClearPerLevelPerfContext(){
  level_to_perf_context.resize(0);
  per_level_perf_context_enabled = false;
}

}  // namespace ROCKSDB_NAMESPACE
