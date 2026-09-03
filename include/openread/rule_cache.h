#pragma once
/// @file rule_cache.h
/// @brief 规则缓存 —— LRU 缓存规则解析结果、正则编译、JS 脚本编译

#include <string>
#include <vector>
#include <unordered_map>
#include <list>
#include <memory>
#include <regex>
#include <mutex>

namespace openread {

/// LRU 缓存模板类
template<typename Key, typename Value>
class LRUCache {
public:
    explicit LRUCache(size_t capacity) : capacity_(capacity) {}

    /// 获取缓存值
    /// @param key 键
    /// @param value 值（输出参数）
    /// @return 是否命中缓存
    bool get(const Key& key, Value& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            return false;
        }
        // 移动到链表头部
        lru_.splice(lru_.begin(), lru_, it->second);
        value = it->second->second;
        return true;
    }

    /// 设置缓存值
    /// @param key 键
    /// @param value 值
    void put(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            // 更新值并移动到链表头部
            it->second->second = value;
            lru_.splice(lru_.begin(), lru_, it->second);
            return;
        }
        // 如果缓存已满，删除链表尾部元素
        if (cache_.size() >= capacity_) {
            auto last = lru_.back();
            cache_.erase(last.first);
            lru_.pop_back();
        }
        // 插入新元素到链表头部
        lru_.emplace_front(key, value);
        cache_[key] = lru_.begin();
    }

    /// 清空缓存
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        lru_.clear();
    }

    /// 获取缓存大小
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

private:
    size_t capacity_;
    std::list<std::pair<Key, Value>> lru_;
    std::unordered_map<Key, typename std::list<std::pair<Key, Value>>::iterator> cache_;
    mutable std::mutex mutex_;
};

/// 规则缓存管理器
class RuleCacheManager {
public:
    /// 获取单例实例
    static RuleCacheManager& getInstance() {
        static RuleCacheManager instance;
        return instance;
    }

    /// 获取规则解析缓存
    LRUCache<std::string, std::vector<std::string>>& getRuleCache() {
        return ruleCache_;
    }

    /// 获取正则编译缓存
    LRUCache<std::string, std::shared_ptr<std::regex>>& getRegexCache() {
        return regexCache_;
    }

    /// 获取 JS 脚本编译缓存
    LRUCache<std::string, std::string>& getScriptCache() {
        return scriptCache_;
    }

    /// 清空所有缓存
    void clearAll() {
        ruleCache_.clear();
        regexCache_.clear();
        scriptCache_.clear();
    }

private:
    RuleCacheManager()
        : ruleCache_(1000),    // 规则缓存容量 1000
          regexCache_(500),    // 正则缓存容量 500
          scriptCache_(200) {} // 脚本缓存容量 200

    LRUCache<std::string, std::vector<std::string>> ruleCache_;
    LRUCache<std::string, std::shared_ptr<std::regex>> regexCache_;
    LRUCache<std::string, std::string> scriptCache_;
};

} // namespace openread
