#include "../src/log_list.hpp"

#include <cstdlib>

namespace {
void Check(bool condition) {
    if (!condition) std::abort();
}
}

int main() {
    using namespace QuestBook;
    Check(VisibleRows(0) == 0);
    Check(VisibleRows(0x400) == 7);
    Check(VisibleRows(800) == 10);
    Check(ClickedRow(0, 800, 5, 30) == 5);
    Check(ClickedRow(800, 800, 5, 30) == 6);
    Check(ClickedRow(7999, 800, 5, 30) == 14);
    Check(ClickedRow(8000, 800, 5, 30) == -1);
    Check(ClickedRow(0, 800, 5, 5) == -1);
    Check(ClickedRow(2400, 800, 5, 7) == -1);

    const auto within = ScrollList({0, 8}, 30, 10, 3);
    Check(within.top == 3 && within.selected == 8);
    const auto down = ScrollList({3, 3}, 30, 10, 3);
    Check(down.top == 6 && down.selected == 6);
    const auto last = ScrollList(down, 30, 10, 50);
    Check(last.top == 20 && last.selected == 20);
    const auto first = ScrollList(last, 30, 10, -50);
    Check(first.top == 0 && first.selected == 9);
    const auto shortList = ScrollList({0, 1}, 3, 10, 30);
    Check(shortList.top == 0 && shortList.selected == 1);
}
