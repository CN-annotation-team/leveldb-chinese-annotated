// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/log_reader.h"

#include <cstdio>

#include "leveldb/env.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {
namespace log {

Reader::Reporter::~Reporter() = default;

Reader::Reader(SequentialFile* file, Reporter* reporter, bool checksum,
               uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      backing_store_(new char[kBlockSize]),
      buffer_(),
      eof_(false),
      last_record_offset_(0),
      end_of_buffer_offset_(0),
      initial_offset_(initial_offset),
      resyncing_(initial_offset > 0) {}

Reader::~Reader() { delete[] backing_store_; }

// 跳转到 initial_offset_ 所在 block 的开头，如果 initial_offset_ 处于结尾填充区则跳到下一个 Block 开头
bool Reader::SkipToInitialBlock() {
  // 在文件中定位 block 开始位置以及 block 内的偏移量
  const size_t offset_in_block = initial_offset_ % kBlockSize;
  uint64_t block_start_location = initial_offset_ - offset_in_block;

  // Don't search a block if we'd be in the trailer
  // 已经到 block 尾部的填充区了，跳到下一个 block
  if (offset_in_block > kBlockSize - 6) {
    block_start_location += kBlockSize;
  }

  // 在 Reader 中记录当前的偏移量
  end_of_buffer_offset_ = block_start_location; 

  // Skip to start of first block that can contain the initial record
  // 不管 offset_in_block, 跳转到 block 的开头
  if (block_start_location > 0) {
    Status skip_status = file_->Skip(block_start_location);
    if (!skip_status.ok()) {
      ReportDrop(block_start_location, skip_status);
      return false;
    }
  }

  return true;
}

bool Reader::ReadRecord(Slice* record, std::string* scratch) {
  if (last_record_offset_ < initial_offset_) {
    if (!SkipToInitialBlock()) { // 跳转到 initial_offset_ 指定的 Block 开头
      return false;
    }
  }

  scratch->clear();
  record->clear();
  bool in_fragmented_record = false;
  // Record offset of the logical record that we're reading
  // 0 is a dummy value to make compilers happy
  // 记录在逻辑记录中的偏移量
  // 逻辑记录是指由一组 FIRST-MIDDLE-LAST 中存储的内容
  // 相对的，Block 中的 Record 称为物理记录
  uint64_t prospective_record_offset = 0;

  Slice fragment;
  while (true) {
    // 读取一个物理记录
    const unsigned int record_type = ReadPhysicalRecord(&fragment);

    // ReadPhysicalRecord may have only had an empty trailer remaining in its
    // internal buffer. Calculate the offset of the next physical record now
    // that it has returned, properly accounting for its header size.
    uint64_t physical_record_offset =
        end_of_buffer_offset_ - buffer_.size() - kHeaderSize - fragment.size();

    if (resyncing_) {
      if (record_type == kMiddleType) {
        continue;
      } else if (record_type == kLastType) {
        resyncing_ = false;
        continue;
      } else {
        resyncing_ = false;
      }
    }

    switch (record_type) {
      case kFullType:
        if (in_fragmented_record) {
          // Handle bug in earlier versions of log::Writer where
          // it could emit an empty kFirstType record at the tail end
          // of a block followed by a kFullType or kFirstType record
          // at the beginning of the next block.
          if (!scratch->empty()) {
            ReportCorruption(scratch->size(), "partial record without end(1)");
          }
        }
        prospective_record_offset = physical_record_offset;
        scratch->clear();
        *record = fragment;
        last_record_offset_ = prospective_record_offset;
        return true;

      case kFirstType:
        if (in_fragmented_record) {
          // Handle bug in earlier versions of log::Writer where
          // it could emit an empty kFirstType record at the tail end
          // of a block followed by a kFullType or kFirstType record
          // at the beginning of the next block.
          if (!scratch->empty()) {
            ReportCorruption(scratch->size(), "partial record without end(2)");
          }
        }
        prospective_record_offset = physical_record_offset;
        scratch->assign(fragment.data(), fragment.size());
        in_fragmented_record = true;
        break;

      case kMiddleType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(),
                           "missing start of fragmented record(1)");
        } else {
          scratch->append(fragment.data(), fragment.size());
        }
        break;

      case kLastType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(),
                           "missing start of fragmented record(2)");
        } else {
          scratch->append(fragment.data(), fragment.size());
          *record = Slice(*scratch);
          last_record_offset_ = prospective_record_offset;
          return true;
        }
        break;

      case kEof:
        if (in_fragmented_record) {
          // This can be caused by the writer dying immediately after
          // writing a physical record but before completing the next; don't
          // treat it as a corruption, just ignore the entire logical record.
          scratch->clear();
        }
        return false;

      case kBadRecord:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "error in middle of record");
          in_fragmented_record = false;
          scratch->clear();
        }
        break;

      default: {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "unknown record type %u", record_type);
        ReportCorruption(
            (fragment.size() + (in_fragmented_record ? scratch->size() : 0)),
            buf);
        in_fragmented_record = false;
        scratch->clear();
        break;
      }
    }
  }
  return false;
}

uint64_t Reader::LastRecordOffset() { return last_record_offset_; }

void Reader::ReportCorruption(uint64_t bytes, const char* reason) {
  ReportDrop(bytes, Status::Corruption(reason));
}

void Reader::ReportDrop(uint64_t bytes, const Status& reason) {
  if (reporter_ != nullptr &&
      end_of_buffer_offset_ - buffer_.size() - bytes >= initial_offset_) {
    reporter_->Corruption(static_cast<size_t>(bytes), reason);
  }
}


// ReadPhysicalRecord 每次调用都会在 *result 中装载一个 record, 并返回这条记录的类型。
// ReadPhysicalRecord 的代码有点绕，让我们简化一下:
// 我们在代码中标注了 case1, case2, case3 三个标签，与伪代码相对应
// 
//    while true {
//        if buffer.size() < kHeaderSize && !eof {
//            // 初始状态
//            // 或者是, 读完了一个 Block 后离开 case2，此时 buffer 可能为空或是尾部填充的 0
//            case1:
//                eof = read(buffer, kBlockSize) // 尝试向 buffer 中读入一个 Block, 顺便更新 eof
//                continue // 如果 buffer.size() >= kHeaderSize 则进入 case2, 否则进入 case3 (buffer 不够必然是遇到了 eof)
//        } else if kHeaderSize <= buffer.size() {
//            assert(buffer.size() <= kBlockSize)
//            // buffer 中至少有一个 record
//            case2: 
//                readRecord() // 读取一个记录并将它从 buffer 中移除
//                goto start // 如果 buffer.size() >= kHeaderSize 则继续在 case2 循环。退出循环后，尚未遇到 eof 则会进入 case1, 如果已经遇到了 eof 则会进入 case3
//        } else if buffer.size() < kHeaderSize && eof {
//            // 遇到了文件尾，此时有两种可能
//            // 如果 buffer 为空或者尾部填充的 0，说明正常结束
//            // 如果 buffer 中其它数据说明遇到了一个损坏的 header
//            case3:
//                exit
//        }
//    }
//
unsigned int Reader::ReadPhysicalRecord(Slice* result) {
  while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        // case1: buffer_.size() < kHeaderSize && eof_ == false 
        // 此时尚未开始读取或者已经读完了若干个完整 Record, buffer 中只剩下了尾部填充
        // Last read was a full read, so this is a trailer to skip
        buffer_.clear();
        Status status = file_->Read(kBlockSize, &buffer_, backing_store_);
        end_of_buffer_offset_ += buffer_.size();
        if (!status.ok()) { 
          // 在读完 block 之前遇到了错误，上报错误
          buffer_.clear();
          ReportDrop(kBlockSize, status);
          eof_ = true;
          return kEof;
        } else if (buffer_.size() < kBlockSize) {
          eof_ = true;
        }
        continue;
      } else {
        // Note that if buffer_ is non-empty, we have a truncated header at the
        // end of the file, which can be caused by the writer crashing in the
        // middle of writing the header. Instead of considering this an error,
        // just report EOF.
        // case3: buffer_.size() < kHeaderSize && eof_ ==  true
        // 此时可能已经正常结束也可能最后几个字节中是一个损坏的 header, 不检测了直接 EOF
        buffer_.clear();
        return kEof;
      }
    }

    // case2: kHeaderSize <= buffer_.size() <= kBlockSize buffer 中有至少一个完整的 Block
    // Parse the header
    // Record 的结构为： checksum: uint32 + length: uint16 + type: uint8 + data: uint8[length]
    // 解析 Record 的 length
    const char* header = buffer_.data();
    const uint32_t a = static_cast<uint32_t>(header[4]) & 0xff;
    const uint32_t b = static_cast<uint32_t>(header[5]) & 0xff;
    const unsigned int type = header[6];
    const uint32_t length = a | (b << 8);
    if (kHeaderSize + length > buffer_.size()) {
      size_t drop_size = buffer_.size();
      buffer_.clear();
      if (!eof_) {
        // record 超出了 block 的大小，报错
        ReportCorruption(drop_size, "bad record length");
        return kBadRecord;
      }
      // If the end of the file has been reached without reading |length| bytes
      // of payload, assume the writer died in the middle of writing the record.
      // Don't report a corruption.
      // 碰到 EOF 说明 writer 写入途中崩溃了，不报错
      return kEof;
    }

    // 跳过空白记录
    if (type == kZeroType && length == 0) {
      // Skip zero length record without reporting any drops since
      // such records are produced by the mmap based writing code in
      // env_posix.cc that preallocates file regions.
      buffer_.clear();
      return kBadRecord;
    }

    // 校验 CRC
    // Check crc
    if (checksum_) {
      uint32_t expected_crc = crc32c::Unmask(DecodeFixed32(header));
      uint32_t actual_crc = crc32c::Value(header + 6, 1 + length);
      if (actual_crc != expected_crc) {
        // Drop the rest of the buffer since "length" itself may have
        // been corrupted and if we trust it, we could find some
        // fragment of a real log record that just happens to look
        // like a valid log record.
        size_t drop_size = buffer_.size();
        buffer_.clear();
        ReportCorruption(drop_size, "checksum mismatch");
        return kBadRecord;
      }
    }

    // 从 buffer 中移除已读的 Record
    buffer_.remove_prefix(kHeaderSize + length);

    // Skip physical record that started before initial_offset_
    if (end_of_buffer_offset_ - buffer_.size() - kHeaderSize - length <
        initial_offset_) {
      result->clear();
      return kBadRecord;
    }

    // Record 内容写入 result
    *result = Slice(header + kHeaderSize, length);
    return type;
  }
}

}  // namespace log
}  // namespace leveldb
