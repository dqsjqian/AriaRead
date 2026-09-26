/// @file rule_analyzer.cpp
/// @brief 规则解析器状态机实现 —— 平衡组、三路分隔符、引号保护
/// 移植自 legado 的 RuleAnalyzer.kt

#include "ariaread/rule_analyzer.h"
#include <algorithm>
#include <stdexcept>

namespace ariaread {

RuleAnalyzer::RuleAnalyzer(const std::string& data, bool code)
    : queue_(data), code_(code) {}

void RuleAnalyzer::reSetPos() {
    pos_ = 0;
    startX_ = 0;
}

void RuleAnalyzer::trim() {
    if (pos_ >= queue_.size()) return;
    char c = queue_[pos_];
    if (c == '@' || c < '!') {
        ++pos_;
        while (pos_ < queue_.size() && (queue_[pos_] == '@' || queue_[pos_] < '!')) {
            ++pos_;
        }
        start_ = pos_;
        startX_ = pos_;
    }
}

bool RuleAnalyzer::consumeTo(const std::string& seq) {
    start_ = pos_;
    size_t offset = queue_.find(seq, pos_);
    if (offset != std::string::npos) {
        pos_ = offset;
        return true;
    }
    return false;
}

bool RuleAnalyzer::consumeToAny(const std::vector<std::string>& seq) {
    size_t pos = pos_;
    while (pos != queue_.size()) {
        for (const auto& s : seq) {
            if (pos + s.size() <= queue_.size() &&
                queue_.compare(pos, s.size(), s) == 0) {
                step_ = static_cast<int>(s.size());
                pos_ = pos;
                return true;
            }
        }
        ++pos;
    }
    return false;
}

int RuleAnalyzer::findToAny(const std::vector<char>& seq) {
    size_t pos = pos_;
    while (pos != queue_.size()) {
        for (char s : seq) {
            if (queue_[pos] == s) {
                return static_cast<int>(pos);
            }
        }
        ++pos;
    }
    return -1;
}

bool RuleAnalyzer::chompCodeBalanced(char open, char close) {
    size_t pos = pos_;
    int depth = 0;
    int otherDepth = 0;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;

    do {
        if (pos == queue_.size()) break;
        char c = queue_[pos++];

        if (c != ESC) {
            if (c == '\'' && !inDoubleQuote) inSingleQuote = !inSingleQuote;
            else if (c == '"' && !inSingleQuote) inDoubleQuote = !inDoubleQuote;

            if (inSingleQuote || inDoubleQuote) continue;

            if (c == '[') depth++;
            else if (c == ']') depth--;
            else if (depth == 0) {
                if (c == open) otherDepth++;
                else if (c == close) otherDepth--;
            }
        } else {
            ++pos;
        }
    } while (depth > 0 || otherDepth > 0);

    if (depth > 0 || otherDepth > 0) {
        return false;
    }
    pos_ = pos;
    return true;
}

bool RuleAnalyzer::chompRuleBalanced(char open, char close) {
    size_t pos = pos_;
    int depth = 0;
    bool inSingleQuote = false;
    bool inDoubleQuote = false;

    do {
        if (pos == queue_.size()) break;
        char c = queue_[pos++];

        if (c == '\'' && !inDoubleQuote) inSingleQuote = !inSingleQuote;
        else if (c == '"' && !inSingleQuote) inDoubleQuote = !inDoubleQuote;

        if (inSingleQuote || inDoubleQuote) continue;
        else if (c == '\\') {
            ++pos;
            continue;
        }

        if (c == open) depth++;
        else if (c == close) depth--;
    } while (depth > 0);

    if (depth > 0) {
        return false;
    }
    pos_ = pos;
    return true;
}

std::vector<std::string> RuleAnalyzer::splitRule(const std::vector<std::string>& split) {
    rule_.clear();

    if (split.size() == 1) {
        elementsType_ = split[0];
        if (!consumeTo(elementsType_)) {
            rule_.push_back(queue_.substr(startX_));
            return rule_;
        }
        step_ = static_cast<int>(elementsType_.size());
        return splitRuleNext();
    }

    if (!consumeToAny(split)) {
        rule_.push_back(queue_.substr(startX_));
        return rule_;
    }

    size_t end = pos_;
    pos_ = start_;

    do {
        int st = findToAny({'[', '('});

        if (st == -1) {
            rule_.push_back(queue_.substr(startX_, end - startX_));
            elementsType_ = queue_.substr(end, step_);
            pos_ = end + step_;

            while (consumeTo(elementsType_)) {
                rule_.push_back(queue_.substr(start_, pos_ - start_));
                pos_ += step_;
            }

            rule_.push_back(queue_.substr(pos_));
            return rule_;
        }

        if (static_cast<size_t>(st) > end) {
            rule_.push_back(queue_.substr(startX_, end - startX_));
            elementsType_ = queue_.substr(end, step_);
            pos_ = end + step_;

            while (consumeTo(elementsType_) && pos_ < static_cast<size_t>(st)) {
                rule_.push_back(queue_.substr(start_, pos_ - start_));
                pos_ += step_;
            }

            if (pos_ > static_cast<size_t>(st)) {
                startX_ = start_;
                return splitRuleNext();
            } else {
                rule_.push_back(queue_.substr(pos_));
                return rule_;
            }
        }

        pos_ = st;
        char next = (queue_[pos_] == '[') ? ']' : ')';

        if (!chompBalanced(queue_[pos_], next)) {
            throw std::runtime_error(queue_.substr(0, start_) + "后未平衡");
        }
    } while (end > pos_);

    start_ = pos_;
    return splitRule(split);
}

std::vector<std::string> RuleAnalyzer::splitRuleNext() {
    size_t end = pos_;
    pos_ = start_;

    do {
        int st = findToAny({'[', '('});

        if (st == -1) {
            rule_.push_back(queue_.substr(startX_, end - startX_));
            pos_ = end + step_;

            while (consumeTo(elementsType_)) {
                rule_.push_back(queue_.substr(start_, pos_ - start_));
                pos_ += step_;
            }

            rule_.push_back(queue_.substr(pos_));
            return rule_;
        }

        if (static_cast<size_t>(st) > end) {
            rule_.push_back(queue_.substr(startX_, end - startX_));
            pos_ = end + step_;

            while (consumeTo(elementsType_) && pos_ < static_cast<size_t>(st)) {
                rule_.push_back(queue_.substr(start_, pos_ - start_));
                pos_ += step_;
            }

            if (pos_ > static_cast<size_t>(st)) {
                startX_ = start_;
                return splitRuleNext();
            } else {
                rule_.push_back(queue_.substr(pos_));
                return rule_;
            }
        }

        pos_ = st;
        char next = (queue_[pos_] == '[') ? ']' : ')';

        if (!chompBalanced(queue_[pos_], next)) {
            throw std::runtime_error(queue_.substr(0, start_) + "后未平衡");
        }
    } while (end > pos_);

    start_ = pos_;
    if (!consumeTo(elementsType_)) {
        rule_.push_back(queue_.substr(startX_));
        return rule_;
    }
    return splitRuleNext();
}

std::string RuleAnalyzer::innerRule(const std::string& inner,
                                     int startStep,
                                     int endStep,
                                     const std::function<std::string(const std::string&)>& fr) {
    std::string st;
    bool replaced = false;

    while (consumeTo(inner)) {
        size_t posPre = pos_;
        if (chompCodeBalanced('{', '}')) {
            std::string key = queue_.substr(posPre + startStep, pos_ - endStep - (posPre + startStep));
            std::string value = fr(key);
            if (!value.empty()) {
                if (!replaced) {
                    st = queue_.substr(startX_, posPre - startX_);
                    replaced = true;
                } else {
                    st += queue_.substr(startX_, posPre - startX_);
                }
                st += value;
                startX_ = pos_;
                continue;
            }
        }
        pos_ += inner.size();
    }

    if (!replaced) {
        return "";
    }
    st += queue_.substr(startX_);
    return st;
}

std::string RuleAnalyzer::innerRule(const std::string& startStr,
                                     const std::string& endStr,
                                     const std::function<std::string(const std::string&)>& fr) {
    std::string st;
    bool replaced = false;

    while (consumeTo(startStr)) {
        pos_ += startStr.size();
        size_t posPre = pos_;
        if (consumeTo(endStr)) {
            std::string key = queue_.substr(posPre, pos_ - posPre);
            std::string value = fr(key);
            if (!replaced) {
                st = queue_.substr(startX_, posPre - startStr.size() - startX_);
                replaced = true;
            } else {
                st += queue_.substr(startX_, posPre - startStr.size() - startX_);
            }
            st += value;
            pos_ += endStr.size();
            startX_ = pos_;
        }
    }

    if (!replaced) {
        return queue_;
    }
    st += queue_.substr(startX_);
    return st;
}

} // namespace ariaread
