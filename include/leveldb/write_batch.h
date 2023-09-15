// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch holds a collection of updates to apply atomically to a DB.
//
// The updates are applied in the order in which they are added
// to the WriteBatch.  For example, the value of "key" will be "v3"
// after the following batch is written:
//
//    batch.Put("key", "v1");
//    batch.Delete("key");
//    batch.Put("key", "v2");
//    batch.Put("key", "v3");
//
// Multiple threads can invoke const methods on a WriteBatch without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same WriteBatch must use
// external synchronization.
// 
// WriteBatch 是一组更新操作的集合，它们会被原子性的写入数据库。 
// 其中的操作是有序的，举例来说，下面的代码最终会得到 key -> v3 的结果
//
//    batch.Put("key", "v1");
//    batch.Delete("key");
//    batch.Put("key", "v2");
//    batch.Put("key", "v3");
//
// 多个线程可以不加锁并发调用 const 方法(只读方法)，如果有一个线程调用了非 const 方法，那么所有线程都需要加锁


#ifndef STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
#define STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_

#include <string>

#include "leveldb/export.h"
#include "leveldb/status.h"

namespace leveldb {

class Slice;

class LEVELDB_EXPORT WriteBatch {
 public:
  class LEVELDB_EXPORT Handler {
   public:
    virtual ~Handler();
    virtual void Put(const Slice& key, const Slice& value) = 0; // 写入键值对
    virtual void Delete(const Slice& key) = 0; // 删除键值对
  };

  WriteBatch();

  // WriteBatch 是可拷贝的
  // Intentionally copyable.
  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;

  ~WriteBatch();

  // 写入键值对
  // Store the mapping "key->value" in the database.
  void Put(const Slice& key, const Slice& value);

  // 删除键值对
  // If the database contains a mapping for "key", erase it.  Else do nothing.
  void Delete(const Slice& key);

  // 清空 WriteBatch
  // Clear all updates buffered in this batch.
  void Clear();

  // 估计应用 WriteBatch 后数据库的大小会增大多少
  // The size of the database changes caused by this batch.
  //
  // This number is tied to implementation details, and may change across
  // releases. It is intended for LevelDB usage metrics.
  size_t ApproximateSize() const;

  // 将 source 中的操作拷贝到当前 WriteBatch 中
  // Copies the operations in "source" to this batch.
  //
  // This runs in O(source size) time. However, the constant factor is better
  // than calling Iterate() over the source batch with a Handler that replicates
  // the operations into this batch.
  void Append(const WriteBatch& source);

  // 遍历 WriteBatch 并将其中的操作回调给 handler
  // Support for iterating over the contents of a batch.
  Status Iterate(Handler* handler) const;

 private:
  friend class WriteBatchInternal;

  std::string rep_;  // See comment in write_batch.cc for the format of rep_
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_WRITE_BATCH_H_
