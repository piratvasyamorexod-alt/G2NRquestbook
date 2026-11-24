#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace QuestBook {

struct Activity {
    std::uint64_t order = 0;
    bool unread = false;
};

class Recency {
    std::uint64_t counter = 0;
    std::unordered_map<const void*, Activity> topics;

public:
    void Record(const void* topic, int before, int after) {
        if (after > before) topics[topic] = {++counter, true};
    }

    Activity Get(const void* topic) const {
        const auto item = topics.find(topic);
        return item == topics.end() ? Activity{} : item->second;
    }

    void Read(const void* topic) {
        const auto item = topics.find(topic);
        if (item != topics.end()) item->second.unread = false;
    }

    void Clear() {
        topics.clear();
        counter = 0;
    }
};

template<class Topic>
void MostRecentFirst(std::vector<Topic>& topics) {
    std::stable_sort(topics.begin(), topics.end(), [](const Topic& a, const Topic& b) {
        return a.activity.order > b.activity.order;
    });
}

} // namespace QuestBook
