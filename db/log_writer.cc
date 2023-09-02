// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/log_writer.h"

#include <cstdint>

#include "leveldb/env.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {
namespace log {

static void InitTypeCrc(uint32_t* type_crc) {
  for (int i = 0; i <= kMaxRecordType; i++) {
    char t = static_cast<char>(i);
    type_crc[i] = crc32c::Value(&t, 1);
  }
}

Writer::Writer(WritableFile* dest) : dest_(dest), block_offset_(0) {
  InitTypeCrc(type_crc_);
}

Writer::Writer(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(dest_length % kBlockSize) {
  InitTypeCrc(type_crc_);
}

Writer::~Writer() = default;

Status Writer::AddRecord(const Slice& slice) {
  const char* ptr = slice.data();
  size_t left = slice.size(); // left 表示还有多少用户数据需要写入

  // Fragment the record if necessary and emit it.  Note that if slice
  // is empty, we still want to iterate once to emit a single
  // zero-length record
  Status s;
  bool begin = true;
  // 尝试将数据分成多个 Record, 每次循环写入一个分段直至完全写入
  do {
    const int leftover = kBlockSize - block_offset_; // 计算 Block 中剩余的空间
    assert(leftover >= 0);
    if (leftover < kHeaderSize) { // 小于 7 个字节，放不下一个 record, 将其填充
      // Switch to a new block
      if (leftover > 0) {
        // Fill the trailer (literal below relies on kHeaderSize being 7)
        static_assert(kHeaderSize == 7, "");
        // 填充 0 值
        dest_->Append(Slice("\x00\x00\x00\x00\x00\x00", leftover));
      }
      // 切换到新的 block, 重置 block_offset_
      block_offset_ = 0;
    }

    // Invariant: we never leave < kHeaderSize bytes in a block.
    // assert: 当前 block 剩余空间大于等于 7 字节
    assert(kBlockSize - block_offset_ - kHeaderSize >= 0);
  
    // 计算 Block 中剩余多少空间可以存储用户数据
    const size_t avail = kBlockSize - block_offset_ - kHeaderSize; 
    // 判断是否需要分成多个片段，以及当前分段的大小
    const size_t fragment_length = (left < avail) ? left : avail;

    // 计算 RecordType
    RecordType type;
    const bool end = (left == fragment_length); 
    if (begin && end) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }
    // EmitPhysicalRecord 实际写入 Record 
    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = false;
  } while (s.ok() && left > 0);
  return s;
}

Status Writer::EmitPhysicalRecord(RecordType t, const char* ptr,
                                  size_t length) {
  assert(length <= 0xffff);  // Must fit in two bytes
  assert(block_offset_ + kHeaderSize + length <= kBlockSize);

  // 编码 length 和 type
  // Format the header
  char buf[kHeaderSize];
  buf[4] = static_cast<char>(length & 0xff);
  buf[5] = static_cast<char>(length >> 8);
  buf[6] = static_cast<char>(t);

  // 计算 CRC 校验和
  // Compute the crc of the record type and the payload.
  uint32_t crc = crc32c::Extend(type_crc_[t], ptr, length);
  crc = crc32c::Mask(crc);  // Adjust for storage
  EncodeFixed32(buf, crc); // 编码为 uint32

  // 写入 header 和数据
  // Write the header and the payload
  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) {
    s = dest_->Append(Slice(ptr, length));
    if (s.ok()) {
      s = dest_->Flush();
    }
  }
  // 更新 block_offset
  block_offset_ += kHeaderSize + length;
  return s;
}

}  // namespace log
}  // namespace leveldb
