level db 的日志格式
=================

level db 日志文件由一系列 32KB 的块（Block）组成。唯一的例外是文件尾部可能包含不完整的块。

每个 Block由一系列记录（Record）组成，它的结构为：

- checksum: type 和 data 字段的 crc32 校验和，小端序
- length: uint16 小端序
- type: uint8, FULL/FIRST/MIDDLE/LAST 之一
- data

从上面的结构中可以看出，一个 data 为空的 Record 需要 7 个字节，所以当 Block 中剩余空间小于 7 字节时剩余的字节用 0 填充(填充的字节在 leveldb 中被称为 trailer)，并且在读取时需要跳过。

Record 有四种 type:

- FULL: 1
- FIRST: 2
- MIDDLE: 3
- LAST: 4

如果某个 Record 记录了完整的内容，那么它的类型为 FULL. 如果由于 Block 空间不足等原因，需要将用户的内容分成多个 Record, 第一个 Record 类型为 FIRST, 最后一个 Record 的类型为 LAST, 如果需要更多 Record 则在中间添加若干类型为 MIDDLE 的 Record. LAST 中可能包含 trailer.

