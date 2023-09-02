// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_MEMTABLE_H_
#define STORAGE_LEVELDB_DB_MEMTABLE_H_

#include <string>

#include "db/dbformat.h"
#include "db/skiplist.h"
#include "leveldb/db.h"
#include "util/arena.h"

namespace leveldb {

class InternalKeyComparator;
class MemTableIterator;

class MemTable {
 public:
  // MemTables are reference counted.  The initial reference count
  // is zero and the caller must call Ref() at least once.
  // MemTable 自带引用计数，且初始引用数为 0，所以调用者必须至少调用一次 Ref
  // InternalKey 是由 UserKey（就是用户 set 的那个 key）+ SequenceNumber + ValueType 三部分组合而成的
  // LevelDB 通过维护一个全局的自增的 SequenceNumber 来实现快照机制，详情参考 dbformat.h 中的 SequenceNumber 类型
  // InternalKeyComparator 定义跳表中 key 的顺序，首先根据 user_key 升序排列然后根据 sequence 降序排列
  explicit MemTable(const InternalKeyComparator& comparator);

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Increase reference count.
  void Ref() { ++refs_; }

  // Drop reference count.  Delete if no more references exist.
  // 减少引用计数，引用计数归 0 后删除 Memtable
  void Unref() {
    --refs_;
    assert(refs_ >= 0);
    if (refs_ <= 0) {
      delete this;
    }
  }

  // Returns an estimate of the number of bytes of data in use by this
  // data structure. It is safe to call when MemTable is being modified.
  // 估算 MemTable 的内存使用
  // 在 MemTable 正在被修改时也可以安全调用(即具备并发安全性)
  size_t ApproximateMemoryUsage();

  // Return an iterator that yields the contents of the memtable.
  //
  // The caller must ensure that the underlying MemTable remains live
  // while the returned iterator is live.  The keys returned by this
  // iterator are internal keys encoded by AppendInternalKey in the
  // db/format.{h,cc} module.
  Iterator* NewIterator();

  // Add an entry into memtable that maps key to value at the
  // specified sequence number and with the specified type.
  // Typically value will be empty if type==kTypeDeletion.
  // 在 Memtable 中插入一个键值对, 这个键值对将被标为指定的 SequenceNumber
  // SequenceNumber 是一个全局的自增的序列号，用于实现快照机制，详情参考 dbformat.h 中的 SequenceNumber 类型
  void Add(SequenceNumber seq, ValueType type, const Slice& key,
           const Slice& value);

  // If memtable contains a value for key, store it in *value and return true.
  // If memtable contains a deletion for key, store a NotFound() error
  // in *status and return true.
  // Else, return false.
  // 在 MemTable 中查询某个 key, 如果能搞找到将其值存储在 *value 中并返回 true
  // 如果 MemTable 中有这个 key 被删除的记录则在 status 中写入 NotFound 并返回 true (MemTable 不直接删除元素，而是写入一个 type == kTypeDeletion 的键值对来标记删除)
  // 其它情况下返回 false
  bool Get(const LookupKey& key, std::string* value, Status* s);

 private:
  friend class MemTableIterator;
  friend class MemTableBackwardIterator;

  struct KeyComparator {
    const InternalKeyComparator comparator;
    explicit KeyComparator(const InternalKeyComparator& c) : comparator(c) {}
    int operator()(const char* a, const char* b) const;
  };

  typedef SkipList<const char*, KeyComparator> Table;

  // 只有引用计数归 0 才会析构，所以析构函数可以设为私有
  ~MemTable();  // Private since only Unref() should be used to delete it

  KeyComparator comparator_;
  int refs_;
  Arena arena_;
  Table table_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_MEMTABLE_H_
