leveldb sstable 文件格式说明
==========================
```
<beginning_of_file>
[data block 1]
[data block 2]
...
[data block N]
[meta block 1]
...
[meta block K]
[metaindex block]
[index block]
[Footer]        (fixed size; starts at file_size - sizeof(Footer))
<end_of_file>
```

table 文件中包含一些被称为 BlockHandle 的内部指针，它包含下列信息：

- offset:   varint64
- size:     varint64

请参考 [varints](https://developers.google.com/protocol-buffers/docs/encoding#varints) 中关于 varint64 的格式说明。

1. 文件中的键值对是有序的，它们被划分为一系列数据块。 这些块一个接一个地出现在文件的开头。每个数据块
的格式在`block_builder.cc`中定义，并可以被压缩。

1. 数据块后面是元信息块，元信息块的类型将在下文进行描述，未来可能添加更多类型的元信息块。

   1. `metaindex` 是对 MetaBlock 的索引，以 key-value 格式存储， key 是 metaindex 的名字，value为指向 meta ndex的BlockHandle

   2. `index` 块包含数据块的指针，以 key-value 格式存储， key 为一个特殊的字符串， 它大于等于目标数据块中最后一个键，且小于下一个数据块中第一个键；value 为数据块的 BlockHandle

2. 文件的最后是一个固定长度的页脚（footer），它包含 metaindex 和 index 块的 BlockHandle 以及一个 magic number

```
metaindex_handle: char[p];     // metaindex 块的指针（BlockHandle）
index_handle:     char[q];     // index 块的指针（BlockHandle）
padding:          char[40-p-q];// 填充的 0 值
                               
magic:            fixed64;     // == 0xdb4775248b80fb57 (little-endian)
```

- table/block.h table/block.cc 定义了读取 Block 的接口
- block_builder.cc 定义了 data block 的内部结构以及构造 data block 的方法
- table/table.h 定义了 sstable 的对外接口
- table/format.h table/format.cc 定义了 BlockHandle、Footer 等结构
- table_builder.cc 定义了 table 的内部结构

## Data Block

Data Block 由 block data、 type crc32 三部分组成。1 个字节的 type 表明采用的压缩方式，目前有 none 和 snappy 两种。

```
    +-----------------+-------+--------+
    |      data       | type  |   crc  |
    +-----------------+-------+--------+
```

### 重启点 restart point

由于 Data Block 中的 key 是有序存储的，这意味着相邻的 key 很可能拥有相同的前缀。如果能够复用前缀可以极大的减少存储空间，

于是 leveldb 中定义两种存储 key 的方式用以压缩存储前缀，一种是重启点(restart point) 它们存储了完整的 key 不复用前缀；另一种普通的 key 则会复用它前面一个相邻重启点的前缀。

DataBlock 中一个键值对的格式如下：

- shared_bytes: varint32 共享前缀长度，对于重启点来说 shared_bytes 始终为 0
- non_shared_bytes: varint32 前缀之后 key 的长度
- value_length: varint32 value 的长度
- key_delta: char[non_shared_bytes] key 在复用前缀之后的部分
- value: char[value_length] 

示例：

```
   +-----------------+-------------------+--------------+------------------+-------+
   | shared_bytes: 0 | unshared_bytes: 3 | value_length | key_delta: "abc" | value |  
   +-----------------+-------------------+--------------+------------------+-------+
   | shared_bytes: 2 | unshared_bytes: 1 | value_length | key_delta: "d"   | value |  
   +-----------------+-------------------+--------------+------------------+-------+ 
   | shared_bytes: 3 | unshared_bytes: 1 | value_length | key_delta: "ef"  | value |  
   +-----------------+-------------------+--------------+------------------+-------+ 
```

第一行是一个重启点
第二行复用了上一条记录的两个字节 "ab" 前缀，加上自己的 key_delta: "d", 这条记录的 key 是 "abd"  
第二行复用了上一条记录的 3 个字节 "abd" 前缀，加上自己的 key_delta: "ef", 这条记录的 key 是 "abdef" 

DataBlock 的结尾部分存储了其中每个重启点的 offset，在查找某个 key 时可以采用二分查找的方法搜索最近的重启点，然后向后遍历寻找目标。

Data Block 整体结构如下：

```
    +--------------------+
    |    key-value 1     |
    +--------------------+
    |    key-value 2     |
    +--------------------+
    |    key-value 3     |
    +--------------------+
    |    restarts[0]     |
    +--------------------+
    |    restarts[1]     |
    +--------------------+
    |    num_restarts    |
    +--------------------+
```

由此可见重启点之间距离过大会拖慢查询性能，Options::block_restart_interval 规定了每隔几个 key 必须有一个重启点。

## Index Block 

IndexBlock 以键值对的格式索引 DataBlock, key 是一个小于 DataBlock 中第一个 key 的字符串，value 是指向 DataBlock 的 BlockHandle.

```
    +--------------+-----------------------+
    |  DataBlock1  | "quick" - "quicklime" |  <----+
    +--------------+-----------------------+       |
    |  DataBlock2  | "quiet" - "quilt"     |  <----|----+
    +--------------+-----------------------+       |    |
    |  DataBlock3  | "quota" - "quorm"     |  <----|----|---+
    +--------------+-----------------------+       |    |   |
                                                   |    |   |
Index Block:                                       |    |   |
    quic                                  ---------+    |   |
    quid (quicklime < quid < quiet)       --------------+   |
    quj  (quilt < quj < quota)            ------------------+
```

如上图所示，指向 DataBlock2 的 index key 为 quid，quid 大于上一个 block 最后一个 key： quicklime，小于 DataBlock2 的第一个 key: quiet

## MetaIndex 

MetaIndex 块中以键值对的格式存储了所有 Meta Block 的位置，key 为 Meta Block 的名字，value 为 Block Handle. 目前的 Meta Block 主要是各种 Filter，它们的名字为 `filter.
## Filter Block

如果在打开数据库时指定了 FilterPolicy 则在 table 文件中会产生相应的 filter 块，比如最经典的 BloomFilter。在执行查询时可以根据 BloomFilter 迅速判断 table 中是否包含某个 key.

