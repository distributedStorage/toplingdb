//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/merging_iterator.h"

#include "db/arena_wrapped_db_iter.h"

namespace ROCKSDB_NAMESPACE {
class MaxHeapItemComparator {
 public:
  MaxHeapItemComparator(const InternalKeyComparator* comparator)
      : comparator_(comparator) {}
  bool operator()(HeapItem* a, HeapItem* b) const {
    if (LIKELY(a->type == HeapItem::ITERATOR)) {
      if (LIKELY(b->type == HeapItem::ITERATOR)) {
        return comparator_->Compare(a->iter.key(), b->iter.key()) < 0;
      } else {
        return comparator_->Compare(a->iter.key(), b->parsed_ikey) < 0;
      }
    } else {
      if (LIKELY(b->type == HeapItem::ITERATOR)) {
        return comparator_->Compare(a->parsed_ikey, b->iter.key()) < 0;
      } else {
        return comparator_->Compare(a->parsed_ikey, b->parsed_ikey) < 0;
      }
    }
  }

 private:
  const InternalKeyComparator* comparator_;
};

#if defined(_MSC_VER) /* Visual Studio */
#define FORCE_INLINE __forceinline
#elif defined(__GNUC__)
#define FORCE_INLINE inline __attribute__((always_inline))
#else
#define FORCE_INLINE inline
#endif

#if 0
  #define bswap_prefix __bswap_64
  using UintPrefix = uint64_t;
#else
  using UintPrefix = unsigned __int128;
  #if defined(__GNUC__) && __GNUC_MINOR__ + 1000 * __GNUC__ > 12000
    #define bswap_prefix __builtin_bswap128
  #else
    FORCE_INLINE UintPrefix bswap_prefix(UintPrefix x) {
      return UintPrefix(__bswap_64(uint64_t(x))) << 64 | __bswap_64(uint64_t(x >> 64));
    }
  #endif
#endif
FORCE_INLINE UintPrefix HostPrefixCacheUK(const Slice& uk) {
  UintPrefix data;
  if (LIKELY(uk.size_ >= sizeof(UintPrefix))) {
    memcpy(&data, uk.data_, sizeof(UintPrefix));
  } else {
    data = 0;
    memcpy(&data, uk.data_, uk.size_);
  }
  if (port::kLittleEndian)
    return bswap_prefix(data);
  else
    return data;
}
FORCE_INLINE UintPrefix HostPrefixCacheIK(const Slice& ik) {
  UintPrefix data;
  if (LIKELY(ik.size_ >= sizeof(UintPrefix) + 8)) {
    memcpy(&data, ik.data_, sizeof(UintPrefix));
  } else {
    data = 0;
    memcpy(&data, ik.data_, ik.size_ - 8);
  }
  if (port::kLittleEndian)
    return bswap_prefix(data);
  else
    return data;
}

struct HeapItemAndPrefix {
  FORCE_INLINE HeapItemAndPrefix() = default;
  FORCE_INLINE HeapItemAndPrefix(HeapItem* item) : item_ptr(item) {
    iter_type = item->type;
    UpdatePrefixCache(*this);
  }
  UintPrefix key_prefix = 0;
  HeapItem* item_ptr;
  HeapItem::Type iter_type;

  HeapItem* operator->() const noexcept { return item_ptr; }

  FORCE_INLINE friend void UpdatePrefixCache(HeapItemAndPrefix& x) {
    auto p = x.item_ptr;
    if (LIKELY(HeapItem::ITERATOR == x.iter_type))
      x.key_prefix = HostPrefixCacheIK(p->iter.key());
    else
      x.key_prefix = HostPrefixCacheUK(p->parsed_ikey.user_key);
  }
};
inline static void UpdatePrefixCache(HeapItem*) {} // do nothing

static FORCE_INLINE uint64_t GetUnalignedU64(const void* ptr) noexcept {
  uint64_t x;
  memcpy(&x, ptr, sizeof(uint64_t));
  return x;
}

static FORCE_INLINE bool BytewiseCompareInternalKey(Slice x, Slice y) noexcept {
  size_t n = std::min(x.size_, y.size_) - 8;
  int cmp = memcmp(x.data_, y.data_, n);
  if (0 != cmp) return cmp < 0;
  if (x.size_ != y.size_) return x.size_ < y.size_;
  return GetUnalignedU64(x.data_ + n) > GetUnalignedU64(y.data_ + n);
}
static bool BytewiseCompareInternalKey(Slice x, const ParsedInternalKey& y)
noexcept {
  size_t nx = x.size_ - 8;
  size_t n = std::min(nx, y.user_key.size_);
  int cmp = memcmp(x.data_, y.user_key.data_, n);
  if (0 != cmp) return cmp < 0;
  if (nx != y.user_key.size_) return nx < y.user_key.size_;
  return GetUnalignedU64(x.data_ + nx) > (y.sequence << 8 | y.type);
}
static bool BytewiseCompareInternalKey(const ParsedInternalKey& x, Slice y)
noexcept {
  size_t ny = y.size_ - 8;
  size_t n = std::min(x.user_key.size_, ny);
  int cmp = memcmp(x.user_key.data_, y.data_, n);
  if (0 != cmp) return cmp < 0;
  if (x.user_key.size_ != ny) return x.user_key.size_ < ny;
  return (x.sequence << 8 | x.type) > GetUnalignedU64(y.data_ + ny);
}
static bool BytewiseCompareInternalKey(const ParsedInternalKey& x,
                                       const ParsedInternalKey& y)
noexcept {
  size_t n = std::min(x.user_key.size_, y.user_key.size_);
  int cmp = memcmp(x.user_key.data_, y.user_key.data_, n);
  if (0 != cmp) return cmp < 0;
  if (x.user_key.size_ != y.user_key.size_)
    return x.user_key.size_ < y.user_key.size_;
  else
    return (x.sequence << 8 | x.type) > (y.sequence << 8 | y.type);
}

static FORCE_INLINE bool RevBytewiseCompareInternalKey(Slice x,
                                                       Slice y) noexcept {
  size_t n = std::min(x.size_, y.size_) - 8;
  int cmp = memcmp(x.data_, y.data_, n);
  if (0 != cmp) return cmp > 0;
  if (x.size_ != y.size_) return x.size_ > y.size_;
  return GetUnalignedU64(x.data_ + n) > GetUnalignedU64(y.data_ + n);
}
static bool RevBytewiseCompareInternalKey(Slice x, const ParsedInternalKey& y)
noexcept {
  size_t nx = x.size_ - 8;
  size_t n = std::min(nx, y.user_key.size_);
  int cmp = memcmp(x.data_, y.user_key.data_, n);
  if (0 != cmp) return cmp > 0;
  if (nx != y.user_key.size_) return nx > y.user_key.size_;
  return GetUnalignedU64(x.data_ + nx) > (y.sequence << 8 | y.type);
}
static bool RevBytewiseCompareInternalKey(const ParsedInternalKey& x, Slice y)
noexcept {
  size_t ny = y.size_ - 8;
  size_t n = std::min(x.user_key.size_, ny);
  int cmp = memcmp(x.user_key.data_, y.data_, n);
  if (0 != cmp) return cmp > 0;
  if (x.user_key.size_ != ny) return x.user_key.size_ > ny;
  return (x.sequence << 8 | x.type) > GetUnalignedU64(y.data_ + ny);
}
static bool RevBytewiseCompareInternalKey(const ParsedInternalKey& x,
                                          const ParsedInternalKey& y)
noexcept {
  size_t n = std::min(x.user_key.size_, y.user_key.size_);
  int cmp = memcmp(x.user_key.data_, y.user_key.data_, n);
  if (0 != cmp) return cmp > 0;
  if (x.user_key.size_ != y.user_key.size_)
    return x.user_key.size_ > y.user_key.size_;
  else
    return (x.sequence << 8 | x.type) > (y.sequence << 8 | y.type);
}

class MinHeapBytewiseComp {
 public:
  MinHeapBytewiseComp(const InternalKeyComparator*) {}
  FORCE_INLINE
  bool operator()(HeapItemAndPrefix const &a, HeapItemAndPrefix const &b) const {
    if (a.key_prefix > b.key_prefix)
      return true;
    else if (a.key_prefix < b.key_prefix)
      return false;
    else if (LIKELY(a.iter_type == HeapItem::ITERATOR)) {
      if (LIKELY(b.iter_type == HeapItem::ITERATOR))
        return BytewiseCompareInternalKey(b->iter.key(), a->iter.key());
      else
        return BytewiseCompareInternalKey(b->parsed_ikey, a->iter.key());
    }
    else {
      if (LIKELY(b.iter_type == HeapItem::ITERATOR))
        return BytewiseCompareInternalKey(b->iter.key(), a->parsed_ikey);
      else
        return BytewiseCompareInternalKey(b->parsed_ikey, a->parsed_ikey);
    }
  }
};

class MaxHeapBytewiseComp {
 public:
  MaxHeapBytewiseComp(const InternalKeyComparator*) {}
  FORCE_INLINE
  bool operator()(HeapItemAndPrefix const &a, HeapItemAndPrefix const &b) const {
    if (a.key_prefix < b.key_prefix)
      return true;
    else if (a.key_prefix > b.key_prefix)
      return false;
    else if (LIKELY(a.iter_type == HeapItem::ITERATOR)) {
      if (LIKELY(b.iter_type == HeapItem::ITERATOR))
        return BytewiseCompareInternalKey(a->iter.key(), b->iter.key());
      else
        return BytewiseCompareInternalKey(a->iter.key(), b->parsed_ikey);
    }
    else {
      if (LIKELY(b.iter_type == HeapItem::ITERATOR))
        return BytewiseCompareInternalKey(a->parsed_ikey, b->iter.key());
      else
        return BytewiseCompareInternalKey(a->parsed_ikey, b->parsed_ikey);
    }
  }
};

class MinHeapRevBytewiseComp {
 public:
  MinHeapRevBytewiseComp(const InternalKeyComparator*) {}
  FORCE_INLINE
  bool operator()(HeapItemAndPrefix const &a, HeapItemAndPrefix const &b) const {
    if (a.key_prefix < b.key_prefix)
      return true;
    else if (a.key_prefix > b.key_prefix)
      return false;
    else if (LIKELY(a.iter_type == HeapItem::ITERATOR)) {
      if (LIKELY(b.iter_type == HeapItem::ITERATOR))
        return RevBytewiseCompareInternalKey(b->iter.key(), a->iter.key());
      else
        return RevBytewiseCompareInternalKey(b->parsed_ikey, a->iter.key());
    }
    else {
      if (LIKELY(b.iter_type == HeapItem::ITERATOR))
        return RevBytewiseCompareInternalKey(b->iter.key(), a->parsed_ikey);
      else
        return RevBytewiseCompareInternalKey(b->parsed_ikey, a->parsed_ikey);
    }
  }
};

class MaxHeapRevBytewiseComp {
 public:
  MaxHeapRevBytewiseComp(const InternalKeyComparator*) {}
  FORCE_INLINE
  bool operator()(HeapItemAndPrefix const &a, HeapItemAndPrefix const &b) const {
    if (a.key_prefix > b.key_prefix)
      return true;
    else if (a.key_prefix < b.key_prefix)
      return false;
    else if (LIKELY(a.iter_type == HeapItem::ITERATOR)) {
      if (LIKELY(b.iter_type == HeapItem::ITERATOR))
        return RevBytewiseCompareInternalKey(a->iter.key(), b->iter.key());
      else
        return RevBytewiseCompareInternalKey(a->iter.key(), b->parsed_ikey);
    }
    else {
      if (LIKELY(b.iter_type == HeapItem::ITERATOR))
        return RevBytewiseCompareInternalKey(a->parsed_ikey, b->iter.key());
      else
        return RevBytewiseCompareInternalKey(a->parsed_ikey, b->parsed_ikey);
    }
  }
};

class MergingIterator : public InternalIterator {
 public:
  // these Methods should be defined here, but for minimal diff with
  // upstream RocksDB code, we declare them as virtual here and define
  // them in derived template class
  virtual void AddIterator(InternalIterator*) = 0;
  virtual void AddRangeTombstoneIterator(TruncatedRangeDelIterator*) = 0;
  virtual void Finish() = 0;

  // We could also use an autovector with a larger reserved size.
  // HeapItem for all child point iterators.
  std::vector<HeapItem> children_;

  // range_tombstone_iters_[i] contains range tombstones in the sorted run that
  // corresponds to children_[i]. range_tombstone_iters_.empty() means not
  // handling range tombstones in merging iterator. range_tombstone_iters_[i] ==
  // nullptr means the sorted run of children_[i] does not have range
  // tombstones.
  terark::valvec32<TruncatedRangeDelIterator*> range_tombstone_iters_;
};

template <class MinHeapComparator, class MaxHeapComparator, class Item = HeapItemAndPrefix>
class MergingIterTmpl final : public MergingIterator {
  using  MergerMinIterHeap = BinaryHeap<Item, MinHeapComparator>;
  struct MergerMaxIterHeap : BinaryHeap<Item, MaxHeapComparator> {
    // to minimize code diff, do not need change maxHeap_->xxx to maxHeap_.xxx
    inline MergerMaxIterHeap* operator->() { return this; }
    inline const MergerMaxIterHeap* operator->() const { return this; }
  };
  static_assert(sizeof(MergerMinIterHeap) == sizeof(MergerMaxIterHeap));

public:
  MergingIterTmpl(const InternalKeyComparator* comparator,
                  InternalIterator** children, int n, bool is_arena_mode,
                  bool prefix_seek_mode,
                  const Slice* iterate_upper_bound = nullptr)
      : is_arena_mode_(is_arena_mode),
        prefix_seek_mode_(prefix_seek_mode),
        direction_(kForward),
        comparator_(comparator),
        current_(nullptr),
        minHeap_(MinHeapComparator(comparator_)),
        pinned_iters_mgr_(nullptr),
        iterate_upper_bound_(iterate_upper_bound) {
    children_.resize(n);
    for (int i = 0; i < n; i++) {
      children_[i].level = i;
      children_[i].iter.Set(children[i]);
    }
  }

  void considerStatus(Status s) {
    if (!s.ok() && status_.ok()) {
      status_ = s;
    }
  }

  virtual void AddIterator(InternalIterator* iter) {
    children_.emplace_back(children_.size(), iter);
    if (pinned_iters_mgr_) {
      iter->SetPinnedItersMgr(pinned_iters_mgr_);
    }
    // Invalidate to ensure `Seek*()` is called to construct the heaps before
    // use.
    current_ = nullptr;
  }

  // Merging iterator can optionally process range tombstones: if a key is
  // covered by a range tombstone, the merging iterator will not output it but
  // skip it.
  //
  // Add the next range tombstone iterator to this merging iterator.
  // There must be either no range tombstone iterator, or same number of
  // range tombstone iterators as point iterators after all range tombstone
  // iters are added. The i-th added range tombstone iterator and the i-th point
  // iterator must point to the same sorted run.
  // Merging iterator takes ownership of the range tombstone iterator and
  // is responsible for freeing it. Note that during Iterator::Refresh()
  // and when a level iterator moves to a different SST file, the range
  // tombstone iterator could be updated. In that case, the merging iterator
  // is only responsible to freeing the new range tombstone iterator
  // that it has pointers to in range_tombstone_iters_.
  void AddRangeTombstoneIterator(TruncatedRangeDelIterator* iter) {
    range_tombstone_iters_.emplace_back(iter);
  }

  // Called by MergingIteratorBuilder when all point iterators and range
  // tombstone iterators are added. Initializes HeapItems for range tombstone
  // iterators so that no further allocation is needed for HeapItem.
  void Finish() {
    if (!range_tombstone_iters_.empty()) {
      pinned_heap_item_.resize(range_tombstone_iters_.size());
      for (size_t i = 0; i < range_tombstone_iters_.size(); ++i) {
        pinned_heap_item_[i].level = i;
        // Range tombstone end key is exclusive. If a point internal key has the
        // same user key and sequence number as the start or end key of a range
        // tombstone, the order will be start < end key < internal key with the
        // following op_type change. This is helpful to ensure keys popped from
        // heap are in expected order since range tombstone start/end keys will
        // be distinct from point internal keys. Strictly speaking, this is only
        // needed for tombstone end points that are truncated in
        // TruncatedRangeDelIterator since untruncated tombstone end points
        // always have kMaxSequenceNumber and kTypeRangeDeletion (see
        // TruncatedRangeDelIterator::start_key()/end_key()).
        pinned_heap_item_[i].parsed_ikey.type = kTypeMaxValid;
      }
    }
    minHeap_.reserve(children_.size() + range_tombstone_iters_.size());
  }

  ~MergingIterTmpl() override {
    minHeap_.~MergerMinIterHeap();
    for (auto child : range_tombstone_iters_) {
      delete child;
    }

    for (auto& child : children_) {
      child.iter.DeleteIter(is_arena_mode_);
    }
    status_.PermitUncheckedError();
  }

  bool Valid() const override { return current_ != nullptr && status_.ok(); }

  Status status() const override { return status_; }

  // Add range_tombstone_iters_[level] into min heap.
  // Updates active_ if the end key of a range tombstone is inserted.
  // @param start_key specifies which end point of the range tombstone to add.
  void InsertRangeTombstoneToMinHeap(size_t level, bool start_key = true,
                                     bool replace_top = false) {
    assert(!range_tombstone_iters_.empty() &&
           range_tombstone_iters_[level]->Valid());
    if (start_key) {
      ParsedInternalKey pik = range_tombstone_iters_[level]->start_key();
      // iterate_upper_bound does not have timestamp
      if (iterate_upper_bound_ &&
          comparator_->user_comparator()->CompareWithoutTimestamp(
              pik.user_key, true /* a_has_ts */, *iterate_upper_bound_,
              false /* b_has_ts */) >= 0) {
        if (replace_top) {
          // replace_top implies this range tombstone iterator is still in
          // minHeap_ and at the top.
          minHeap_.pop();
        }
        return;
      }
      pinned_heap_item_[level].SetTombstoneKey(std::move(pik));
      pinned_heap_item_[level].type = HeapItem::DELETE_RANGE_START;
      assert(active_.count(level) == 0);
    } else {
      // allow end key to go over upper bound (if present) since start key is
      // before upper bound and the range tombstone could still cover a
      // range before upper bound.
      pinned_heap_item_[level].SetTombstoneKey(
          range_tombstone_iters_[level]->end_key());
      pinned_heap_item_[level].type = HeapItem::DELETE_RANGE_END;
      active_.insert(level);
    }
    if (replace_top) {
      minHeap_.replace_top(&pinned_heap_item_[level]);
    } else {
      minHeap_.push(&pinned_heap_item_[level]);
    }
  }

  // Add range_tombstone_iters_[level] into max heap.
  // Updates active_ if the start key of a range tombstone is inserted.
  // @param end_key specifies which end point of the range tombstone to add.
  void InsertRangeTombstoneToMaxHeap(size_t level, bool end_key = true,
                                     bool replace_top = false) {
    assert(!range_tombstone_iters_.empty() &&
           range_tombstone_iters_[level]->Valid());
    if (end_key) {
      pinned_heap_item_[level].SetTombstoneKey(
          range_tombstone_iters_[level]->end_key());
      pinned_heap_item_[level].type = HeapItem::DELETE_RANGE_END;
      assert(active_.count(level) == 0);
    } else {
      pinned_heap_item_[level].SetTombstoneKey(
          range_tombstone_iters_[level]->start_key());
      pinned_heap_item_[level].type = HeapItem::DELETE_RANGE_START;
      active_.insert(level);
    }
    if (replace_top) {
      maxHeap_->replace_top(&pinned_heap_item_[level]);
    } else {
      maxHeap_->push(&pinned_heap_item_[level]);
    }
  }

  // Remove HeapItems from top of minHeap_ that are of type DELETE_RANGE_START
  // until minHeap_ is empty or the top of the minHeap_ is not of type
  // DELETE_RANGE_START. Each such item means a range tombstone becomes active,
  // so `active_` is updated accordingly.
  void PopDeleteRangeStart() {
    while (!minHeap_.empty() &&
           minHeap_.top()->type == HeapItem::DELETE_RANGE_START) {
      TEST_SYNC_POINT_CALLBACK("MergeIterator::PopDeleteRangeStart", nullptr);
      // insert end key of this range tombstone and updates active_
      InsertRangeTombstoneToMinHeap(
          minHeap_.top()->level, false /* start_key */, true /* replace_top */);
    }
  }

  // Remove HeapItems from top of maxHeap_ that are of type DELETE_RANGE_END
  // until maxHeap_ is empty or the top of the maxHeap_ is not of type
  // DELETE_RANGE_END. Each such item means a range tombstone becomes active,
  // so `active_` is updated accordingly.
  void PopDeleteRangeEnd() {
    while (!maxHeap_->empty() &&
           maxHeap_->top()->type == HeapItem::DELETE_RANGE_END) {
      // insert start key of this range tombstone and updates active_
      InsertRangeTombstoneToMaxHeap(maxHeap_->top()->level, false /* end_key */,
                                    true /* replace_top */);
    }
  }

  void SeekToFirst() override {
    ClearHeaps();
    status_ = Status::OK();
    for (auto& child : children_) {
      child.iter.SeekToFirst();
      AddToMinHeapOrCheckStatus(&child);
    }

    for (size_t i = 0; i < range_tombstone_iters_.size(); ++i) {
      if (range_tombstone_iters_[i]) {
        range_tombstone_iters_[i]->SeekToFirst();
        if (range_tombstone_iters_[i]->Valid()) {
          // It is possible to be invalid due to snapshots.
          InsertRangeTombstoneToMinHeap(i);
        }
      }
    }
    FindNextVisibleKey();
    direction_ = kForward;
    current_ = CurrentForward();
  }

  void SeekToLast() override {
    ClearHeaps();
    InitMaxHeap();
    status_ = Status::OK();
    for (auto& child : children_) {
      child.iter.SeekToLast();
      AddToMaxHeapOrCheckStatus(&child);
    }

    for (size_t i = 0; i < range_tombstone_iters_.size(); ++i) {
      if (range_tombstone_iters_[i]) {
        range_tombstone_iters_[i]->SeekToLast();
        if (range_tombstone_iters_[i]->Valid()) {
          // It is possible to be invalid due to snapshots.
          InsertRangeTombstoneToMaxHeap(i);
        }
      }
    }
    FindPrevVisibleKey();
    direction_ = kReverse;
    current_ = CurrentReverse();
  }

  // Position this merging iterator at the first key >= target (internal key).
  // If range tombstones are present, keys covered by range tombstones are
  // skipped, and this merging iter points to the first non-range-deleted key >=
  // target after Seek(). If !Valid() and status().ok() then end of the iterator
  // is reached.
  //
  // Internally, this involves positioning all child iterators at the first key
  // >= target. If range tombstones are present, we apply a similar
  // optimization, cascading seek, as in Pebble
  // (https://github.com/cockroachdb/pebble). Specifically, if there is a range
  // tombstone [start, end) that covers the target user key at level L, then
  // this range tombstone must cover the range [target key, end) in all levels >
  // L. So for all levels > L, we can pretend the target key is `end`. This
  // optimization is applied at each level and hence the name "cascading seek".
  // After a round of (cascading) seeks, the top of the heap is checked to see
  // if it is covered by a range tombstone (see FindNextVisibleKey() for more
  // detail), and advanced if so. The process is repeated until a
  // non-range-deleted key is at the top of the heap, or heap becomes empty.
  //
  // As mentioned in comments above HeapItem, to make the checking of whether
  // top of the heap is covered by some range tombstone efficient, we treat each
  // range deletion [start, end) as two point keys and insert them into the same
  // min/maxHeap_ where point iterators are. The set `active_` tracks the levels
  // that have active range tombstones. If level L is in `active_`, and the
  // point key at top of the heap is from level >= L, then the point key is
  // within the internal key range of the range tombstone that
  // range_tombstone_iters_[L] currently points to. For correctness reasoning,
  // one invariant that Seek() (and every other public APIs Seek*(),
  // Next/Prev()) guarantees is as follows. After Seek(), suppose `k` is the
  // current key of level L's point iterator. Then for each range tombstone
  // iterator at level <= L, it is at or before the first range tombstone with
  // end key > `k`. This ensures that when level L's point iterator reaches top
  // of the heap, `active_` is calculated correctly (it contains the covering
  // range tombstone's level if there is one), since no range tombstone iterator
  // was skipped beyond that point iterator's current key during Seek().
  // Next()/Prev() maintains a stronger version of this invariant where all
  // range tombstone iterators from level <= L are *at* the first range
  // tombstone with end key > `k`.
  void Seek(const Slice& target) override {
    assert(range_tombstone_iters_.empty() ||
           range_tombstone_iters_.size() == children_.size());
    SeekImpl(target);
    FindNextVisibleKey();

    direction_ = kForward;
    {
      PERF_TIMER_GUARD(seek_min_heap_time);
      current_ = CurrentForward();
    }
  }

  void SeekForPrev(const Slice& target) override {
    assert(range_tombstone_iters_.empty() ||
           range_tombstone_iters_.size() == children_.size());
    SeekForPrevImpl(target);
    FindPrevVisibleKey();

    direction_ = kReverse;
    {
      PERF_TIMER_GUARD(seek_max_heap_time);
      current_ = CurrentReverse();
    }
  }

  void Next() override {
    assert(Valid());
    // Ensure that all children are positioned after key().
    // If we are moving in the forward direction, it is already
    // true for all of the non-current children since current_ is
    // the smallest child and key() == current_->key().
    if (UNLIKELY(direction_ != kForward)) {
      // The loop advanced all non-current children to be > key() so current_
      // should still be strictly the smallest key.
      SwitchToForward();
    }

    // For the heap modifications below to be correct, current_ must be the
    // current top of the heap.
    assert(current_ == CurrentForward());
    // as the current points to the current record. move the iterator forward.
    current_->Next();
    if (LIKELY(current_->Valid())) {
      // current is still valid after the Next() call above.  Call
      // replace_top() to restore the heap property.  When the same child
      // iterator yields a sequence of keys, this is cheap.
      assert(current_->status().ok());
      UpdatePrefixCache(minHeap_.top());
      minHeap_.update_top();
    } else {
      // current stopped being valid, remove it from the heap.
      considerStatus(current_->status());
      minHeap_.pop();
    }
    FindNextVisibleKey();
    current_ = CurrentForward();
  }

  bool NextAndGetResult(IterateResult* result) override {
    Next();
    bool is_valid = Valid();
    if (is_valid) {
      result->SetKey(this->key());
      result->bound_check_result = UpperBoundCheckResult();
      result->value_prepared = current_->IsValuePrepared();
    }
    return is_valid;
  }

  void Prev() override {
    assert(Valid());
    // Ensure that all children are positioned before key().
    // If we are moving in the reverse direction, it is already
    // true for all of the non-current children since current_ is
    // the largest child and key() == current_->key().
    if (UNLIKELY(direction_ != kReverse)) {
      // Otherwise, retreat the non-current children.  We retreat current_
      // just after the if-block.
      SwitchToBackward();
    }

    // For the heap modifications below to be correct, current_ must be the
    // current top of the heap.
    assert(current_ == CurrentReverse());
    current_->Prev();
    if (LIKELY(current_->Valid())) {
      // current is still valid after the Prev() call above.  Call
      // replace_top() to restore the heap property.  When the same child
      // iterator yields a sequence of keys, this is cheap.
      assert(current_->status().ok());
      UpdatePrefixCache(maxHeap_->top());
      maxHeap_->replace_top(maxHeap_->top());
    } else {
      // current stopped being valid, remove it from the heap.
      considerStatus(current_->status());
      maxHeap_->pop();
    }
    FindPrevVisibleKey();
    current_ = CurrentReverse();
  }

  Slice key() const override {
    assert(Valid());
    return current_->key();
  }

  Slice value() const override {
    assert(Valid());
    return current_->value();
  }

  bool PrepareValue() override {
    assert(Valid());
    if (current_->PrepareValue()) {
      return true;
    }

    considerStatus(current_->status());
    assert(!status_.ok());
    return false;
  }

  // Here we simply relay MayBeOutOfLowerBound/MayBeOutOfUpperBound result
  // from current child iterator. Potentially as long as one of child iterator
  // report out of bound is not possible, we know current key is within bound.

  bool MayBeOutOfLowerBound() override {
    assert(Valid());
    return current_->MayBeOutOfLowerBound();
  }

  IterBoundCheck UpperBoundCheckResult() override {
    assert(Valid());
    return current_->UpperBoundCheckResult();
  }

  void SetPinnedItersMgr(PinnedIteratorsManager* pinned_iters_mgr) override {
    pinned_iters_mgr_ = pinned_iters_mgr;
    for (auto& child : children_) {
      child.iter.SetPinnedItersMgr(pinned_iters_mgr);
    }
  }

  bool IsKeyPinned() const override {
    assert(Valid());
    return pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled() &&
           current_->IsKeyPinned();
  }

  bool IsValuePinned() const override {
    assert(Valid());
    return pinned_iters_mgr_ && pinned_iters_mgr_->PinningEnabled() &&
           current_->IsValuePinned();
  }

 private:
  friend class MergeIteratorBuilder;
  // Clears heaps for both directions, used when changing direction or seeking
  void ClearHeaps(bool clear_active = true);
  // Ensures that maxHeap_ is initialized when starting to go in the reverse
  // direction
  void InitMaxHeap();

  // Advance this merging iterator until the current key (top of min heap) is
  // not covered by any range tombstone or that there is no more keys (heap is
  // empty). After this call, if Valid(), current_ points to the next key that
  // is not covered by any range tombstone.
  void FindNextVisibleKey();
  void FindPrevVisibleKey();

  void FindNextVisibleKeySlowPath();
  void FindPrevVisibleKeySlowPath();

  void SeekImpl(const Slice& target, size_t starting_level = 0,
                bool range_tombstone_reseek = false);

  // Seek to fist key <= target key (internal key) for
  // children_[starting_level:].
  void SeekForPrevImpl(const Slice& target, size_t starting_level = 0,
                       bool range_tombstone_reseek = false);

  bool is_arena_mode_;
  bool prefix_seek_mode_;
  // Which direction is the iterator moving?
  enum Direction : uint8_t { kForward, kReverse };
  Direction direction_;
  const InternalKeyComparator* comparator_;
  // HeapItem for range tombstone start and end keys. Each range tombstone
  // iterator will have at most one side (start key or end key) in a heap
  // at the same time, so this vector will be of size children_.size();
  // pinned_heap_item_[i] corresponds to the start key and end key HeapItem
  // for range_tombstone_iters_[i].
  std::vector<HeapItem> pinned_heap_item_;

  // Levels (indices into range_tombstone_iters_/children_ ) that currently have
  // "active" range tombstones. See comments above Seek() for meaning of
  // "active".
  std::set<size_t> active_;

  bool SkipNextDeleted();
  bool SkipPrevDeleted();

  // Cached pointer to child iterator with the current key, or nullptr if no
  // child iterators are valid.  This is the top of minHeap_ or maxHeap_
  // depending on the direction.
  IteratorWrapper* current_;
  // If any of the children have non-ok status, this is one of them.
  Status status_;
  union {
    MergerMinIterHeap minHeap_;
    MergerMaxIterHeap maxHeap_;
  };

  PinnedIteratorsManager* pinned_iters_mgr_;

  // Used to bound range tombstones. For point keys, DBIter and SSTable iterator
  // take care of boundary checking.
  const Slice* iterate_upper_bound_;

  // In forward direction, process a child that is not in the min heap.
  // If valid, add to the min heap. Otherwise, check status.
  void AddToMinHeapOrCheckStatus(HeapItem*);

  // In backward direction, process a child that is not in the max heap.
  // If valid, add to the min heap. Otherwise, check status.
  void AddToMaxHeapOrCheckStatus(HeapItem*);

  void SwitchToForward();

  // Switch the direction from forward to backward without changing the
  // position. Iterator should still be valid.
  void SwitchToBackward();

  IteratorWrapper* CurrentForward() const {
    assert(direction_ == kForward);
    assert(minHeap_.empty() || minHeap_.top()->type == HeapItem::ITERATOR);
    return !minHeap_.empty() ? &minHeap_.top()->iter : nullptr;
  }

  IteratorWrapper* CurrentReverse() const {
    assert(direction_ == kReverse);
    assert(maxHeap_->empty() || maxHeap_->top()->type == HeapItem::ITERATOR);
    return !maxHeap_->empty() ? &maxHeap_->top()->iter : nullptr;
  }
};

#define MergingIterMethod(Ret) \
  template <class MinHeapComparator, class MaxHeapComparator, class Item> \
  Ret MergingIterTmpl<MinHeapComparator, MaxHeapComparator, Item>::

// Seek to fist key >= target key (internal key) for children_[starting_level:].
// Cascading seek optimizations are applied if range tombstones are present (see
// comment above Seek() for more).
//
// @param range_tombstone_reseek Whether target is some range tombstone
// end, i.e., whether this SeekImpl() call is a part of a "cascading seek". This
// is used only for recoding relevant perf_context.
MergingIterMethod(void)SeekImpl(const Slice& target, size_t starting_level,
                               bool range_tombstone_reseek) {
  // active range tombstones before `starting_level` remain active
  ClearHeaps(false /* clear_active */);
  ParsedInternalKey pik;
  if (!range_tombstone_iters_.empty()) {
    // pik is only used in InsertRangeTombstoneToMinHeap().
    ParseInternalKey(target, &pik, false).PermitUncheckedError();
  }

  // TODO: perhaps we could save some upheap cost by add all child iters first
  //  and then do a single heapify.
  for (size_t level = 0; level < starting_level; ++level) {
    PERF_TIMER_GUARD(seek_min_heap_time);
    AddToMinHeapOrCheckStatus(&children_[level]);
  }
  if (!range_tombstone_iters_.empty()) {
    // Add range tombstones from levels < starting_level. We can insert from
    // pinned_heap_item_ for the following reasons:
    // - pinned_heap_item_[level] is in minHeap_ iff
    // range_tombstone_iters[level]->Valid().
    // - If `level` is in active_, then range_tombstone_iters_[level]->Valid()
    // and pinned_heap_item_[level] is of type RANGE_DELETION_END.
    for (size_t level = 0; level < starting_level; ++level) {
      if (range_tombstone_iters_[level] &&
          range_tombstone_iters_[level]->Valid()) {
        // use an iterator on active_ if performance becomes an issue here
        if (active_.count(level) > 0) {
          assert(pinned_heap_item_[level].type == HeapItem::DELETE_RANGE_END);
          // if it was active, then start key must be within upper_bound,
          // so we can add to minHeap_ directly.
          minHeap_.push(&pinned_heap_item_[level]);
        } else {
          // this takes care of checking iterate_upper_bound, but with an extra
          // key comparison if range_tombstone_iters_[level] was already out of
          // bound. Consider using a new HeapItem type or some flag to remember
          // boundary checking result.
          InsertRangeTombstoneToMinHeap(level);
        }
      } else {
        assert(!active_.count(level));
      }
    }
    // levels >= starting_level will be reseeked below, so clearing their active
    // state here.
    active_.erase(active_.lower_bound(starting_level), active_.end());
  }

  status_ = Status::OK();
  IterKey current_search_key;
  current_search_key.SetInternalKey(target, false /* copy */);
  // Seek target might change to some range tombstone end key, so
  // we need to remember them for async requests.
  // (level, target) pairs
  autovector<std::pair<size_t, std::string>> prefetched_target;
  for (auto level = starting_level; level < children_.size(); ++level) {
    {
      PERF_TIMER_GUARD(seek_child_seek_time);
      children_[level].iter.Seek(current_search_key.GetInternalKey());
    }

    PERF_COUNTER_ADD(seek_child_seek_count, 1);

    if (!range_tombstone_iters_.empty()) {
      if (range_tombstone_reseek) {
        // This seek is to some range tombstone end key.
        // Should only happen when there are range tombstones.
        PERF_COUNTER_ADD(internal_range_del_reseek_count, 1);
      }
      if (children_[level].iter.status().IsTryAgain()) {
        prefetched_target.emplace_back(
            level, current_search_key.GetInternalKey().ToString());
      }
      auto range_tombstone_iter = range_tombstone_iters_[level];
      if (range_tombstone_iter) {
        range_tombstone_iter->Seek(current_search_key.GetUserKey());
        if (range_tombstone_iter->Valid()) {
          // insert the range tombstone end that is closer to and >=
          // current_search_key. Strictly speaking, since the Seek() call above
          // is on user key, it is possible that range_tombstone_iter->end_key()
          // < current_search_key. This can happen when range_tombstone_iter is
          // truncated and range_tombstone_iter.largest_ has the same user key
          // as current_search_key.GetUserKey() but with a larger sequence
          // number than current_search_key. Correctness is not affected as this
          // tombstone end key will be popped during FindNextVisibleKey().
          InsertRangeTombstoneToMinHeap(
              level, comparator_->Compare(range_tombstone_iter->start_key(),
                                          pik) > 0 /* start_key */);
          // current_search_key < end_key guaranteed by the Seek() and Valid()
          // calls above. Only interested in user key coverage since older
          // sorted runs must have smaller sequence numbers than this range
          // tombstone.
          //
          // TODO: range_tombstone_iter->Seek() finds the max covering
          //  sequence number, can make it cheaper by not looking for max.
          if (comparator_->user_comparator()->Compare(
                  range_tombstone_iter->start_key().user_key,
                  current_search_key.GetUserKey()) <= 0) {
            // Since range_tombstone_iter->Valid(), seqno should be valid, so
            // there is no need to check it.
            range_tombstone_reseek = true;
            // Current target user key is covered by this range tombstone.
            // All older sorted runs will seek to range tombstone end key.
            // Note that for prefix seek case, it is possible that the prefix
            // is not the same as the original target, it should not affect
            // correctness. Besides, in most cases, range tombstone start and
            // end key should have the same prefix?
            // If range_tombstone_iter->end_key() is truncated to its largest_
            // boundary, the timestamp in user_key will not be max timestamp,
            // but the timestamp of `range_tombstone_iter.largest_`. This should
            // be fine here as current_search_key is used to Seek into lower
            // levels.
            current_search_key.SetInternalKey(
                range_tombstone_iter->end_key().user_key, kMaxSequenceNumber);
          }
        }
      }
    }
    // child.iter.status() is set to Status::TryAgain indicating asynchronous
    // request for retrieval of data blocks has been submitted. So it should
    // return at this point and Seek should be called again to retrieve the
    // requested block and add the child to min heap.
    if (children_[level].iter.status().IsTryAgain()) {
      continue;
    }
    {
      // Strictly, we timed slightly more than min heap operation,
      // but these operations are very cheap.
      PERF_TIMER_GUARD(seek_min_heap_time);
      AddToMinHeapOrCheckStatus(&children_[level]);
    }
  }

  if (range_tombstone_iters_.empty()) {
    for (auto& child : children_) {
      if (child.iter.status().IsTryAgain()) {
        child.iter.Seek(target);
        {
          PERF_TIMER_GUARD(seek_min_heap_time);
          AddToMinHeapOrCheckStatus(&child);
        }
        PERF_COUNTER_ADD(number_async_seek, 1);
      }
    }
  } else {
    for (auto& prefetch : prefetched_target) {
      // (level, target) pairs
      children_[prefetch.first].iter.Seek(prefetch.second);
      {
        PERF_TIMER_GUARD(seek_min_heap_time);
        AddToMinHeapOrCheckStatus(&children_[prefetch.first]);
      }
      PERF_COUNTER_ADD(number_async_seek, 1);
    }
  }
}

// Returns true iff the current key (min heap top) should not be returned
// to user (of the merging iterator). This can be because the current key
// is deleted by some range tombstone, the current key is some fake file
// boundary sentinel key, or the current key is an end point of a range
// tombstone. Advance the iterator at heap top if needed. Heap order is restored
// and `active_` is updated accordingly.
// See FindNextVisibleKey() for more detail on internal implementation
// of advancing child iters.
//
// REQUIRES:
// - min heap is currently not empty, and iter is in kForward direction.
// - minHeap_ top is not DELETE_RANGE_START (so that `active_` is current).
MergingIterMethod(bool)SkipNextDeleted() {
  // 3 types of keys:
  // - point key
  // - file boundary sentinel keys
  // - range deletion end key
  auto current = minHeap_.top();
  if (current->type == HeapItem::DELETE_RANGE_END) {
    active_.erase(current->level);
    assert(range_tombstone_iters_[current->level] &&
           range_tombstone_iters_[current->level]->Valid());
    range_tombstone_iters_[current->level]->Next();
    if (range_tombstone_iters_[current->level]->Valid()) {
      InsertRangeTombstoneToMinHeap(current->level, true /* start_key */,
                                    true /* replace_top */);
    } else {
      minHeap_.pop();
    }
    return true /* current key deleted */;
  }
  if (current->iter.IsDeleteRangeSentinelKey()) {
    // If the file boundary is defined by a range deletion, the range
    // tombstone's end key must come before this sentinel key (see op_type in
    // SetTombstoneKey()).
    assert(ExtractValueType(current->iter.key()) != kTypeRangeDeletion ||
           active_.count(current->level) == 0);
    // When entering a new file, old range tombstone iter is freed,
    // but the last key from that range tombstone iter may still be in the heap.
    // We need to ensure the data underlying its corresponding key Slice is
    // still alive. We do so by popping the range tombstone key from heap before
    // calling iter->Next(). Technically, this change is not needed: if there is
    // a range tombstone end key that is after file boundary sentinel key in
    // minHeap_, the range tombstone end key must have been truncated at file
    // boundary. The underlying data of the range tombstone end key Slice is the
    // SST file's largest internal key stored as file metadata in Version.
    // However, since there are too many implicit assumptions made, it is safer
    // to just ensure range tombstone iter is still alive.
    minHeap_.pop();
    // Remove last SST file's range tombstone end key if there is one.
    // This means file boundary is before range tombstone end key,
    // which could happen when a range tombstone and a user key
    // straddle two SST files. Note that in TruncatedRangeDelIterator
    // constructor, parsed_largest.sequence is decremented 1 in this case.
    if (!minHeap_.empty() && minHeap_.top()->level == current->level &&
        minHeap_.top()->type == HeapItem::DELETE_RANGE_END) {
      minHeap_.pop();
      active_.erase(current->level);
    }
    // LevelIterator enters a new SST file
    current->iter.Next();
    if (current->iter.Valid()) {
      assert(current->iter.status().ok());
      UpdatePrefixCache(current);
      minHeap_.push(current);
    }
    if (range_tombstone_iters_[current->level] &&
        range_tombstone_iters_[current->level]->Valid()) {
      InsertRangeTombstoneToMinHeap(current->level);
    }
    return true /* current key deleted */;
  }
  assert(current->type == HeapItem::ITERATOR);
  // Point key case: check active_ for range tombstone coverage.
  ParsedInternalKey pik(current->iter.key());
  if (!active_.empty()) {
    auto i = *active_.begin();
    if (i < current->level) {
      // range tombstone is from a newer level, definitely covers
      assert(comparator_->Compare(range_tombstone_iters_[i]->start_key(),
                                  pik) <= 0);
      assert(comparator_->Compare(pik, range_tombstone_iters_[i]->end_key()) <
             0);
      std::string target;
      AppendInternalKey(&target, range_tombstone_iters_[i]->end_key());
      SeekImpl(target, current->level, true);
      return true /* current key deleted */;
    } else if (i == current->level) {
      // range tombstone is from the same level as current, check sequence
      // number. By `active_` we know current key is between start key and end
      // key.
      assert(comparator_->Compare(range_tombstone_iters_[i]->start_key(),
                                  pik) <= 0);
      assert(comparator_->Compare(pik, range_tombstone_iters_[i]->end_key()) <
             0);
      if (pik.sequence < range_tombstone_iters_[current->level]->seq()) {
        // covered by range tombstone
        current->iter.Next();
        if (current->iter.Valid()) {
          UpdatePrefixCache(current);
          minHeap_.replace_top(current);
        } else {
          minHeap_.pop();
        }
        return true /* current key deleted */;
      } else {
        return false /* current key not deleted */;
      }
    } else {
      return false /* current key not deleted */;
      // range tombstone from an older sorted run with current key < end key.
      // current key is not deleted and the older sorted run will have its range
      // tombstone updated when the range tombstone's end key are popped from
      // minHeap_.
    }
  }
  // we can reach here only if active_ is empty
  assert(active_.empty());
  assert(minHeap_.top()->type == HeapItem::ITERATOR);
  return false /* current key not deleted */;
}

MergingIterMethod(void)SeekForPrevImpl(const Slice& target,
                                      size_t starting_level,
                                      bool range_tombstone_reseek) {
  // active range tombstones before `starting_level` remain active
  ClearHeaps(false /* clear_active */);
  InitMaxHeap();
  ParsedInternalKey pik;
  if (!range_tombstone_iters_.empty()) {
    ParseInternalKey(target, &pik, false).PermitUncheckedError();
  }
  for (size_t level = 0; level < starting_level; ++level) {
    PERF_TIMER_GUARD(seek_max_heap_time);
    AddToMaxHeapOrCheckStatus(&children_[level]);
  }
  if (!range_tombstone_iters_.empty()) {
    // Add range tombstones before starting_level.
    for (size_t level = 0; level < starting_level; ++level) {
      if (range_tombstone_iters_[level] &&
          range_tombstone_iters_[level]->Valid()) {
        assert(static_cast<bool>(active_.count(level)) ==
               (pinned_heap_item_[level].type == HeapItem::DELETE_RANGE_START));
        maxHeap_->push(&pinned_heap_item_[level]);
      } else {
        assert(!active_.count(level));
      }
    }
    // levels >= starting_level will be reseeked below,
    active_.erase(active_.lower_bound(starting_level), active_.end());
  }

  status_ = Status::OK();
  IterKey current_search_key;
  current_search_key.SetInternalKey(target, false /* copy */);
  // Seek target might change to some range tombstone end key, so
  // we need to remember them for async requests.
  // (level, target) pairs
  autovector<std::pair<size_t, std::string>> prefetched_target;
  for (auto level = starting_level; level < children_.size(); ++level) {
    {
      PERF_TIMER_GUARD(seek_child_seek_time);
      children_[level].iter.SeekForPrev(current_search_key.GetInternalKey());
    }

    PERF_COUNTER_ADD(seek_child_seek_count, 1);

    if (!range_tombstone_iters_.empty()) {
      if (range_tombstone_reseek) {
        // This seek is to some range tombstone end key.
        // Should only happen when there are range tombstones.
        PERF_COUNTER_ADD(internal_range_del_reseek_count, 1);
      }
      if (children_[level].iter.status().IsTryAgain()) {
        prefetched_target.emplace_back(
            level, current_search_key.GetInternalKey().ToString());
      }
      auto range_tombstone_iter = range_tombstone_iters_[level];
      if (range_tombstone_iter) {
        range_tombstone_iter->SeekForPrev(current_search_key.GetUserKey());
        if (range_tombstone_iter->Valid()) {
          InsertRangeTombstoneToMaxHeap(
              level, comparator_->Compare(range_tombstone_iter->end_key(),
                                          pik) <= 0 /* end_key */);
          // start key <= current_search_key guaranteed by the Seek() call above
          // Only interested in user key coverage since older sorted runs must
          // have smaller sequence numbers than this tombstone.
          if (comparator_->user_comparator()->Compare(
                  current_search_key.GetUserKey(),
                  range_tombstone_iter->end_key().user_key) < 0) {
            range_tombstone_reseek = true;
            current_search_key.SetInternalKey(
                range_tombstone_iter->start_key().user_key, kMaxSequenceNumber,
                kValueTypeForSeekForPrev);
          }
        }
      }
    }
    // child.iter.status() is set to Status::TryAgain indicating asynchronous
    // request for retrieval of data blocks has been submitted. So it should
    // return at this point and Seek should be called again to retrieve the
    // requested block and add the child to min heap.
    if (children_[level].iter.status().IsTryAgain()) {
      continue;
    }
    {
      // Strictly, we timed slightly more than min heap operation,
      // but these operations are very cheap.
      PERF_TIMER_GUARD(seek_max_heap_time);
      AddToMaxHeapOrCheckStatus(&children_[level]);
    }
  }

  if (range_tombstone_iters_.empty()) {
    for (auto& child : children_) {
      if (child.iter.status().IsTryAgain()) {
        child.iter.SeekForPrev(target);
        {
          PERF_TIMER_GUARD(seek_min_heap_time);
          AddToMaxHeapOrCheckStatus(&child);
        }
        PERF_COUNTER_ADD(number_async_seek, 1);
      }
    }
  } else {
    for (auto& prefetch : prefetched_target) {
      // (level, target) pairs
      children_[prefetch.first].iter.SeekForPrev(prefetch.second);
      {
        PERF_TIMER_GUARD(seek_max_heap_time);
        AddToMaxHeapOrCheckStatus(&children_[prefetch.first]);
      }
      PERF_COUNTER_ADD(number_async_seek, 1);
    }
  }
}

// See more in comments above SkipNextDeleted().
// REQUIRES:
// - max heap is currently not empty, and iter is in kReverse direction.
// - maxHeap_ top is not DELETE_RANGE_END (so that `active_` is current).
MergingIterMethod(bool)SkipPrevDeleted() {
  // 3 types of keys:
  // - point key
  // - file boundary sentinel keys
  // - range deletion start key
  auto current = maxHeap_->top();
  if (current->type == HeapItem::DELETE_RANGE_START) {
    active_.erase(current->level);
    assert(range_tombstone_iters_[current->level] &&
           range_tombstone_iters_[current->level]->Valid());
    range_tombstone_iters_[current->level]->Prev();
    if (range_tombstone_iters_[current->level]->Valid()) {
      InsertRangeTombstoneToMaxHeap(current->level, true /* end_key */,
                                    true /* replace_top */);
    } else {
      maxHeap_->pop();
    }
    return true /* current key deleted */;
  }
  if (current->iter.IsDeleteRangeSentinelKey()) {
    // LevelIterator enters a new SST file
    maxHeap_->pop();
    // Remove last SST file's range tombstone key if there is one.
    if (!maxHeap_->empty() && maxHeap_->top()->level == current->level &&
        maxHeap_->top()->type == HeapItem::DELETE_RANGE_START) {
      maxHeap_->pop();
      active_.erase(current->level);
    }
    current->iter.Prev();
    if (current->iter.Valid()) {
      assert(current->iter.status().ok());
      UpdatePrefixCache(current);
      maxHeap_->push(current);
    }

    if (range_tombstone_iters_[current->level] &&
        range_tombstone_iters_[current->level]->Valid()) {
      InsertRangeTombstoneToMaxHeap(current->level);
    }
    return true /* current key deleted */;
  }
  assert(current->type == HeapItem::ITERATOR);
  // Point key case: check active_ for range tombstone coverage.
  ParsedInternalKey pik(current->iter.key());
  if (!active_.empty()) {
    auto i = *active_.begin();
    if (i < current->level) {
      // range tombstone is from a newer level, definitely covers
      assert(comparator_->Compare(range_tombstone_iters_[i]->start_key(),
                                  pik) <= 0);
      assert(comparator_->Compare(pik, range_tombstone_iters_[i]->end_key()) <
             0);
      std::string target;
      AppendInternalKey(&target, range_tombstone_iters_[i]->start_key());
      // This is different from SkipNextDeleted() which does reseek at sorted
      // runs >= level (instead of i+1 here). With min heap, if level L is at
      // top of the heap, then levels <L all have internal keys > level L's
      // current internal key, which means levels <L are already at a different
      // user key. With max heap, if level L is at top of the heap, then levels
      // <L all have internal keys smaller than level L's current internal key,
      // which might still be the same user key.
      SeekForPrevImpl(target, i + 1, true);
      return true /* current key deleted */;
    } else if (i == current->level) {
      // By `active_` we know current key is between start key and end key.
      assert(comparator_->Compare(range_tombstone_iters_[i]->start_key(),
                                  pik) <= 0);
      assert(comparator_->Compare(pik, range_tombstone_iters_[i]->end_key()) <
             0);
      if (pik.sequence < range_tombstone_iters_[current->level]->seq()) {
        current->iter.Prev();
        if (current->iter.Valid()) {
          UpdatePrefixCache(current);
          maxHeap_->replace_top(current);
        } else {
          maxHeap_->pop();
        }
        return true /* current key deleted */;
      } else {
        return false /* current key not deleted */;
      }
    } else {
      return false /* current key not deleted */;
    }
  }

  assert(active_.empty());
  assert(maxHeap_->top()->type == HeapItem::ITERATOR);
  return false /* current key not deleted */;
}

MergingIterMethod(void)AddToMinHeapOrCheckStatus(HeapItem* child) {
  if (child->iter.Valid()) {
    assert(child->iter.status().ok());
    minHeap_.push(child);
  } else {
    considerStatus(child->iter.status());
  }
}

MergingIterMethod(void)AddToMaxHeapOrCheckStatus(HeapItem* child) {
  if (child->iter.Valid()) {
    assert(child->iter.status().ok());
    maxHeap_->push(child);
  } else {
    considerStatus(child->iter.status());
  }
}

// Advance all non current_ child to > current_.key().
// We advance current_ after the this function call as it does not require
// Seek().
// Advance all range tombstones iters, including the one corresponding to
// current_, to the first tombstone with end_key > current_.key().
// TODO: potentially do cascading seek here too
MergingIterMethod(void)SwitchToForward() {
  ClearHeaps();
  Slice target = key();
  for (auto& child : children_) {
    if (&child.iter != current_) {
      child.iter.Seek(target);
      // child.iter.status() is set to Status::TryAgain indicating asynchronous
      // request for retrieval of data blocks has been submitted. So it should
      // return at this point and Seek should be called again to retrieve the
      // requested block and add the child to min heap.
      if (child.iter.status() == Status::TryAgain()) {
        continue;
      }
      if (child.iter.Valid() && comparator_->Equal(target, child.iter.key())) {
        assert(child.iter.status().ok());
        child.iter.Next();
      }
    }
    AddToMinHeapOrCheckStatus(&child);
  }

  for (auto& child : children_) {
    if (child.iter.status() == Status::TryAgain()) {
      child.iter.Seek(target);
      if (child.iter.Valid() && comparator_->Equal(target, child.iter.key())) {
        assert(child.iter.status().ok());
        child.iter.Next();
      }
      AddToMinHeapOrCheckStatus(&child);
    }
  }

  // Current range tombstone iter also needs to seek for the following case:
  // Previous direction is backward, so range tombstone iter may point to a
  // tombstone before current_. If there is no such tombstone, then the range
  // tombstone iter is !Valid(). Need to reseek here to make it valid again.
  if (!range_tombstone_iters_.empty()) {
    ParsedInternalKey pik(target);
    for (size_t i = 0; i < range_tombstone_iters_.size(); ++i) {
      auto iter = range_tombstone_iters_[i];
      if (iter) {
        iter->Seek(pik.user_key);
        // The while loop is needed as the Seek() call above is only for user
        // key. We could have a range tombstone with end_key covering user_key,
        // but still is smaller than target. This happens when the range
        // tombstone is truncated at iter.largest_.
        while (iter->Valid() &&
               comparator_->Compare(iter->end_key(), pik) <= 0) {
          iter->Next();
        }
        if (range_tombstone_iters_[i]->Valid()) {
          InsertRangeTombstoneToMinHeap(
              i, comparator_->Compare(range_tombstone_iters_[i]->start_key(),
                                      pik) > 0 /* start_key */);
        }
      }
    }
  }

  direction_ = kForward;
  assert(current_ == CurrentForward());
}

// Advance all range tombstones iters, including the one corresponding to
// current_, to the first tombstone with start_key <= current_.key().
MergingIterMethod(void)SwitchToBackward() {
  ClearHeaps();
  InitMaxHeap();
  Slice target = key();
  for (auto& child : children_) {
    if (&child.iter != current_) {
      child.iter.SeekForPrev(target);
      TEST_SYNC_POINT_CALLBACK("MergeIterator::Prev:BeforePrev", &child);
      if (child.iter.Valid() && comparator_->Equal(target, child.iter.key())) {
        assert(child.iter.status().ok());
        child.iter.Prev();
      }
    }
    AddToMaxHeapOrCheckStatus(&child);
  }

  ParsedInternalKey pik(target);
  for (size_t i = 0; i < range_tombstone_iters_.size(); ++i) {
    auto iter = range_tombstone_iters_[i];
    if (iter) {
      iter->SeekForPrev(pik.user_key);
      // Since the SeekForPrev() call above is only for user key,
      // we may end up with some range tombstone with start key having the
      // same user key at current_, but with a smaller sequence number. This
      // makes current_ not at maxHeap_ top for the CurrentReverse() call
      // below. If there is a range tombstone start key with the same user
      // key and the same sequence number as current_.key(), it will be fine as
      // in InsertRangeTombstoneToMaxHeap() we change op_type to be the smallest
      // op_type.
      while (iter->Valid() &&
             comparator_->Compare(iter->start_key(), pik) > 0) {
        iter->Prev();
      }
      if (iter->Valid()) {
        InsertRangeTombstoneToMaxHeap(
            i, comparator_->Compare(range_tombstone_iters_[i]->end_key(),
                                    pik) <= 0 /* end_key */);
      }
    }
  }

  direction_ = kReverse;
  if (!prefix_seek_mode_) {
    // Note that we don't do assert(current_ == CurrentReverse()) here
    // because it is possible to have some keys larger than the seek-key
    // inserted between Seek() and SeekToLast(), which makes current_ not
    // equal to CurrentReverse().
    current_ = CurrentReverse();
  }
  assert(current_ == CurrentReverse());
}

MergingIterMethod(void)ClearHeaps(bool clear_active) {
  minHeap_.clear();
  if (clear_active) {
    active_.clear();
  }
}

MergingIterMethod(inline void)InitMaxHeap() {
  maxHeap_.clear();
}

// Repeatedly check and remove heap top key if it is not a point key
// that is not covered by range tombstones. SeekImpl() is called to seek to end
// of a range tombstone if the heap top is a point key covered by some range
// tombstone from a newer sorted run. If the covering tombstone is from current
// key's level, then the current child iterator is simply advanced to its next
// key without reseeking.
MergingIterMethod(inline void)FindNextVisibleKey() {
  if (LIKELY(range_tombstone_iters_.empty())) {
    return;
  }
  FindNextVisibleKeySlowPath();
}
MergingIterMethod(void)FindNextVisibleKeySlowPath() {
  // When active_ is empty, we know heap top cannot be a range tombstone end
  // key. It cannot be a range tombstone start key per PopDeleteRangeStart().
  PopDeleteRangeStart();
  while (!minHeap_.empty() &&
         (!active_.empty() || minHeap_.top()->IsDeleteRangeSentinelKey()) &&
         SkipNextDeleted()) {
    PopDeleteRangeStart();
  }
}

MergingIterMethod(inline void)FindPrevVisibleKey() {
  if (LIKELY(range_tombstone_iters_.empty())) {
    return;
  }
  FindPrevVisibleKeySlowPath();
}
MergingIterMethod(void)FindPrevVisibleKeySlowPath() {
  PopDeleteRangeEnd();
  while (!maxHeap_->empty() &&
         (!active_.empty() || maxHeap_->top()->IsDeleteRangeSentinelKey()) &&
         SkipPrevDeleted()) {
    PopDeleteRangeEnd();
  }
}

template<class MinHeapComparator, class MaxHeapComparator, class Item = HeapItemAndPrefix>
static MergingIterator* NewIterTpl(const InternalKeyComparator* cmp,
                                   InternalIterator** list, int n,
                                   Arena* arena, bool prefix_seek_mode,
                                   const Slice* upper_bound) {
  using Iter = MergingIterTmpl<MinHeapComparator, MaxHeapComparator, Item>;
  if (arena == nullptr) {
    return new Iter(cmp, list, n, false, prefix_seek_mode, upper_bound);
  } else {
    auto mem = arena->AllocateAligned(sizeof(Iter));
    return new (mem) Iter(cmp, list, n, true, prefix_seek_mode, upper_bound);
  }
}

static MergingIterator* NewIter(const InternalKeyComparator* cmp,
                                InternalIterator** list, int n,
                                Arena* arena, bool prefix_seek_mode,
                                const Slice* upper_bound) {
  if (cmp->IsForwardBytewise()) {
    return NewIterTpl<MinHeapBytewiseComp, MaxHeapBytewiseComp>
        (cmp, list, n, arena, prefix_seek_mode, upper_bound);
  } else if (cmp->IsReverseBytewise()) {
    return NewIterTpl<MinHeapRevBytewiseComp, MaxHeapRevBytewiseComp>
        (cmp, list, n, arena, prefix_seek_mode, upper_bound);
  } else {
    return NewIterTpl<MinHeapItemComparator, MaxHeapItemComparator, HeapItem*>
        (cmp, list, n, arena, prefix_seek_mode, upper_bound);
  }
}

InternalIterator* NewMergingIterator(const InternalKeyComparator* cmp,
                                     InternalIterator** list, int n,
                                     Arena* arena, bool prefix_seek_mode) {
  assert(n >= 0);
  if (n == 0) {
    return NewEmptyInternalIterator<Slice>(arena);
  } else if (n == 1) {
    return list[0];
  } else {
    return NewIter(cmp, list, n, arena, prefix_seek_mode, nullptr);
  }
}

MergeIteratorBuilder::MergeIteratorBuilder(
    const InternalKeyComparator* comparator, Arena* a, bool prefix_seek_mode,
    const Slice* iterate_upper_bound)
    : first_iter(nullptr), use_merging_iter(false), arena(a) {
  ROCKSDB_VERIFY(nullptr != arena);
  merge_iter = NewIter(comparator, nullptr, 0, a, prefix_seek_mode, iterate_upper_bound);
}

MergeIteratorBuilder::~MergeIteratorBuilder() {
  if (first_iter != nullptr) {
    first_iter->~InternalIterator();
  }
  if (merge_iter != nullptr) {
    merge_iter->~MergingIterator();
  }
}

void MergeIteratorBuilder::AddIterator(InternalIterator* iter) {
  if (!use_merging_iter && first_iter != nullptr) {
    merge_iter->AddIterator(first_iter);
    use_merging_iter = true;
    first_iter = nullptr;
  }
  if (use_merging_iter) {
    merge_iter->AddIterator(iter);
  } else {
    first_iter = iter;
  }
}

void MergeIteratorBuilder::AddPointAndTombstoneIterator(
    InternalIterator* point_iter, TruncatedRangeDelIterator* tombstone_iter,
    TruncatedRangeDelIterator*** tombstone_iter_ptr) {
  // tombstone_iter_ptr != nullptr means point_iter is a LevelIterator.
  bool add_range_tombstone = tombstone_iter ||
                             !merge_iter->range_tombstone_iters_.empty() ||
                             tombstone_iter_ptr;
  if (!use_merging_iter && (add_range_tombstone || first_iter)) {
    use_merging_iter = true;
    if (first_iter) {
      merge_iter->AddIterator(first_iter);
      first_iter = nullptr;
    }
  }
  if (use_merging_iter) {
    merge_iter->AddIterator(point_iter);
    if (add_range_tombstone) {
      // If there was a gap, fill in nullptr as empty range tombstone iterators.
      while (merge_iter->range_tombstone_iters_.size() <
             merge_iter->children_.size() - 1) {
        merge_iter->AddRangeTombstoneIterator(nullptr);
      }
      merge_iter->AddRangeTombstoneIterator(tombstone_iter);
    }

    if (tombstone_iter_ptr) {
      // This is needed instead of setting to &range_tombstone_iters_[i]
      // directly here since the memory address of range_tombstone_iters_[i]
      // might change during vector resizing.
      range_del_iter_ptrs_.emplace_back(
          merge_iter->range_tombstone_iters_.size() - 1, tombstone_iter_ptr);
    }
  } else {
    first_iter = point_iter;
  }
}

InternalIterator* MergeIteratorBuilder::Finish(ArenaWrappedDBIter* db_iter) {
  InternalIterator* ret = nullptr;
  if (!use_merging_iter) {
    ret = first_iter;
    first_iter = nullptr;
  } else {
    for (auto& p : range_del_iter_ptrs_) {
      *(p.second) = &(merge_iter->range_tombstone_iters_[p.first]);
    }
    if (db_iter && !merge_iter->range_tombstone_iters_.empty()) {
      // memtable is always the first level
      db_iter->SetMemtableRangetombstoneIter(
          &merge_iter->range_tombstone_iters_.front());
    }
    merge_iter->Finish();
    ret = merge_iter;
    merge_iter = nullptr;
  }
  return ret;
}

}  // namespace ROCKSDB_NAMESPACE
