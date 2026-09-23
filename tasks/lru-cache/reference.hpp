#pragma once

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>

template <typename Key, typename Value>
class LruCache {
 public:
  explicit LruCache(std::size_t capacity) : capacity_(capacity) {}

  std::optional<Value> get(const Key& key) {
    auto it = index_.find(key);
    if (it == index_.end()) return std::nullopt;
    items_.splice(items_.begin(), items_, it->second);
    return it->second->second;
  }

  void put(const Key& key, Value value) {
    if (capacity_ == 0) return;
    if (auto it = index_.find(key); it != index_.end()) {
      it->second->second = std::move(value);
      items_.splice(items_.begin(), items_, it->second);
      return;
    }
    if (items_.size() == capacity_) {
      index_.erase(items_.back().first);
      items_.pop_back();
    }
    items_.emplace_front(key, std::move(value));
    index_.emplace(key, items_.begin());
  }

  bool contains(const Key& key) const { return index_.count(key) != 0; }
  std::size_t size() const { return items_.size(); }
  std::size_t capacity() const { return capacity_; }

 private:
  using List = std::list<std::pair<Key, Value>>;
  std::size_t capacity_;
  List items_;
  std::unordered_map<Key, typename List::iterator> index_;
};
