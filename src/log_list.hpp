#pragma once

#include <algorithm>

namespace QuestBook {

inline constexpr int ViewUnits = 8192;
inline constexpr int WheelLines = 3;

struct ListPosition {
    int top;
    int selected;
};

// ZenGin's list reserves one font line for its scroll arrow.
inline int VisibleRows(int fontHeight) {
    return fontHeight > 0 ? (ViewUnits - 1) / fontHeight : 0;
}

inline int ClickedRow(int localY, int fontHeight, int top, int count) {
    const int rows = VisibleRows(fontHeight);
    if (rows == 0 || localY < 0 || top < 0 || top >= count) return -1;
    const int row = localY / fontHeight;
    return row < rows && row < count - top ? top + row : -1;
}

inline ListPosition ScrollList(ListPosition position, int count, int rows, int lines) {
    if (count <= 0 || rows <= 0) return position;
    const int lastTop = count > rows ? count - rows : 0;
    const int top = static_cast<int>(std::clamp(static_cast<long long>(position.top) + lines,
                                                0LL, static_cast<long long>(lastTop)));
    const int lastVisible = (std::min)(count - 1, top + rows - 1);
    return {top, std::clamp(position.selected, top, lastVisible)};
}

} // namespace QuestBook
