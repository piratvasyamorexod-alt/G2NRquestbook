#include "../src/log_recency.hpp"

#include <cstdlib>
#include <vector>

using namespace QuestBook;

void Check(bool value) {
    if (!value) std::abort();
}

int main() {
    Recency recency;
    int original = 0, added = 0, completed = 0;
    recency.Record(&added, 0, 1);
    recency.Record(&original, 7, 8);
    recency.Record(&added, 1, 1); // Native duplicate rejection.
    recency.Record(&completed, 4, 5);

    struct Topic {
        const void* id;
        Activity activity;
    };
    std::vector<Topic> current{{&added, recency.Get(&added)}, {&original, recency.Get(&original)}};
    std::vector<Topic> done{{&completed, recency.Get(&completed)}};
    MostRecentFirst(current);
    Check(current[0].id == &original && current[0].activity.unread);
    Check(done[0].activity.order > current[0].activity.order);

    recency.Read(&original);
    Check(!recency.Get(&original).unread && recency.Get(&original).order == current[0].activity.order);
    recency.Record(&original, 8, 9);
    Check(recency.Get(&original).unread && recency.Get(&original).order > done[0].activity.order);

    recency.Clear();
    current = {{&added, recency.Get(&added)}, {&original, recency.Get(&original)}};
    MostRecentFirst(current);
    Check(current[0].id == &added && !current[0].activity.unread && current[0].activity.order == 0);
    Check(recency.Get(&completed).order == 0);
}
