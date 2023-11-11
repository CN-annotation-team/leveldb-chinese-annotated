在前面的几篇文章中我们已经讨论了 leveldb 中三个重要组件：MemTable、Log 以及 SSTable，从本文开始我们将开始探究 leveldb 核心流程。

[include/leveldb/db.h](../include/leveldb/db.h) 中的 DB 类定义了 leveldb 对外暴露的接口，它的实现代码在 [db/db_impl.cc](../db/db_impl.cc), 而 db_impl.cc 也是 leveldb 的核心所在。

在了解 leveldb 如何组织数据之前很难看懂读取逻辑，所以我们先从写逻辑入手, 看看 leveldb 如何组织数据。

## WriteBatch

写流程的入口在 DB::Put 函数中：

```cpp
// Convenience methods
Status DBImpl::Put(const WriteOptions& o, const Slice& key, const Slice& val) {
  return DB::Put(o, key, val);
}

Status DB::Put(const WriteOptions& opt, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(opt, &batch);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates);
```

在上面的代码中我们注意到写入过程实际是由 DB::Write 函数执行的，DB::Put 只是它的快捷方式。而 DB::Write 写入的并不是一个键值对而是 WriteBatch。

WriteBatch 是一个写事务，它将一串写操作包装在一起，由 Write 函数原子性的写入。

> 源码传送门 [WriteBatch.h](../include/write_batch.h)/[WriteBatch.cc](../db/write_batch.cc)

由于 leveldb 采用新版本键值对覆盖旧版本的方式进行更新和删除，所以 WriteBatch 中直接存储了新版本的键值对。

![](img017.png)

WriteBatch 中的键值对经过编码后存储在 string 类型的 rep_ 字段中, 上图展示了 WriteBatch 编码的结构。开头的 8 位是用于 MVCC 的 SequenceNumber，接下来的 4bit 以 FixedUint32 编码存储 WriteBatch 中键值对的数量。

键值对编码由 KeySize、UserKey、ValueSize、Value 四部分组成，KeySize 和 ValueSize 采用 varint 编码记录 UserKey 和 Value 的长度。

我们在[02-工具类](02-utils.md#internalkey) 中简单介绍过 SequenceNumber，它是一个递增的全局序列号，用于实现多版本并发控制(MVCC)。每个 WriteBatch 都会拥有一个唯一的 SequenceNumber，在写入时 SequenceNumber 会存储在键值对的 InternalKey 中。读事务会先获得当前的序列号然后只读取序列号更小的键值对，这样在读事务开始后的写入便不会被看到。

## DBImpl::Write

DBImpl::Write 函数负责实际的写入工作，它的代码极其复杂我们分开来看。

## writers 队列

leveldb 写入操作的固定开销很大，说白话就是一次写入十个 WriteBatch 的耗时远小于分十次每次写入一 WriteBatch。为了提高性能，Write 函数引入了 writers 队列将多个 WriteBatch 一次性写入。

writers 的代码并不容易读懂，我们先画个时序图再来看代码：

![](img018.png)

```cpp
Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  // 创建一个 writer
  // Writer 在多线程间协调时代表一个线程
  Writer w(&mutex_); 
  MutexLock l(&mutex_);  
  // 将代表自己的 writer 加入到队列
  writers_.push_back(&w);
  // 如果自己不是队列中第一个 writer 则通过条件变量 w.cv 等待 w.done 变为 true
  while (!w.done && &w != writers_.front()) {
    // 条件变量 CondVar 的 Wait 方法会暂时释放锁，然后阻塞当前线程
    // 在 Wait 期间，其它线程可以拿到锁，并在完成操作后将等待中的线程唤醒
    // 被唤醒的线程将重新拿回锁
    w.cv.Wait(); 
  }
  // 能通过 while 进入这里的线程有两种情况：
  // 1. 自己就是队列头，需要负责写入
  // 2. 自己的数据已被其它线程写入，直接退出就好
  //
  // 所有进入此处的线程都持有锁
  if (w.done) { 
    // 返回后 MutexLock 析构，自动释放锁
    return w.status;
  }
  
  // 将队列中所有等待的 writer 打包成一个 write_batch
  // 此时当前线程持有 mutex_, 其它线程无法入队
   WriteBatch* write_batch = BuildBatchGroup(&last_writer);
      
  // 解锁之后，在当前线程写入期间其它线程就可以将自己的 writer 入队了
  mutex_.Unlock(); 
  
  // 此处省略三百行实际写入流程代码

  // 写入完成，通过条件变量唤醒那些工作已被自己代劳的线程
  while (true) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != &w) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) break;
  }
  // 唤醒队列中排在后面的第一个 writer 进行下一次写入
   // Notify new head of write queue
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }
}
```