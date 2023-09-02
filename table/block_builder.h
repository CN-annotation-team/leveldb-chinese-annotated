// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
#define STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_

#include <cstdint>
#include <vector>

#include "leveldb/slice.h"

namespace leveldb {

struct Options; // 具体定义在 options.h

class BlockBuilder {
 public:
  explicit BlockBuilder(const Options* options);

  // 禁止复制
  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // Reset the contents as if the BlockBuilder was just constructed.
  void Reset();

  // REQUIRES: Finish() has not been called since the last call to Reset().
  // REQUIRES: key is larger than any previously added key
  // 要求在上次 Reset() 之后还没有调用过 Finish()
  void Add(const Slice& key, const Slice& value);

  // Finish building the block and return a slice that refers to the
  // block contents.  The returned slice will remain valid for the
  // lifetime of this builder or until Reset() is called.
  // 结束 Block 构建, 并返回指向 Block 数据的 Slice. 在调用 Reset() 之前，返回的 Slice 会保持可用
  Slice Finish();

  // Returns an estimate of the current (uncompressed) size of the block
  // we are building.
  // 返回对正在构建的 Block 的估计大小(即不考虑压缩)
  size_t CurrentSizeEstimate() const;

  // Return true iff no entries have been added since the last Reset()
  // 判断正在构建的 Block 是否为空白
  bool empty() const { return buffer_.empty(); }

 private:
  const Options* options_;
  std::string buffer_;              // Destination buffer 输出缓冲区
  std::vector<uint32_t> restarts_;  // Restart points 重启点的 offset，重启点相关介绍参考 table_format_cn.md
  int counter_;                     // Number of entries emitted since restart 从上个重启点开始键值对的数量
  bool finished_;                   // Has Finish() been called? 是否调用过 finished
  std::string last_key_;            // 上一条记录的 key， 用于复用公共前缀
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_BLOCK_BUILDER_H_
