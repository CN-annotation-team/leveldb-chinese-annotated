// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_TABLE_TWO_LEVEL_ITERATOR_H_
#define STORAGE_LEVELDB_TABLE_TWO_LEVEL_ITERATOR_H_

#include "leveldb/iterator.h"

namespace leveldb {

struct ReadOptions;

// TwoLevelIterator 原本设计用来遍历 table, index_iter 负责遍历 IndexBlock 并返回其中 DataBlock 的指针
// block_function 的 arg 参数接受 index_iter 返回的 DataBlock 指针并返回相应 Block 的迭代器
// TwoLevelIterator 也被用在 merge 过程中，此时它可以抽象为遍历一个二维数组:
// 
//   std::array<std::array<int, 3>, 2> vec2 = {{{1, 2, 3}, {4, 5, 6}}};
//   auto index_iter = vec2.begin(); // index_iter 负责遍历第一层
//   while (index_iter != arr.end()) {
//     auto data_iter = index_iter->begin(); // block_function 等价于 index_iter->begin()
//     while (data_iter != it1->end()) { // data_iter 负责遍历第二层
//       std::cout << *data_iter << " ";
//       ++data_iter;
//     }
//     ++index_iter
//   }
// 
// Return a new two level iterator.  A two-level iterator contains an
// index iterator whose values point to a sequence of blocks where
// each block is itself a sequence of key,value pairs.  The returned
// two-level iterator yields the concatenation of all key/value pairs
// in the sequence of blocks.  Takes ownership of "index_iter" and
// will delete it when no longer needed.
//
// Uses a supplied function to convert an index_iter value into
// an iterator over the contents of the corresponding block.
Iterator* NewTwoLevelIterator(
    Iterator* index_iter,
    Iterator* (*block_function)(void* arg, const ReadOptions& options,
                                const Slice& index_value),
    void* arg, const ReadOptions& options);

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_TABLE_TWO_LEVEL_ITERATOR_H_
