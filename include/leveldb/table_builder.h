// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// TableBuilder provides the interface used to build a Table
// (an immutable and sorted map from keys to values).
//
// Multiple threads can invoke const methods on a TableBuilder without
// external synchronization, but if any of the threads may call a
// non-const method, all threads accessing the same TableBuilder must use
// external synchronization.
// TableBuilder 提供了构建 sstable 的接口
// 多个线程可以不加锁的调用 const 方法(只读方法)，但只要有一个现成调用了非 const 方法，那么所有线程都需要加锁

#ifndef STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_
#define STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_

#include <cstdint>

#include "leveldb/export.h"
#include "leveldb/options.h"
#include "leveldb/status.h"

namespace leveldb {

class BlockBuilder;
class BlockHandle;
class WritableFile;

class LEVELDB_EXPORT TableBuilder {
 public:
  // Create a builder that will store the contents of the table it is
  // building in *file.  Does not close the file.  It is up to the
  // caller to close the file after calling Finish().
  // 创建 TableBuilder 在 *file 上构建 sstable
  // TableBuilder 不会关闭 *file，需要调用方来关闭
  TableBuilder(const Options& options, WritableFile* file);

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // REQUIRES: Either Finish() or Abandon() has been called.
  // 调用了 Finish() 或 Abandon() 之后才能析构
  ~TableBuilder();

  // Change the options used by this builder.  Note: only some of the
  // option fields can be changed after construction.  If a field is
  // not allowed to change dynamically and its value in the structure
  // passed to the constructor is different from its value in the
  // structure passed to this method, this method will return an error
  // without changing any fields.
  // 修改 Builder 的部分参数
  // 只有部分参数可以在运行期修改，
  // 如果 options 参数中某些字段与 TableBuilder 中不同且无法动态修改, ChangeOptions 会返回错误并且不会修改任何值
  Status ChangeOptions(const Options& options);

  // Add key,value to the table being constructed.
  // REQUIRES: key is after any previously added key according to comparator.
  // REQUIRES: Finish(), Abandon() have not been called
  // 添加一个键值对
  // key 必须大于上一个 key 以保证有序
  // 在 Finish() 或 Abandon() 后不能再使用 Add
  void Add(const Slice& key, const Slice& value);

  // Advanced operation: flush any buffered key/value pairs to file.
  // Can be used to ensure that two adjacent entries never live in
  // the same data block.  Most clients should not need to use this method.
  // REQUIRES: Finish(), Abandon() have not been called
  // 将缓存中的 DataBlock 刷新到磁盘，此后的加入的键值对会存入新的 DataBlock 中
  // 可以避免相邻的两个键值对在同一个 DataBlock 中
  // 在 Finish() 或 Abandon() 后不能再使用 Flush
  void Flush();

  // Return non-ok iff some error has been detected.
  Status status() const;

  // Finish building the table.  Stops using the file passed to the
  // constructor after this function returns.
  // REQUIRES: Finish(), Abandon() have not been called
  // 结束 sstable 构建
  Status Finish();

  // Indicate that the contents of this builder should be abandoned.  Stops
  // using the file passed to the constructor after this function returns.
  // If the caller is not going to call Finish(), it must call Abandon()
  // before destroying this builder.
  // REQUIRES: Finish(), Abandon() have not been called
  // 放弃 sstable 构建，丢弃缓存中的内容
  void Abandon();

  // Number of calls to Add() so far.
  uint64_t NumEntries() const;

  // Size of the file generated so far.  If invoked after a successful
  // Finish() call, returns the size of the final generated file.
  uint64_t FileSize() const;

 private:
  bool ok() const { return status().ok(); }
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);
  void WriteRawBlock(const Slice& data, CompressionType, BlockHandle* handle);

  struct Rep;
  Rep* rep_; // TableBuilder 实际的字段都在 Rep 中
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_TABLE_BUILDER_H_
