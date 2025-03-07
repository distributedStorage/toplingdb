//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#ifndef ROCKSDB_LITE

#include "rocksdb/utilities/write_batch_with_index.h"

#include <memory>

#include "db/column_family.h"
#include "db/db_impl/db_impl.h"
#include "db/merge_context.h"
#include "db/merge_helper.h"
#include "memory/arena.h"
#include "memtable/skiplist.h"
#include "options/db_options.h"
#include "rocksdb/comparator.h"
#include "rocksdb/iterator.h"
#include "util/cast_util.h"
#include "util/string_util.h"
#include "utilities/write_batch_with_index/write_batch_with_index_internal.h"

#include <terark/util/function.hpp>

namespace ROCKSDB_NAMESPACE {
struct WriteBatchWithIndex::Rep {
  explicit Rep(const Comparator* index_comparator, size_t reserved_bytes = 0,
               size_t max_bytes = 0, bool _overwrite_key = false,
               size_t protection_bytes_per_key = 0)
      : write_batch(reserved_bytes, max_bytes, protection_bytes_per_key,
                    index_comparator ? index_comparator->timestamp_size() : 0),
        comparator(index_comparator, &write_batch),
        skip_list(comparator, &arena),
        overwrite_key(_overwrite_key),
        last_entry_offset(0),
        last_sub_batch_offset(0),
        sub_batch_cnt(1) {}
  ReadableWriteBatch write_batch;
  WriteBatchEntryComparator comparator;
  Arena arena;
  WriteBatchEntrySkipList skip_list;
  bool overwrite_key;
  size_t last_entry_offset;
  // The starting offset of the last sub-batch. A sub-batch starts right before
  // inserting a key that is a duplicate of a key in the last sub-batch. Zero,
  // the default, means that no duplicate key is detected so far.
  size_t last_sub_batch_offset;
  // Total number of sub-batches in the write batch. Default is 1.
  size_t sub_batch_cnt;

  // Remember current offset of internal write batch, which is used as
  // the starting offset of the next record.
  void SetLastEntryOffset() { last_entry_offset = write_batch.GetDataSize(); }

  // In overwrite mode, find the existing entry for the same key and update it
  // to point to the current entry.
  // Return true if the key is found and updated.
  bool UpdateExistingEntry(ColumnFamilyHandle* column_family, const Slice& key,
                           WriteType type);
  bool UpdateExistingEntryWithCfId(uint32_t column_family_id, const Slice& key,
                                   WriteType type);

  // Add the recent entry to the update.
  // In overwrite mode, if key already exists in the index, update it.
  void AddOrUpdateIndex(ColumnFamilyHandle* column_family, const Slice& key,
                        WriteType type);
  void AddOrUpdateIndex(const Slice& key, WriteType type);

  // Allocate an index entry pointing to the last entry in the write batch and
  // put it to skip list.
  void AddNewEntry(uint32_t column_family_id);

  // Clear all updates buffered in this batch.
  void Clear();
  void ClearIndex();

  // Rebuild index by reading all records from the batch.
  // Returns non-ok status on corruption.
  Status ReBuildIndex();
};

bool WriteBatchWithIndex::Rep::UpdateExistingEntry(
    ColumnFamilyHandle* column_family, const Slice& key, WriteType type) {
  uint32_t cf_id = GetColumnFamilyID(column_family);
  return UpdateExistingEntryWithCfId(cf_id, key, type);
}

bool WriteBatchWithIndex::Rep::UpdateExistingEntryWithCfId(
    uint32_t column_family_id, const Slice& key, WriteType type) {
  if (!overwrite_key) {
    return false;
  }

  WBWIIteratorImpl iter(column_family_id, &skip_list, &write_batch,
                        &comparator);
  iter.Seek(key);
  if (!iter.Valid()) {
    return false;
  } else if (!iter.MatchesKey(column_family_id, key)) {
    return false;
  } else {
    // Move to the end of this key (NextKey-Prev)
    iter.NextKey();  // Move to the next key
    if (iter.Valid()) {
      iter.Prev();  // Move back one entry
    } else {
      iter.SeekToLast();
    }
  }
  WriteBatchIndexEntry* non_const_entry =
      const_cast<WriteBatchIndexEntry*>(iter.GetRawEntry());
  if (LIKELY(last_sub_batch_offset <= non_const_entry->offset)) {
    last_sub_batch_offset = last_entry_offset;
    sub_batch_cnt++;
  }
  if (type == kMergeRecord) {
    return false;
  } else {
    non_const_entry->offset = last_entry_offset;
    return true;
  }
}

void WriteBatchWithIndex::Rep::AddOrUpdateIndex(
    ColumnFamilyHandle* column_family, const Slice& key, WriteType type) {
  if (!UpdateExistingEntry(column_family, key, type)) {
    uint32_t cf_id = GetColumnFamilyID(column_family);
    const auto* cf_cmp = GetColumnFamilyUserComparator(column_family);
    if (cf_cmp != nullptr) {
      comparator.SetComparatorForCF(cf_id, cf_cmp);
    }
    AddNewEntry(cf_id);
  }
}

void WriteBatchWithIndex::Rep::AddOrUpdateIndex(const Slice& key,
                                                WriteType type) {
  if (!UpdateExistingEntryWithCfId(0, key, type)) {
    AddNewEntry(0);
  }
}

void WriteBatchWithIndex::Rep::AddNewEntry(uint32_t column_family_id) {
  const std::string& wb_data = write_batch.Data();
  Slice entry_ptr = Slice(wb_data.data() + last_entry_offset,
                          wb_data.size() - last_entry_offset);
  // Extract key
  Slice key;
  bool success =
      ReadKeyFromWriteBatchEntry(&entry_ptr, &key, column_family_id != 0);
#ifdef NDEBUG
  (void)success;
#endif
  assert(success);

  const Comparator* const ucmp = comparator.GetComparator(column_family_id);
  size_t ts_sz = ucmp ? ucmp->timestamp_size() : 0;

  if (ts_sz > 0) {
    key.remove_suffix(ts_sz);
  }

  auto* mem = arena.Allocate(sizeof(WriteBatchIndexEntry));
  auto* index_entry =
      new (mem) WriteBatchIndexEntry(last_entry_offset, column_family_id,
                                     key.data() - wb_data.data(), key.size());
  skip_list.Insert(index_entry);
}

void WriteBatchWithIndex::Rep::Clear() {
  write_batch.Clear();
  ClearIndex();
}

void WriteBatchWithIndex::Rep::ClearIndex() {
  skip_list.~WriteBatchEntrySkipList();
  arena.~Arena();
  new (&arena) Arena();
  new (&skip_list) WriteBatchEntrySkipList(comparator, &arena);
  last_entry_offset = 0;
  last_sub_batch_offset = 0;
  sub_batch_cnt = 1;
}

Status WriteBatchWithIndex::Rep::ReBuildIndex() {
  Status s;

  ClearIndex();

  if (write_batch.Count() == 0) {
    // Nothing to re-index
    return s;
  }

  size_t offset = WriteBatchInternal::GetFirstOffset(&write_batch);

  Slice input(write_batch.Data());
  input.remove_prefix(offset);

  // Loop through all entries in Rep and add each one to the index
  uint32_t found = 0;
  while (s.ok() && !input.empty()) {
    Slice key, value, blob, xid;
    uint32_t column_family_id = 0;  // default
    char tag = 0;

    // set offset of current entry for call to AddNewEntry()
    last_entry_offset = input.data() - write_batch.Data().data();

    s = ReadRecordFromWriteBatch(&input, &tag, &column_family_id, &key, &value,
                                 &blob, &xid);
    if (!s.ok()) {
      break;
    }

    switch (tag) {
      case kTypeColumnFamilyValue:
      case kTypeValue:
        found++;
        if (!UpdateExistingEntryWithCfId(column_family_id, key, kPutRecord)) {
          AddNewEntry(column_family_id);
        }
        break;
      case kTypeColumnFamilyDeletion:
      case kTypeDeletion:
        found++;
        if (!UpdateExistingEntryWithCfId(column_family_id, key,
                                         kDeleteRecord)) {
          AddNewEntry(column_family_id);
        }
        break;
      case kTypeColumnFamilySingleDeletion:
      case kTypeSingleDeletion:
        found++;
        if (!UpdateExistingEntryWithCfId(column_family_id, key,
                                         kSingleDeleteRecord)) {
          AddNewEntry(column_family_id);
        }
        break;
      case kTypeColumnFamilyMerge:
      case kTypeMerge:
        found++;
        if (!UpdateExistingEntryWithCfId(column_family_id, key, kMergeRecord)) {
          AddNewEntry(column_family_id);
        }
        break;
      case kTypeLogData:
      case kTypeBeginPrepareXID:
      case kTypeBeginPersistedPrepareXID:
      case kTypeBeginUnprepareXID:
      case kTypeEndPrepareXID:
      case kTypeCommitXID:
      case kTypeCommitXIDAndTimestamp:
      case kTypeRollbackXID:
      case kTypeNoop:
        break;
      default:
        return Status::Corruption(
            "unknown WriteBatch tag in ReBuildIndex",
            std::to_string(static_cast<unsigned int>(tag)));
    }
  }

  if (s.ok() && found != write_batch.Count()) {
    s = Status::Corruption("WriteBatch has wrong count");
  }

  return s;
}

WriteBatchWithIndex::WriteBatchWithIndex(
    const Comparator* default_index_comparator, size_t reserved_bytes,
    bool overwrite_key, size_t max_bytes, size_t protection_bytes_per_key)
    : rep(new Rep(default_index_comparator, reserved_bytes, max_bytes,
                  overwrite_key, protection_bytes_per_key)) {}

WriteBatchWithIndex::WriteBatchWithIndex(Slice/*placeholder*/) {}

WriteBatchWithIndex::~WriteBatchWithIndex() {}

const Comparator* WriteBatchWithIndex::GetUserComparator(uint32_t cf_id) const {
  return rep->comparator.GetComparator(cf_id);
}

WriteBatch* WriteBatchWithIndex::GetWriteBatch() { return &rep->write_batch; }

size_t WriteBatchWithIndex::SubBatchCnt() { return rep->sub_batch_cnt; }

WBWIIterator* WriteBatchWithIndex::NewIterator() {
  return new WBWIIteratorImpl(0, &(rep->skip_list), &rep->write_batch,
                              &(rep->comparator));
}

WBWIIterator* WriteBatchWithIndex::NewIterator(
    ColumnFamilyHandle* column_family) {
  return new WBWIIteratorImpl(GetColumnFamilyID(column_family),
                              &(rep->skip_list), &rep->write_batch,
                              &(rep->comparator));
}

Iterator* WriteBatchWithIndex::NewIteratorWithBase(
    ColumnFamilyHandle* column_family, Iterator* base_iterator,
    const ReadOptions* read_options) {
  auto wbwiii =
      new WBWIIteratorImpl(GetColumnFamilyID(column_family), &(rep->skip_list),
                           &rep->write_batch, &rep->comparator);
  return new BaseDeltaIterator(column_family, base_iterator, wbwiii,
                               GetColumnFamilyUserComparator(column_family),
                               read_options);
}

Iterator* WriteBatchWithIndex::NewIteratorWithBase(Iterator* base_iterator) {
  // default column family's comparator
  auto wbwiii = new WBWIIteratorImpl(0, &(rep->skip_list), &rep->write_batch,
                                     &rep->comparator);
  return new BaseDeltaIterator(nullptr, base_iterator, wbwiii,
                               rep->comparator.default_comparator());
}

Status WriteBatchWithIndex::Put(ColumnFamilyHandle* column_family,
                                const Slice& key, const Slice& value) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Put(column_family, key, value);
  if (s.ok()) {
    rep->AddOrUpdateIndex(column_family, key, kPutRecord);
  }
  return s;
}

Status WriteBatchWithIndex::Put(const Slice& key, const Slice& value) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Put(key, value);
  if (s.ok()) {
    rep->AddOrUpdateIndex(key, kPutRecord);
  }
  return s;
}

Status WriteBatchWithIndex::Put(ColumnFamilyHandle* column_family,
                                const Slice& /*key*/, const Slice& /*ts*/,
                                const Slice& /*value*/) {
  if (!column_family) {
    return Status::InvalidArgument("column family handle cannot be nullptr");
  }
  // TODO: support WBWI::Put() with timestamp.
  return Status::NotSupported();
}

Status WriteBatchWithIndex::Delete(ColumnFamilyHandle* column_family,
                                   const Slice& key) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Delete(column_family, key);
  if (s.ok()) {
    rep->AddOrUpdateIndex(column_family, key, kDeleteRecord);
  }
  return s;
}

Status WriteBatchWithIndex::Delete(const Slice& key) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Delete(key);
  if (s.ok()) {
    rep->AddOrUpdateIndex(key, kDeleteRecord);
  }
  return s;
}

Status WriteBatchWithIndex::Delete(ColumnFamilyHandle* column_family,
                                   const Slice& /*key*/, const Slice& /*ts*/) {
  if (!column_family) {
    return Status::InvalidArgument("column family handle cannot be nullptr");
  }
  // TODO: support WBWI::Delete() with timestamp.
  return Status::NotSupported();
}

Status WriteBatchWithIndex::SingleDelete(ColumnFamilyHandle* column_family,
                                         const Slice& key) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.SingleDelete(column_family, key);
  if (s.ok()) {
    rep->AddOrUpdateIndex(column_family, key, kSingleDeleteRecord);
  }
  return s;
}

Status WriteBatchWithIndex::SingleDelete(const Slice& key) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.SingleDelete(key);
  if (s.ok()) {
    rep->AddOrUpdateIndex(key, kSingleDeleteRecord);
  }
  return s;
}

Status WriteBatchWithIndex::SingleDelete(ColumnFamilyHandle* column_family,
                                         const Slice& /*key*/,
                                         const Slice& /*ts*/) {
  if (!column_family) {
    return Status::InvalidArgument("column family handle cannot be nullptr");
  }
  // TODO: support WBWI::SingleDelete() with timestamp.
  return Status::NotSupported();
}

Status WriteBatchWithIndex::Merge(ColumnFamilyHandle* column_family,
                                  const Slice& key, const Slice& value) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Merge(column_family, key, value);
  if (s.ok()) {
    rep->AddOrUpdateIndex(column_family, key, kMergeRecord);
  }
  return s;
}

Status WriteBatchWithIndex::Merge(const Slice& key, const Slice& value) {
  rep->SetLastEntryOffset();
  auto s = rep->write_batch.Merge(key, value);
  if (s.ok()) {
    rep->AddOrUpdateIndex(key, kMergeRecord);
  }
  return s;
}

Status WriteBatchWithIndex::PutLogData(const Slice& blob) {
  return rep->write_batch.PutLogData(blob);
}

void WriteBatchWithIndex::Clear() { rep->Clear(); }

Status WriteBatchWithIndex::GetFromBatch(ColumnFamilyHandle* column_family,
                                         const DBOptions& options,
                                         const Slice& key, std::string* value) {
  Status s;
  WriteBatchWithIndexInternal wbwii(&options, column_family);
  auto result = wbwii.GetFromBatch(this, key, value, &s);

  switch (result) {
    case WBWIIteratorImpl::kFound:
    case WBWIIteratorImpl::kError:
      // use returned status
      break;
    case WBWIIteratorImpl::kDeleted:
    case WBWIIteratorImpl::kNotFound:
      s = Status::NotFound();
      break;
    case WBWIIteratorImpl::kMergeInProgress:
      s = Status::MergeInProgress();
      break;
    default:
      assert(false);
  }

  return s;
}

WBWIIterator::Result
WriteBatchWithIndex::GetFromBatchRaw(DB* db, ColumnFamilyHandle* cfh,
    const Slice& key, MergeContext* merge_context, std::string* value,
    Status* s) {
  WriteBatchWithIndexInternal wbwii(db, cfh);
  return wbwii.GetFromBatch(this, key, merge_context, value, s);
}

Status WriteBatchWithIndex::MergeKey(
          DB* db, ColumnFamilyHandle* column_family,
          const Slice& key, const Slice* origin_value,
          std::string* result, const MergeContext& mgcontext) {
  if (UNLIKELY(nullptr == column_family)) {
    return Status::InvalidArgument("Must provide a column_family");
  }
  auto cfh = static_cast<ColumnFamilyHandleImpl*>(column_family);
  const auto merge_operator = cfh->cfd()->ioptions()->merge_operator.get();
  if (UNLIKELY(merge_operator == nullptr)) {
    return Status::InvalidArgument(
        "Merge_operator must be set for column_family");
  }
  auto& idbo = static_cast<DBImpl*>(db->GetRootDB())->immutable_db_options();
  auto* statistics = idbo.statistics.get();
  auto* logger = idbo.info_log.get();
  auto* clock = idbo.clock;
  return MergeHelper::TimedFullMerge(merge_operator, key, origin_value,
                                     mgcontext.GetOperands(), result, logger,
                                     statistics, clock
#if (ROCKSDB_MAJOR * 10000 + ROCKSDB_MINOR * 10 + ROCKSDB_PATCH) >= 70090
                                     , nullptr // result_operand
                                     , true // update_num_ops_stats
#endif
                                     );
}

Status WriteBatchWithIndex::MergeKey(
          const DBOptions& options, ColumnFamilyHandle* column_family,
          const Slice& key, const Slice* origin_value,
          std::string* result, const MergeContext& mgcontext) {
  if (UNLIKELY(nullptr == column_family)) {
    return Status::InvalidArgument("Must provide a column_family");
  }
  auto cfh = static_cast<ColumnFamilyHandleImpl*>(column_family);
  const auto merge_operator = cfh->cfd()->ioptions()->merge_operator.get();
  if (UNLIKELY(merge_operator == nullptr)) {
    return Status::InvalidArgument(
        "Merge_operator must be set for column_family");
  }
  auto* statistics = options.statistics.get();
  auto* logger = options.info_log.get();
  auto* clock = options.env->GetSystemClock().get();
  return MergeHelper::TimedFullMerge(merge_operator, key, origin_value,
                                      mgcontext.GetOperands(), result, logger,
                                      statistics, clock
#if (ROCKSDB_MAJOR * 10000 + ROCKSDB_MINOR * 10 + ROCKSDB_PATCH) >= 70090
                                     , nullptr // result_operand
                                     , true // update_num_ops_stats
#endif
                                     );
}

Status WriteBatchWithIndex::GetFromBatchAndDB(DB* db,
                                              const ReadOptions& read_options,
                                              const Slice& key,
                                              std::string* value) {
  assert(value != nullptr);
  PinnableSlice pinnable_val(value);
  assert(!pinnable_val.IsPinned());
  auto s = GetFromBatchAndDB(db, read_options, db->DefaultColumnFamily(), key,
                             &pinnable_val);
  if (s.ok() && pinnable_val.IsPinned()) {
    value->assign(pinnable_val.data(), pinnable_val.size());
  }  // else value is already assigned
  return s;
}

Status WriteBatchWithIndex::GetFromBatchAndDB(DB* db,
                                              const ReadOptions& read_options,
                                              const Slice& key,
                                              PinnableSlice* pinnable_val) {
  return GetFromBatchAndDB(db, read_options, db->DefaultColumnFamily(), key,
                           pinnable_val);
}

Status WriteBatchWithIndex::GetFromBatchAndDB(DB* db,
                                              const ReadOptions& read_options,
                                              ColumnFamilyHandle* column_family,
                                              const Slice& key,
                                              std::string* value) {
  assert(value != nullptr);
  PinnableSlice pinnable_val(value);
  assert(!pinnable_val.IsPinned());
  auto s =
      GetFromBatchAndDB(db, read_options, column_family, key, &pinnable_val);
  if (s.ok() && pinnable_val.IsPinned()) {
    value->assign(pinnable_val.data(), pinnable_val.size());
  }  // else value is already assigned
  return s;
}

Status WriteBatchWithIndex::GetFromBatchAndDB(DB* db,
                                              const ReadOptions& read_options,
                                              ColumnFamilyHandle* column_family,
                                              const Slice& key,
                                              PinnableSlice* pinnable_val) {
  return GetFromBatchAndDB(db, read_options, column_family, key, pinnable_val,
                           nullptr);
}

#define RepGetUserComparator(cfh) \
    cfh ? cfh->GetComparator() : \
    rep ? rep->comparator.GetComparator(cfh) : nullptr

Status WriteBatchWithIndex::GetFromBatchAndDB(
    DB* db, const ReadOptions& read_options, ColumnFamilyHandle* column_family,
    const Slice& key, PinnableSlice* pinnable_val, ReadCallback* callback) {
#if defined(TOPLINGDB_WITH_TIMESTAMP)
  const Comparator* const ucmp = RepGetUserComparator(column_family);
  size_t ts_sz = ucmp ? ucmp->timestamp_size() : 0;
  if (ts_sz > 0 && !read_options.timestamp) {
    return Status::InvalidArgument("Must specify timestamp");
  }
#endif

  Status s;
  WriteBatchWithIndexInternal wbwii(db, column_family);

  // Since the lifetime of the WriteBatch is the same as that of the transaction
  // we cannot pin it as otherwise the returned value will not be available
  // after the transaction finishes.
  std::string& batch_value = *pinnable_val->GetSelf();
  auto result = wbwii.GetFromBatch(this, key, &batch_value, &s);

  if (result == WBWIIteratorImpl::kFound) {
    pinnable_val->PinSelf();
    return s;
  } else if (!s.ok() || result == WBWIIteratorImpl::kError) {
    return s;
  } else if (result == WBWIIteratorImpl::kDeleted) {
    return Status::NotFound();
  }
  assert(result == WBWIIteratorImpl::kMergeInProgress ||
         result == WBWIIteratorImpl::kNotFound);

  // Did not find key in batch OR could not resolve Merges.  Try DB.
  if (!callback) {
    s = db->Get(read_options, column_family, key, pinnable_val);
  } else {
    DBImpl::GetImplOptions get_impl_options;
    get_impl_options.column_family = column_family;
    get_impl_options.value = pinnable_val;
    get_impl_options.callback = callback;
    s = static_cast_with_check<DBImpl>(db->GetRootDB())
            ->GetImpl(read_options, key, get_impl_options);
  }

  if (s.ok() || s.IsNotFound()) {  // DB Get Succeeded
    if (result == WBWIIteratorImpl::kMergeInProgress) {
      // Merge result from DB with merges in Batch
      std::string merge_result;
      if (s.ok()) {
        s = wbwii.MergeKey(key, pinnable_val, &merge_result);
      } else {  // Key not present in db (s.IsNotFound())
        s = wbwii.MergeKey(key, nullptr, &merge_result);
      }
      if (s.ok()) {
        pinnable_val->Reset();
        *pinnable_val->GetSelf() = std::move(merge_result);
        pinnable_val->PinSelf();
      }
    }
  }

  return s;
}

void WriteBatchWithIndex::MultiGetFromBatchAndDB(
    DB* db, const ReadOptions& read_options, ColumnFamilyHandle* column_family,
    const size_t num_keys, const Slice* keys, PinnableSlice* values,
    Status* statuses, bool sorted_input) {
  MultiGetFromBatchAndDB(db, read_options, column_family, num_keys, keys,
                         values, statuses, sorted_input,
                         read_options.read_callback);
}

void WriteBatchWithIndex::MultiGetFromBatchAndDB(
    DB* db, const ReadOptions& read_options, ColumnFamilyHandle* column_family,
    const size_t num_keys, const Slice* keys, PinnableSlice* values,
    Status* statuses, bool sorted_input, ReadCallback* callback) {
#if defined(TOPLINGDB_WITH_TIMESTAMP)
  const Comparator* const ucmp = RepGetUserComparator(column_family);
  size_t ts_sz = ucmp ? ucmp->timestamp_size() : 0;
  if (ts_sz > 0 && !read_options.timestamp) {
    for (size_t i = 0; i < num_keys; ++i) {
      statuses[i] = Status::InvalidArgument("Must specify timestamp");
    }
    return;
  }
#endif
  struct Elem : public MergeContext {
    Elem(WBWIIteratorImpl::Result wbwi_result1, size_t idx, MergeContext&& mg)
     : MergeContext(std::move(mg)) {
      ext_flags_ = uint32_t(idx);
      ext_uint16_ = wbwi_result1;
    }
    WBWIIteratorImpl::Result wbwi_result() const {
      return (WBWIIteratorImpl::Result)(ext_uint16_);
    }
    size_t full_index() const { return ext_flags_; }
  };
  TERARK_FAST_ALLOC(Elem, merges, num_keys);
  TERARK_FAST_ALLOC(Slice, db_keys, num_keys);
  size_t num_get_db = 0;
  for (size_t i = 0; i < num_keys; ++i) {
    MergeContext merge_context;
    std::string batch_value;
    Status* s = &statuses[i];
    PinnableSlice* pinnable_val = &values[i];
    pinnable_val->Reset();
    auto result = GetFromBatchRaw(db, column_family, keys[i],
                                  &merge_context, &batch_value, s);
    if (result == WBWIIteratorImpl::kFound) {
      *pinnable_val->GetSelf() = std::move(batch_value);
      pinnable_val->PinSelf();
      continue;
    }
    if (result == WBWIIteratorImpl::kDeleted) {
      *s = Status::NotFound();
      continue;
    }
    if (result == WBWIIteratorImpl::kError) {
      continue;
    }
    assert(result == WBWIIteratorImpl::kMergeInProgress ||
           result == WBWIIteratorImpl::kNotFound);
    db_keys[num_get_db] = keys[i];
    new(merges + num_get_db)Elem{result, i, std::move(merge_context)};
    num_get_db++;
  }
  TERARK_FAST_ARRAY(PinnableSlice, db_values, num_get_db);
  TERARK_FAST_ARRAY(Status, db_statuses, num_get_db);

  // Did not find key in batch OR could not resolve Merges.  Try DB.
  DBImpl* rdb = static_cast_with_check<DBImpl>(db->GetRootDB());

  // patch: read_options.read_callback is not thread-safe
  ReadCallback* old_callback = read_options.read_callback;
  read_options.read_callback = callback;
  rdb->MultiGet(read_options, column_family,
                num_get_db, db_keys, db_values, db_statuses);
  read_options.read_callback = old_callback;

  for (size_t index = 0; index < num_get_db; index++) {
    size_t full_index = merges[index].full_index();
    const Slice& key = db_keys[index];
    Status& s = statuses[full_index] = std::move(db_statuses[index]);
    if (s.ok() || s.IsNotFound()) { // DB Get Succeeded
      auto& mg = merges[index];
      if (mg.wbwi_result() == WBWIIteratorImpl::kMergeInProgress) {
        // topling comment: prev MergeKey() in wbwii.GetFromBatch is a waste
        std::string merged_value;
        // Merge result from DB with merges in Batch
        PinnableSlice* db_value = s.ok() ? &db_values[index] : nullptr;
        s = MergeKey(db, column_family, key, db_value, &merged_value, mg);
        if (s.ok()) {
          values[full_index].Reset();
          *values[full_index].GetSelf() = std::move(merged_value);
          values[full_index].PinSelf();
        }
      }
      else {
        values[full_index] = std::move(db_values[index]);
      }
    }
    else {
      values[full_index] = std::move(db_values[index]);
    }
  }
  TERARK_FAST_CLEAN(db_statuses, num_get_db, num_get_db);
  TERARK_FAST_CLEAN(db_values, num_get_db, num_get_db);
  TERARK_FAST_CLEAN(db_keys, num_get_db, num_keys);
  TERARK_FAST_CLEAN(merges, num_get_db, num_keys);
}

void WriteBatchWithIndex::SetSavePoint() { rep->write_batch.SetSavePoint(); }

Status WriteBatchWithIndex::RollbackToSavePoint() {
  Status s = rep->write_batch.RollbackToSavePoint();

  if (s.ok()) {
    rep->sub_batch_cnt = 1;
    rep->last_sub_batch_offset = 0;
    s = rep->ReBuildIndex();
  }

  return s;
}

Status WriteBatchWithIndex::PopSavePoint() {
  return rep->write_batch.PopSavePoint();
}

void WriteBatchWithIndex::SetMaxBytes(size_t max_bytes) {
  rep->write_batch.SetMaxBytes(max_bytes);
}

size_t WriteBatchWithIndex::GetDataSize() const {
  return rep->write_batch.GetDataSize();
}

const Comparator* WriteBatchWithIndexInternal::GetUserComparator(
    const WriteBatchWithIndex& wbwi, uint32_t cf_id) {
#if 0
  const WriteBatchEntryComparator& ucmps = wbwi.rep->comparator;
  return ucmps.GetComparator(cf_id);
#else // topling
  return wbwi.GetUserComparator(cf_id);
#endif
}

//---------------------------------------------------------------------------

WBWIFactory::~WBWIFactory() {
  // do nothing
}
class SkipListWBWIFactory : public WBWIFactory {
public:
  const char* Name() const noexcept final { return "SkipList"; }
  WriteBatchWithIndex* NewWriteBatchWithIndex(
      const Comparator* default_comparator, bool overwrite_key,
      size_t prot) final {
    return new WriteBatchWithIndex(default_comparator, 0, overwrite_key, 0, prot);
  }
};
std::shared_ptr<WBWIFactory> SingleSkipListWBWIFactory() {
  static auto fac = std::make_shared<SkipListWBWIFactory>();
  return fac;
}

}  // namespace ROCKSDB_NAMESPACE
#endif  // !ROCKSDB_LITE
