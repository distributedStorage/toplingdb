// Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#pragma once

#include "rocksdb/rocksdb_namespace.h"

namespace ROCKSDB_NAMESPACE {

class Cleanable {
 public:
  Cleanable();
  // No copy constructor and copy assignment allowed.
  Cleanable(Cleanable&) = delete;
  Cleanable& operator=(Cleanable&) = delete;

  ~Cleanable();

  // Move constructor and move assignment is allowed.
  Cleanable(Cleanable&&);
  Cleanable& operator=(Cleanable&&);

  // Clients are allowed to register function/arg1/arg2 triples that
  // will be invoked when this iterator is destroyed.
  //
  // Note that unlike all of the preceding methods, this method is
  // not abstract and therefore clients should not override it.
  typedef void (*CleanupFunction)(void* arg1, void* arg2);
  void RegisterCleanup(CleanupFunction function, void* arg1, void* arg2);
  void DelegateCleanupsTo(Cleanable* other);
  // DoCleanup and also resets the pointers for reuse
  inline void Reset() {
    DoCleanup();
    cleanup_.function = nullptr;
    cleanup_.next = nullptr;
  }

 protected:
  struct Cleanup {
    CleanupFunction function;
    void* arg1;
    void* arg2;
    Cleanup* next;
  };
  Cleanup cleanup_;
  // It also becomes the owner of c
  void RegisterCleanup(Cleanup* c);

 private:
  // Performs all the cleanups. It does not reset the pointers. Making it
  // private
  // to prevent misuse
  inline void DoCleanup() {
    if (cleanup_.function != nullptr) {
      (*cleanup_.function)(cleanup_.arg1, cleanup_.arg2);
      for (Cleanup* c = cleanup_.next; c != nullptr;) {
        (*c->function)(c->arg1, c->arg2);
        Cleanup* next = c->next;
        delete c;
        c = next;
      }
    }
  }
};

bool IsCompactionWorker();

}  // namespace ROCKSDB_NAMESPACE
