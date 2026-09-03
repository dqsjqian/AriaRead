/// @file elements_single.cpp
/// @brief ElementsSingle 索引切片系统实现 —— 移植自 legado

#include "openread/elements_single.h"
#include <algorithm>
#include <sstream>
#include <regex>

namespace openread {

std::vector<int> ElementsSingle::parseAndFilter(const std::string& rule, int elementsSize) {
    findIndexSet(rule);

    std::vector<int> indexSet;
    int lastIndexes = indexDefault_.empty() ? (indexes_.size() - 1) : (indexDefault_.size() - 1);

    if (indexes_.empty()) {
        // 非 [] 式索引，逆向遍历插入
        for (int ix = lastIndexes; ix >= 0; --ix) {
            int it = indexDefault_[ix];
            if (it >= 0 && it < elementsSize) {
                indexSet.push_back(it);
            } else if (it < 0 && elementsSize >= -it) {
                indexSet.push_back(it + elementsSize);
            }
        }
    } else {
        // [] 式索引，逆向遍历插入
        for (int ix = lastIndexes; ix >= 0; --ix) {
            if (std::holds_alternative<std::tuple<int, int, int>>(indexes_[ix])) {
                // 区间
                auto [startX, endX, stepX] = std::get<std::tuple<int, int, int>>(indexes_[ix]);

                int start = startX;
                if (start < 0) start += elementsSize;

                int end = endX;
                if (end < 0) end += elementsSize;

                if ((start < 0 && end < 0) || (start >= elementsSize && end >= elementsSize)) {
                    continue;
                }

                if (start >= elementsSize) start = elementsSize - 1;
                else if (start < 0) start = 0;

                if (end >= elementsSize) end = elementsSize - 1;
                else if (end < 0) end = 0;

                if (start == end || stepX >= elementsSize) {
                    indexSet.push_back(start);
                    continue;
                }

                int step = stepX > 0 ? stepX : (-stepX < elementsSize ? stepX + elementsSize : 1);

                if (end > start) {
                    for (int i = start; i <= end; i += step) {
                        indexSet.push_back(i);
                    }
                } else {
                    for (int i = start; i >= end; i -= step) {
                        indexSet.push_back(i);
                    }
                }
            } else {
                // 单个索引
                int it = std::get<int>(indexes_[ix]);
                if (it >= 0 && it < elementsSize) {
                    indexSet.push_back(it);
                } else if (it < 0 && elementsSize >= -it) {
                    indexSet.push_back(it + elementsSize);
                }
            }
        }
    }

    // 去重
    std::sort(indexSet.begin(), indexSet.end());
    indexSet.erase(std::unique(indexSet.begin(), indexSet.end()), indexSet.end());

    return indexSet;
}

void ElementsSingle::findIndexSet(const std::string& rule) {
    std::string rus = rule;
    // 去除首尾空白
    size_t start = rus.find_first_not_of(" \t\r\n");
    size_t end = rus.find_last_not_of(" \t\r\n");
    if (start != std::string::npos) {
        rus = rus.substr(start, end - start + 1);
    } else {
        rus.clear();
    }

    if (rus.empty()) return;

    int len = rus.size();
    bool head = (rus.back() == ']');

    if (head) {
        // 常规索引写法 [index...]
        len--;
        std::string l;
        bool curMinus = false;
        std::vector<int> curList;

        while (len-- >= 0) {
            if (len < 0) break;  // 防御：len-- 可能使 len 变为 -1，避免 rus[-1] 越界 UB
            char rl = rus[len];
            if (rl == ' ') continue;

            if (rl >= '0' && rl <= '9') {
                l = rl + l;
            } else if (rl == '-') {
                curMinus = true;
            } else {
                int curInt = l.empty() ? 0 : (curMinus ? -std::stoi(l) : std::stoi(l));

                if (rl == ':') {
                    curList.push_back(curInt);
                } else {
                    if (curList.empty()) {
                        if (l.empty()) break;  // 是 jsoup 选择器而非索引列表
                        indexes_.push_back(curInt);
                    } else {
                        // 列表最后压入的是区间右端，若列表有两位则最先压入的是间隔
                        int step = curList.size() == 2 ? curList[0] : 1;
                        indexes_.push_back(std::make_tuple(curInt, curList.back(), step));
                        curList.clear();
                    }

                    if (rl == '!') {
                        split_ = '!';
                        while (len > 0 && rus[--len] == ' ') {}
                    }

                    if (rl == '[') {
                        beforeRule_ = rus.substr(0, len);
                        return;
                    }

                    if (rl != ',') break;
                }

                l.clear();
                curMinus = false;
            }
        }
    } else {
        // 阅读原本写法，逆向遍历
        std::string l;
        bool curMinus = false;

        while (len-- >= 0) {
            if (len < 0) break;  // 防御：len-- 可能使 len 变为 -1，避免 rus[-1] 越界 UB
            char rl = rus[len];
            if (rl == ' ') continue;

            if (rl >= '0' && rl <= '9') {
                l = rl + l;
            } else if (rl == '-') {
                curMinus = true;
            } else {
                if (rl == '!' || rl == '.' || rl == ':') {
                    int curInt = l.empty() ? 0 : (curMinus ? -std::stoi(l) : std::stoi(l));
                    indexDefault_.push_back(curInt);

                    if (rl == '!') {
                        split_ = '!';
                        while (len > 0 && rus[--len] == ' ') {}
                    }

                    if (rl == '.' || rl == '!') {
                        beforeRule_ = rus.substr(0, len + 1);
                        return;
                    }
                }

                l.clear();
                curMinus = false;
            }
        }

        // 整个字符串都是数字
        if (!l.empty()) {
            indexDefault_.push_back(curMinus ? -std::stoi(l) : std::stoi(l));
        }
    }
}

std::optional<int> ElementsSingle::parseIndex(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try {
        return std::stoi(s);
    } catch (...) {
        return std::nullopt;
    }
}

std::tuple<int, int, int> ElementsSingle::parseRange(const std::string& s) {
    // 解析 "start:end:step" 格式
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string part;
    while (std::getline(iss, part, ':')) {
        parts.push_back(part);
    }

    int start = 0, end = -1, step = 1;
    if (parts.size() >= 1 && !parts[0].empty()) {
        start = std::stoi(parts[0]);
    }
    if (parts.size() >= 2 && !parts[1].empty()) {
        end = std::stoi(parts[1]);
    }
    if (parts.size() >= 3 && !parts[2].empty()) {
        step = std::stoi(parts[2]);
    }

    return std::make_tuple(start, end, step);
}

} // namespace openread
