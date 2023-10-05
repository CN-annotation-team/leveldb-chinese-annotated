// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_SKIPLIST_H_
#define STORAGE_LEVELDB_DB_SKIPLIST_H_

// Thread safety
// -------------
//
// Writes require external synchronization, most likely a mutex.
// Reads require a guarantee that the SkipList will not be destroyed
// while the read is in progress.  Apart from that, reads progress
// without any internal locking or synchronization.
//
// Invariants:
//
// (1) Allocated nodes are never deleted until the SkipList is
// destroyed.  This is trivially guaranteed by the code since we
// never delete any skip list nodes.
//
// (2) The contents of a Node except for the next/prev pointers are
// immutable after the Node has been linked into the SkipList.
// Only Insert() modifies the list, and it is careful to initialize
// a node and use release-stores to publish the nodes in one or
// more lists.
//
// ... prev vs. next pointer ordering ...

// 线程安全
// -------
// 
// 写入需要锁等外部机制来保证线程安全
// 进行读取时需要保证跳表不被销毁
// 除此之外，读取不需要任何锁或同步机制
//
// 不变量：
// (1) 在跳表销毁之前所有的节点都不会被删除。
// (2) 节点的内容在加入跳表后就不会被改变

#include <atomic>
#include <cassert>
#include <cstdlib>

#include "util/arena.h"
#include "util/random.h"

namespace leveldb {

template <typename Key, class Comparator>
class SkipList {
 private:
  struct Node;

 public:
  // Create a new SkipList object that will use "cmp" for comparing keys,
  // and will allocate memory using "*arena".  Objects allocated in the arena
  // must remain allocated for the lifetime of the skiplist object.
  // 在 arena 上创建一个新的跳表，cmp 用于指定 key 的排序
  // 在 arena 中分配的对象必须在跳表对象的生命周期内保持存在
  explicit SkipList(Comparator cmp, Arena* arena);

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  // Insert key into the list.
  // REQUIRES: nothing that compares equal to key is currently in the list.
  // 将 key 插入到跳表中
  // 调用时需要保证跳表中不存在相同 key
  void Insert(const Key& key);

  // Returns true iff an entry that compares equal to key is in the list.
  // 判断跳表中是否存在某个 key
  bool Contains(const Key& key) const;

  // Iteration over the contents of a skip list
  class Iterator {
   public:
    // Initialize an iterator over the specified list.
    // The returned iterator is not valid.
    // 初始化一个迭代器
    // 返回的迭代器需要先 SeekToFirst 或 SeekToLast 之后才可用
    explicit Iterator(const SkipList* list);

    // Returns true iff the iterator is positioned at a valid node.
    // 判断迭代器是否指向了可用的节点
    bool Valid() const;

    // Returns the key at the current position.
    // REQUIRES: Valid()
    // 返回当前的 key
    // 需要迭代器可用
    const Key& key() const;

    // Advances to the next position.
    // REQUIRES: Valid()
    // 迭代器指向下一个 key
    // 需要迭代器可用
    void Next();

    // Advances to the previous position.
    // REQUIRES: Valid()
    // 迭代器指向上一个 key
    // 需要迭代器可用
    void Prev();

    // Advance to the first entry with a key >= target
    // 迭代器指向第一个 key >= target 的节点
    // 需要迭代器可用
    void Seek(const Key& target);

    // Position at the first entry in list.
    // Final state of iterator is Valid() iff list is not empty.
    // 指向跳表第一个节点
    void SeekToFirst();

    // Position at the last entry in list.
    // Final state of iterator is Valid() iff list is not empty.
    // 指向跳表最后一个节点
    void SeekToLast();

   private:
    const SkipList* list_;
    Node* node_;
    // Intentionally copyable
  };

 private:
  enum { kMaxHeight = 12 };

  inline int GetMaxHeight() const {
    return max_height_.load(std::memory_order_relaxed);
  }

  Node* NewNode(const Key& key, int height);
  int RandomHeight();
  bool Equal(const Key& a, const Key& b) const { return (compare_(a, b) == 0); }

  // Return true if key is greater than the data stored in "n"
  bool KeyIsAfterNode(const Key& key, Node* n) const;

  // Return the earliest node that comes at or after key.
  // Return nullptr if there is no such node.
  //
  // If prev is non-null, fills prev[level] with pointer to previous
  // node at "level" for every level in [0..max_height_-1].
  // 寻找并返回大于等于 key 的第一个节点 x, 并将 x 在各层的前驱节点存储在 prev 数组中
  // 
  // 示例：
  //  Lv2: 1 ---------------------------> 13
  //                           
  //  Lv1: 1 ------> 5 ------> 9 -------> 13
  //
  //  Lv0: 1 -> 3 -> 5 -> 7 -> 9 -> 11 -> 13
  //
  // 在上图的跳表中 FindGreaterOrEqual(8) 返回节点 9, prev 为节点 9 在各层的前驱节点 [1, 5, 7]
  Node* FindGreaterOrEqual(const Key& key, Node** prev) const;

  // Return the latest node with a key < key.
  // Return head_ if there is no such node.
  // 返回小于 key 的最后一个节点
  // 如果不存在则返回 head
  Node* FindLessThan(const Key& key) const;

  // Return the last node in the list.
  // Return head_ if list is empty.
  // 返回跳表中最后一个节点
  // 如果不存在则返回 head
  Node* FindLast() const;

  // Immutable after construction
  // 跳表创建之后，compare_ 和 arena_ 就不能再改变  
  Comparator const compare_;
  Arena* const arena_;  // Arena used for allocations of nodes

  Node* const head_;

  // Modified only by Insert().  Read racily by readers, but stale
  // values are ok.
  // 只有 Insert 会修改最大高度
  // 读取线程会进入 race condition, 但仍能正常工作
  std::atomic<int> max_height_;  // Height of the entire list

  // Read/written only by Insert().
  Random rnd_;
};

// Implementation details follow
template <typename Key, class Comparator>
struct SkipList<Key, Comparator>::Node {
  explicit Node(const Key& k) : key(k) {}

  Key const key;

  // Accessors/mutators for links.  Wrapped in methods so we can
  // add the appropriate barriers as necessary.
  Node* Next(int n) {
    assert(n >= 0);
    // Use an 'acquire load' so that we observe a fully initialized
    // version of the returned Node.
    return next_[n].load(std::memory_order_acquire);
  }
  void SetNext(int n, Node* x) {
    assert(n >= 0);
    // Use a 'release store' so that anybody who reads through this
    // pointer observes a fully initialized version of the inserted node.
    next_[n].store(x, std::memory_order_release);
  }

  // No-barrier variants that can be safely used in a few locations.
  Node* NoBarrier_Next(int n) {
    assert(n >= 0);
    return next_[n].load(std::memory_order_relaxed);
  }
  void NoBarrier_SetNext(int n, Node* x) {
    assert(n >= 0);
    next_[n].store(x, std::memory_order_relaxed);
  }

 private:
  // Array of length equal to the node height.  next_[0] is lowest level link.
  std::atomic<Node*> next_[1];
};

template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::NewNode(
    const Key& key, int height) {
  char* const node_memory = arena_->AllocateAligned(
      sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1));
  return new (node_memory) Node(key);
}

template <typename Key, class Comparator>
inline SkipList<Key, Comparator>::Iterator::Iterator(const SkipList* list) {
  list_ = list;
  node_ = nullptr;
}

template <typename Key, class Comparator>
inline bool SkipList<Key, Comparator>::Iterator::Valid() const {
  return node_ != nullptr;
}

template <typename Key, class Comparator>
inline const Key& SkipList<Key, Comparator>::Iterator::key() const {
  assert(Valid());
  return node_->key;
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Next() {
  assert(Valid());
  node_ = node_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Prev() {
  // Instead of using explicit "prev" links, we just search for the
  // last node that falls before key.
  assert(Valid());
  node_ = list_->FindLessThan(node_->key);
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::Seek(const Key& target) {
  node_ = list_->FindGreaterOrEqual(target, nullptr);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToFirst() {
  node_ = list_->head_->Next(0);
}

template <typename Key, class Comparator>
inline void SkipList<Key, Comparator>::Iterator::SeekToLast() {
  node_ = list_->FindLast();
  if (node_ == list_->head_) {
    node_ = nullptr;
  }
}

template <typename Key, class Comparator>
int SkipList<Key, Comparator>::RandomHeight() {
  // Increase height with probability 1 in kBranching
  static const unsigned int kBranching = 4;
  int height = 1;
  while (height < kMaxHeight && rnd_.OneIn(kBranching)) {
    height++;
  }
  assert(height > 0);
  assert(height <= kMaxHeight);
  return height;
}

// 判断 key 是否大于 n->key
template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::KeyIsAfterNode(const Key& key, Node* n) const {
  // null n is considered infinite
  return (n != nullptr) && (compare_(n->key, key) < 0);
}

// 寻找并返回大于等于 key 的第一个节点 x, 并将 x 在各层的前驱节点存储在 prev 数组中
// 
// 示例：
//  Lv2: 1 ---------------------------> 13
//                           
//  Lv1: 1 ------> 5 ------> 9 -------> 13
//
//  Lv0: 1 -> 3 -> 5 -> 7 -> 9 -> 11 -> 13
//
// 在上图的跳表中 FindGreaterOrEqual(8) 返回节点 9, prev 为节点 9 在各层的前驱节点 [1, 5, 7]
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindGreaterOrEqual(const Key& key,
                                              Node** prev) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (KeyIsAfterNode(key, next)) {
      // Keep searching in this list
      // 在当前层级中继续寻找
      x = next;
    } else {
      // next > key, 准备去下一个层级寻找
      if (prev != nullptr) prev[level] = x; // 将当前层级的前驱节点存在 prev 中
      if (level == 0) {
        return next;
      } else {
        // Switch to next list
        // 去下一个层级
        level--;
      }
    }
  }
}

// 返回小于 key 的最后一个节点
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node*
SkipList<Key, Comparator>::FindLessThan(const Key& key) const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    assert(x == head_ || compare_(x->key, key) < 0);
    Node* next = x->Next(level);
    if (next == nullptr || compare_(next->key, key) >= 0) {
      if (level == 0) {
        return x;
      } else {
        // Switch to next list
        level--;
      }
    } else {
      x = next;
    }
  }
}

// 找到跳表的最后一个节点
template <typename Key, class Comparator>
typename SkipList<Key, Comparator>::Node* SkipList<Key, Comparator>::FindLast()
    const {
  Node* x = head_;
  int level = GetMaxHeight() - 1;
  while (true) {
    Node* next = x->Next(level);
    if (next == nullptr) {
      if (level == 0) {
        return x;
      } else {
        // Switch to next list
        level--;
      }
    } else {
      x = next;
    }
  }
}

// 在 arena 上创建一个新的跳表，cmp 用于指定 key 的排序
template <typename Key, class Comparator>
SkipList<Key, Comparator>::SkipList(Comparator cmp, Arena* arena)
    : compare_(cmp),
      arena_(arena),
      head_(NewNode(0 /* any key will do */, kMaxHeight)),
      max_height_(1),
      rnd_(0xdeadbeef) {
  for (int i = 0; i < kMaxHeight; i++) {
    head_->SetNext(i, nullptr);
  }
}

template <typename Key, class Comparator>
void SkipList<Key, Comparator>::Insert(const Key& key) {
  // TODO(opt): We can use a barrier-free variant of FindGreaterOrEqual()
  // here since Insert() is externally synchronized.
  // TODO: 因为 Insert() 函数是有锁的情况下被调用的，所以 FindGreaterOrEqual 可以使用一个无锁版本
  // FindGreaterOrEqual 寻找并返回大于等于 key 的第一个节点 x, 并将 x 在各层的前驱节点存储在 prev 数组中
  //  使用 FindGreaterOrEqual 返回的 prev 数组作为各层插入时的前驱节点
  Node* prev[kMaxHeight];
  Node* x = FindGreaterOrEqual(key, prev);

  // Our data structure does not allow duplicate insertion
  // 我们的数据结构不允许重复插入，不过参数 key 中包含全局唯一的 InternalKey 所以实际上不会重复
  assert(x == nullptr || !Equal(key, x->key));

  int height = RandomHeight();
  if (height > GetMaxHeight()) {
    // 如果新节点的高度超过了原来的最大高度，新增的层数以 head 作为前驱
    for (int i = GetMaxHeight(); i < height; i++) {
      prev[i] = head_;
    }
    // It is ok to mutate max_height_ without any synchronization
    // with concurrent readers.  A concurrent reader that observes
    // the new value of max_height_ will see either the old value of
    // new level pointers from head_ (nullptr), or a new value set in
    // the loop below.  In the former case the reader will
    // immediately drop to the next level since nullptr sorts after all
    // keys.  In the latter case the reader will use the new node.
    // 修改 max_height
    // 在没有同步机制保护的情况下修改 max_height_ 是安全的
    // 其它线程看到新的 height 之后，可能看到新层级上的指针也可能看不到新层级的上指针
    // 如果能够看到，那么直接使用即可
    // 如果看不到新层级上的指针，即新层级上是 nullptr, 会立即下降到下一层
    // 总之，在写入的同时并发读是安全的，不需要锁
    max_height_.store(height, std::memory_order_relaxed);
  }

  x = NewNode(key, height);
  for (int i = 0; i < height; i++) {
    // NoBarrier_SetNext() suffices since we will add a barrier when
    // we publish a pointer to "x" in prev[i].
    // 新节点 x 此时还没有加入链表，无法被访问到，所以在无锁的情况下 SetNext 是安全的
    x->NoBarrier_SetNext(i, prev[i]->NoBarrier_Next(i));
    // 把 x 加入到链表中就需要锁了
    prev[i]->SetNext(i, x);
  }
}

// 判断跳表中是否包含指定 key
template <typename Key, class Comparator>
bool SkipList<Key, Comparator>::Contains(const Key& key) const {
  Node* x = FindGreaterOrEqual(key, nullptr);
  if (x != nullptr && Equal(key, x->key)) {
    return true;
  } else {
    return false;
  }
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_SKIPLIST_H_
