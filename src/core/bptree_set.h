// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <optional>

#include "base/pmr/memory_resource.h"
#include "core/detail/bptree_internal.h"

namespace dfly {

template <typename T> struct DefaultCompareTo {
  int operator()(const T& a, const T& b) const {
    std::less<T> cmp;
    return cmp(a, b) ? -1 : (cmp(b, a) ? 1 : 0);
  }
};

template <typename T> struct BPTreePolicy {
  using KeyT = T;
  using KeyCompareTo = DefaultCompareTo<T>;
};

template <typename T, typename Policy = BPTreePolicy<T>> class BPTree {
  BPTree(const BPTree&) = delete;
  BPTree& operator=(const BPTree&) = delete;

  using BPTreeNode = detail::BPTreeNode<T>;
  using BPTreePath = detail::BPTreePath<T>;

 public:
  using KeyT = typename Policy::KeyT;

  BPTree(PMR_NS::memory_resource* mr = PMR_NS::get_default_resource()) : mr_(mr) {
  }

  ~BPTree() {
    Clear();
  }

  // true if inserted, false if skipped.
  bool Insert(KeyT item);

  bool Contains(KeyT item) const;

  bool Delete(KeyT item);

  std::optional<uint32_t> GetRank(KeyT item) const;

  size_t Height() const {
    return height_;
  }

  size_t Size() const {
    return count_;  // number of items in the tree
  }

  size_t NodeCount() const {
    // number of nodes in the tree (usually, order of magnitude smaller than Size()).
    return num_nodes_;
  }

  void Clear();

  BPTreeNode* DEBUG_root() {
    return root_;
  }

 private:
  BPTreeNode* CreateNode(bool leaf);

  void DestroyNode(BPTreeNode* node);

  void InsertToFullLeaf(KeyT item, const BPTreePath& path);

  // Returns true if insertion was handled by rebalancing.
  bool RebalanceLeafAndInsert(const BPTreePath& path, unsigned parent_depth, KeyT item,
                              unsigned insert_pos);

  void IncreaseSubtreeCounts(const BPTreePath& path, unsigned depth, int32_t delta);

  // Charts the path towards key. Returns true if key is found.
  // In that case path->Last().first->Key(path->Last().second) == key.
  // Fills the tree path not including the key itself.
  bool Locate(KeyT key, BPTreePath* path) const;

  BPTreeNode* root_ = nullptr;  // root node or NULL if empty tree
  uint32_t count_ = 0;          // number of items in tree
  uint32_t height_ = 0;         // height of tree from root to leaf
  uint32_t num_nodes_ = 0;      // number of nodes in tree
  PMR_NS::memory_resource* mr_;
};

template <typename T, typename Policy> bool BPTree<T, Policy>::Contains(KeyT item) const {
  BPTreePath path;
  bool found = Locate(item, &path);
  return found;
}

template <typename T, typename Policy> void BPTree<T, Policy>::Clear() {
  if (!root_)
    return;

  BPTreePath path;
  BPTreeNode* node = root_;

  auto deep_left = [&](unsigned pos) {
    do {
      path.Push(node, pos);
      node = node->Child(pos);
      pos = 0;
    } while (!node->IsLeaf());
  };

  if (!root_->IsLeaf())
    deep_left(0);

  while (true) {
    DestroyNode(node);

    if (path.Depth() == 0) {
      break;
    }
    node = path.Last().first;
    unsigned pos = path.Last().second;
    path.Pop();
    if (pos < node->NumItems()) {
      deep_left(pos + 1);
    }
  }
  root_ = nullptr;
  height_ = count_ = 0;
}

template <typename T, typename Policy> bool BPTree<T, Policy>::Insert(KeyT item) {
  if (!root_) {
    root_ = CreateNode(true);
    root_->InitSingle(item);
    count_ = height_ = 1;

    return true;
  }

  BPTreePath path;
  bool found = Locate(item, &path);

  if (found) {
    return false;
  }

  assert(path.Depth() > 0u);

  BPTreeNode* leaf = path.Last().first;
  assert(leaf->IsLeaf());

  if (leaf->NumItems() == detail::BPNodeLayout<T>::kMaxLeafKeys) {
    InsertToFullLeaf(item, path);
  } else {
    unsigned pos = path.Last().second;
    leaf->LeafInsert(pos, item);
    if (path.Depth() > 1)
      IncreaseSubtreeCounts(path, path.Depth() - 2, 1);
  }
  count_++;
  return true;
}

template <typename T, typename Policy> bool BPTree<T, Policy>::Delete(KeyT item) {
  if (!root_)
    return false;

  BPTreePath path;
  bool found = Locate(item, &path);
  if (!found)
    return false;

  BPTreeNode* node = path.Last().first;
  unsigned key_pos = path.Last().second;

  // Remove the key from the node.
  if (node->IsLeaf()) {
    node->ShiftLeft(key_pos);  // shift left everything after key_pos.
  } else {
    // We can not remove the item from the inner node because it also serves as a separator.
    // Therefore, we swap it the rightmost key in the left subtree and pop from there instead.
    path.DigRight();

    BPTreeNode* leaf = path.Last().first;
    // set a new separator.
    node->SetKey(key_pos, leaf->Key(leaf->NumItems() - 1));
    leaf->LeafEraseRight();  // pop the rightmost key from the leaf.
    node = leaf;
  }
  count_--;

  assert(node->IsLeaf());

  // go up the tree and rebalance if number of items in the node is less
  // than low limit. We either merge or rebalance nodes.
  while (node->NumItems() < node->MinItems()) {
    if (node == root_) {
      if (node->NumItems() == 0) {
        // terminal case, we reached the root - and it has either a single child (0 delimiters)
        // or no children at all (leaf). The former is more common case: the tree can only shrink
        // through the root.
        if (node->IsLeaf()) {
          assert(count_ == 0u);
          root_ = nullptr;
        } else {
          root_ = root_->Child(0);
        }
        --height_;
        DestroyNode(node);
      }
      return true;
    }

    // The node has a parent. Pop the node from the path and try rebalance it via its parent.
    assert(path.Depth() > 0u);
    path.Pop();

    BPTreeNode* parent = path.Last().first;
    unsigned pos = path.Last().second;
    assert(parent->Child(pos) == node);
    node = parent->MergeOrRebalanceChild(pos);

    parent->IncreaseTreeCount(-1);

    if (node == nullptr)  // succeeded to merge/rebalance without the need to propagate.
      break;

    DestroyNode(node);

    // assert(parent->TreeCount() == parent->DEBUG_TreeCount());
    node = parent;
  }

  if (path.Depth() >= 2) {
    IncreaseSubtreeCounts(path, path.Depth() - 2, -1);
  }
  return true;
}

template <typename T, typename Policy>
std::optional<uint32_t> BPTree<T, Policy>::GetRank(KeyT item) const {
  if (!root_)
    return std::nullopt;

  BPTreePath path;
  bool found = Locate(item, &path);
  if (!found)
    return std::nullopt;

  uint32_t rank = 0;
  unsigned bound = path.Depth();

  for (unsigned i = 0; i < bound; ++i) {
    BPTreeNode* node = path.Node(i);
    unsigned pos = path.Position(i);
    if (!node->IsLeaf()) {
      unsigned delta = (i == bound - 1) ? 1 : 0;
      for (unsigned j = 0; j < pos + delta; ++j) {
        rank += node->Child(j)->TreeCount();
      }
    }
    rank += pos;
  }
  return rank;
}

template <typename T, typename Policy>
bool BPTree<T, Policy>::Locate(KeyT key, BPTreePath* path) const {
  assert(root_);
  BPTreeNode* node = root_;
  typename Policy::KeyCompareTo cmp;
  while (true) {
    typename BPTreeNode::SearchResult res = node->BSearch(key, cmp);
    path->Push(node, res.index);
    if (res.found) {
      return true;
    }
    assert(res.index <= node->NumItems());

    if (node->IsLeaf()) {
      break;
    }
    node = node->Child(res.index);
  }
  return false;
}

template <typename T, typename Policy>
void BPTree<T, Policy>::InsertToFullLeaf(KeyT item, const BPTreePath& path) {
  using Layout = detail::BPNodeLayout<T>;
  using Comp [[maybe_unused]] = typename Policy::KeyCompareTo;

  assert(path.Depth() > 0u);

  BPTreeNode* node = path.Last().first;
  assert(node->IsLeaf() && node->AvailableSlotCount() == 0);

  unsigned insert_pos = path.Last().second;
  unsigned level = path.Depth() - 1;
  if (level > 0 && RebalanceLeafAndInsert(path, level - 1, item, insert_pos)) {
    // Update the tree count of the ascendants.
    IncreaseSubtreeCounts(path, level - 1, 1);
    return;
  }

  KeyT median;
  BPTreeNode* right = CreateNode(true);
  node->Split(right, &median);

  assert(node->NumItems() < Layout::kMaxLeafKeys);

  if (insert_pos <= node->NumItems()) {
    assert(Comp()(item, median) == -1);
    node->LeafInsert(insert_pos, item);
  } else {
    assert(Comp()(item, median) == 1);
    right->LeafInsert(insert_pos - node->NumItems() - 1, item);
  }

  // we must add the newly created `right` to the parent and update its tree count.
  while (level > 0) {
    --level;
    // level up, now node is parent.
    node = path.Node(level);
    unsigned pos = path.Position(level);  // position of the child node in parent.

    assert(!node->IsLeaf() && pos <= node->NumItems());
    assert(right);

    // Terminal case: Node is not full so we can just add `right` to it.
    if (node->NumItems() < Layout::kMaxInnerKeys) {
      // We do not update the subtree count of the node here because the surpus of another item
      // resulted with the additional key in this node.
      node->InnerInsert(pos, median, right);
      node->IncreaseTreeCount(1);
      right = nullptr;
      break;
    }

    // We need to insert right into a node as position pos. Node is full so we must handle it
    // either via rebalancing "node" or via its splitting. Rebalancing is a better case, we try
    // it first.
    if (level > 0) {
      // see if we can rebalance node (right's parent) via node's parent.
      BPTreeNode* parent = path.Node(level - 1);
      unsigned parent_pos = path.Position(level - 1);
      assert(parent->Child(parent_pos) == node);

      auto [new_node, inner_pos] = parent->RebalanceChild(parent_pos, pos);
      if (new_node) {
        // we rebalanced inner_full so we can insert (median, right) and stop propagating.
        new_node->InnerInsert(inner_pos, median, right);

        if (new_node != node) {
          // Fix subtree counts if right was migrated to the sibling.
          node->IncreaseTreeCount(-right->TreeCount());
          new_node->IncreaseTreeCount(right->TreeCount() + 1);
        } else {
          node->IncreaseTreeCount(1);
        }
        right = nullptr;
        break;
      }
    }

    // node is not rebalanced, so we need to split it.
    BPTreeNode* next_right = CreateNode(false);
    KeyT next_median;
    node->Split(next_right, &next_median);
    assert(node->NumItems() < Layout::kMaxInnerKeys);

    if (pos <= node->NumItems()) {
      assert(Comp()(median, next_median) == -1);

      node->InnerInsert(pos, median, right);
      node->IncreaseTreeCount(1);
    } else {
      assert(Comp()(median, next_median) == 1);

      next_right->InnerInsert(pos - node->NumItems() - 1, median, right);

      // Fix tree counts.
      node->IncreaseTreeCount(-right->TreeCount());
      next_right->IncreaseTreeCount(right->TreeCount() + 1);
    }
    right = next_right;
    median = next_median;
  }

  if (right) {
    assert(level == 0);
    BPTreeNode* new_root = CreateNode(false);
    new_root->InitSingle(median);
    new_root->SetChild(0, root_);
    new_root->SetChild(1, right);
    new_root->SetTreeCount(root_->TreeCount() + right->TreeCount() + 1);
    root_ = new_root;
    height_++;
  } else {
    if (level > 0) {
      IncreaseSubtreeCounts(path, level - 1, 1);
    }
  }
}

template <typename T, typename Policy>
bool BPTree<T, Policy>::RebalanceLeafAndInsert(const BPTreePath& path, unsigned parent_depth,
                                               KeyT item, unsigned insert_pos) {
  BPTreeNode* parent = path.Node(parent_depth);
  unsigned pos = path.Position(parent_depth);

  std::pair<BPTreeNode*, unsigned> rebalance_res = parent->RebalanceChild(pos, insert_pos);
  if (rebalance_res.first) {
    rebalance_res.first->LeafInsert(rebalance_res.second, item);
    return true;
  }
  return false;
}

template <typename T, typename Policy>
void BPTree<T, Policy>::IncreaseSubtreeCounts(const BPTreePath& path, unsigned depth,
                                              int32_t delta) {
  for (int i = depth; i >= 0; --i) {
    BPTreeNode* node = path.Node(i);
    node->IncreaseTreeCount(delta);
  }
}

template <typename T, typename Policy>
detail::BPTreeNode<T>* BPTree<T, Policy>::CreateNode(bool leaf) {
  num_nodes_++;
  void* ptr = mr_->allocate(detail::kBPNodeSize, 8);
  BPTreeNode* node = new (ptr) BPTreeNode(leaf);

  return node;
}

template <typename T, typename Policy> void BPTree<T, Policy>::DestroyNode(BPTreeNode* node) {
  void* ptr = node;
  mr_->deallocate(ptr, detail::kBPNodeSize, 8);
  num_nodes_--;
}

}  // namespace dfly
