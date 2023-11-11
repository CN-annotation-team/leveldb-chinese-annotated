在上文介绍完 sstable 的结构之后，本文来探究一下 sstable 构造过程。

### TableBuilder

sstable 由 TableBuilder 负责构建，其中有几个主要方法:

> 结构介绍在这里：[05-SSTable](./05-SSTable.md#Table基本结构)
> 源码传送门 [table_builder.h](../table/table_builder.h) / [table_builder.cc](../table/table_builder.cc)

```cpp
// 创建 TableBuilder 在 *file 上构建 sstable
TableBuilder(const Options& options, WritableFile* file);

// 添加一个键值对, key 必须大于上一个 key 以保证有序
void Add(const Slice& key, const Slice& value);

// 将缓存中的 DataBlock 刷新到磁盘，此后的加入的键值对会存入新的 DataBlock 中
// 可以避免相邻的两个键值对在同一个 DataBlock 中
void Flush();

// 结束 sstable 构建
Status Finish();

// 放弃 sstable 构建，丢弃缓存中的内容
void Abandon();
```

TableBuilder 中的数据都存储在 [TableBuilder::Rep](../table/table_builder.cc) 内部类中, 其中的字段我们在遇到时再解释。

首先看一下 Add 函数，其中唯一略微复杂的地方是 pending_index_entry 机制：

```cpp
void TableBuilder::Add(const Slice& key, const Slice& value) {
    if (r->pending_index_entry) { 
        // 这个分支建议看完整个函数之后回头再看
        
        // 05-SSTable 一节中讲过，IndexBlock 使用字典序介于当前 DataBlock 第一个 key 与上一个 DataBlock 最后一个 key 中间的字符串作为当前 DataBlock 的索引。因此在添加下一个 DataBlock 第一个 key 时才能确定上一个 DataBlock 的索引。
        // 当一个 DataBlock 结束时 pending_index_entry 会被置为 true，这样下一次调用 Add 函数写入下一个 DataBlock 第一个 entry 时就会进入这个分支，为上一个 DataBlock 构建索引。
        
        // FindShortestSeparator 负责找到介于两个 Block 中间的字符串作为索引值, 并将它写入 r->last_key
        r->options.comparator->FindShortestSeparator(&r->last_key, key);
        std::string handle_encoding;
        r->pending_handle.EncodeTo(&handle_encoding);
        // 将上一个 DataBlock 的索引写入 IndexBlock
        r->index_block.Add(r->last_key, Slice(handle_encoding));
        // 重置 pending_index_entry，让后续的键值对正常写入
        r->pending_index_entry = false;
    }

    // 将 key 加入到 filter block 中
    if (r->filter_block != nullptr) {
        r->filter_block->AddKey(key);
    }

    // 将键值对存入 DataBlock, 更新各类统计信息
    r->last_key.assign(key);
    r->num_entries++;
    r->data_block.Add(key, value);
    
    // 若当前 DataBlock 已满则刷盘并开始下一个 Block
    const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
    if (estimated_block_size >= r->options.block_size) {
        // Flush 具体负责结束 DataBlock 并开始下一个 Block 的操作
        Flush();
    }
}
```

顺便简单看一下 Flush 做了什么：

```cpp
void TableBuilder::Flush() {
  // 将 data_block 写入文件，data_block 的指针会被存入 pending_handle 中
  // WriteBlock 负责压缩、CRC 校验之类的工作，具体格式上文已经介绍过这里就不展开了
  WriteBlock(&r->data_block, &r->pending_handle);
  if (ok()) {
    // 写入成功
    // 将 pending_index_entry 设为 true，下次调用 Add 时会写入 Index Entry
    r->pending_index_entry = true; 
    r->status = r->file->Flush();
  }
  if (r->filter_block != nullptr) {
    // 将下一个 DataBlock 的开头偏移量传给 filter builder 为它准备 filter
    r->filter_block->StartBlock(r->offset);
  }
}
```

首个 DataBlock 的索引和 Filter 在 TableBuilder 构造时即完成了构造和初始化，这里就不贴出代码了。

### BlockBuilder

TableBuilder 的 data_block 和 index_block 字段的类型都是 BlockBuilder类型，BlockBuilder 如其名字负责具体 Block 的构造工作。

> DataBlock 结构介绍在这里：[05-SSTable](./05-SSTable.md#DataBlock)
> 源码传送门 [block_builder.h](../table/block_builder.h) / [block_builder.cc](../table/block_builder.cc)

BlockBuilder 中主要的复杂逻辑是我们在[之前](./05-SSTable.md#RestartPoints)介绍过的 restart points 机制，现在我们可以看一下具体实现。

```cpp
void BlockBuilder::Add(const Slice& key, const Slice& value) {
  if (counter_ < options_->block_restart_interval) {
    // 上个重启点之后的键值对数量未超出限制
    // 检查和上一个key有多少公共前缀可以复用
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
      shared++; 
    }
  } else {
    // 超出限制，当前键值对作为新的重启点
    restarts_.push_back(buffer_.size());
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;
  // 向 buffer 写入当前键值对的 shared_bytes, non_shared_bytes, value_size 三个字段  
  PutVarint32(&buffer_, shared);
  PutVarint32(&buffer_, non_shared);
  PutVarint32(&buffer_, value.size());
  // 写入 key_delta 和 value
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());
}
```

Finish 函数负责写入 DataBlock 结尾的 restart_points 数组：

```cpp
Slice BlockBuilder::Finish() {
  // 将 restart_points 写入 DataBLock
  for (size_t i = 0; i < restarts_.size(); i++) {
    PutFixed32(&buffer_, restarts_[i]);
  }
  // 写入 restart_points 个数
  PutFixed32(&buffer_, restarts_.size());
  finished_ = true;
  return Slice(buffer_);
}
```

DataBlock 的压缩和 crc 由 TableBuilder::WriteBlock 负责。

### FilterBlock

FilterBlock 由 [filter_block.cc](../table/filter_block.cc)  负责构建，TableBuilder 每完成一个 DataBlock 即调用一次 GenerateFilter

```cpp
void FilterBlockBuilder::GenerateFilter() {
  if (num_keys == 0) {
    // 没有 key 立即返回
    filter_offsets_.push_back(result_.size());
    return;
  }
  
  // 将 keys_ 中的 key 导出到 tmp_keys_
  // keys_ 是把所有 key join 成的长字符串，start_ 是每个 key 在 keys_ 中的开始地址
  // 比如 keys_="abcdef" start_=[0,3] 表示两个 key: abc 和 def
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i + 1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // 将当前 filter 的 offset 存入 filter_offsets_
  filter_offsets_.push_back(result_.size());
  // 调用 FilterPolicy 构造过滤器
  policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

  // 重置状态
  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}
```