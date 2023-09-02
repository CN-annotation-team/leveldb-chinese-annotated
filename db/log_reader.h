// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_LOG_READER_H_
#define STORAGE_LEVELDB_DB_LOG_READER_H_

#include <cstdint>

#include "db/log_format.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace leveldb {

class SequentialFile;

namespace log {

class Reader {
 public:
  // Reporter 用于上报日志损坏
  // Interface for reporting errors.
  class Reporter {
   public:
    virtual ~Reporter();

    // Some corruption was detected.  "bytes" is the approximate number
    // of bytes dropped due to the corruption.
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  // Create a reader that will return log records from "*file".
  // "*file" must remain live while this Reader is in use.
  //
  // If "reporter" is non-null, it is notified whenever some data is
  // dropped due to a detected corruption.  "*reporter" must remain
  // live while this Reader is in use.
  //
  // If "checksum" is true, verify checksums if available.
  //
  // The Reader will start reading at the first record located at physical
  // position >= initial_offset within the file.
  //
  // 创建一个 Reader, 在 Reader 使用期间 file 必须存活
  // 如果 reporter 不为空，所有数据损坏都会通过 reporter 上报，在 Reader 使用期间 reporter 必须存活
  // checksum 用于控制读取时是否检测 CRC 校验和
  // Reader 从 initial_offset 后第一个 Record 开始读
  Reader(SequentialFile* file, Reporter* reporter, bool checksum,
         uint64_t initial_offset);

  // 禁止复制
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  ~Reader();

  // Read the next record into *record.  Returns true if read
  // successfully, false if we hit end of the input.  May use
  // "*scratch" as temporary storage.  The contents filled in *record
  // will only be valid until the next mutating operation on this
  // reader or the next mutation to *scratch.
  //
  // 读取记录存储在 record 中，如果正确读取返回 true, 如果遇到文件结尾则返回 false
  // 读取过程中可能临时使用 scratch 缓冲区
  // read 中的记录会在再次使用 Reader 或者修改 scratch 之后失效 
  bool ReadRecord(Slice* record, std::string* scratch);

  // Returns the physical offset of the last record returned by ReadRecord.
  //
  // Undefined before the first call to ReadRecord.
  // 获得 ReadRecord 返回的最后一条记录的 offset
  // 调用 ReadRecord 之前获取 LastRecordOffset 会导致未定义行为
  uint64_t LastRecordOffset();

 private:
  // Extend record types with the following special values
  enum {
    kEof = kMaxRecordType + 1,
    // Returned whenever we find an invalid physical record.
    // Currently there are three situations in which this happens:
    // * The record has an invalid CRC (ReadPhysicalRecord reports a drop)
    // * The record is a 0-length record (No drop is reported)
    // * The record is below constructor's initial_offset (No drop is reported)
    kBadRecord = kMaxRecordType + 2
  };

  // Skips all blocks that are completely before "initial_offset_".
  //
  // Returns true on success. Handles reporting.
  // 
  // 跳过 initial_offset_ 前面所有完整的 block
  // 即跳到 initial_offset_ 所在 block 的开头，如果 initial_offset_ 处于结尾填充区则跳到下一个 Block 开头
  // 成功返回 true，内部会上报数据损坏
  bool SkipToInitialBlock();

  // Return type, or one of the preceding special values
  // 返回记录的类型，包括 kEof 和 kBadRecord 等前面定义的特殊值
  unsigned int ReadPhysicalRecord(Slice* result);

  // Reports dropped bytes to the reporter.
  // buffer_ must be updated to remove the dropped bytes prior to invocation.
  void ReportCorruption(uint64_t bytes, const char* reason);
  void ReportDrop(uint64_t bytes, const Status& reason);

  SequentialFile* const file_;
  Reporter* const reporter_;
  bool const checksum_;
  // backing_store_ 是读取用的缓冲区，buffer_ 底层实际存储数据的地方
  // 它是一个生命周期与 reader 一样长的 32KB 数组，这样可以避免频繁的内存分配与回收
  // Record 中的数据也指向 backing_store_, 再次调用 ReadRecord() 后 backing_store_ 中的数据会被覆盖，上次返回的 Record 将会失效
  char* const backing_store_;
  // 注意 Slice 类只提供读写数据的函数并不负责内存管理和分配
  // buffer_ 只是访问读写缓冲区的接口，数据实际存储在 backing_store_ 中
  Slice buffer_;
  // 表示是否读到了 EOF
  bool eof_;  // Last Read() indicated EOF by returning < kBlockSize

  // Offset of the last record returned by ReadRecord.
  // ReadRecord 最近返回的一条记录的 offset
  uint64_t last_record_offset_;
  // Offset of the first location past the end of buffer_.
  // 当前文件的读取进度
  uint64_t end_of_buffer_offset_;

  // Offset at which to start looking for the first record to return
  uint64_t const initial_offset_;

  // True if we are resynchronizing after a seek (initial_offset_ > 0). In
  // particular, a run of kMiddleType and kLastType records can be silently
  // skipped in this mode
  bool resyncing_;
};

}  // namespace log
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_LOG_READER_H_
