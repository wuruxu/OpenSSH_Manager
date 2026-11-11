#pragma once
#include <algorithm>
#include <cstdint>
namespace layoutmath {
constexpr int designWidth = 1020, designHeight = 760;
struct Viewport {
    int width = designWidth, height = designHeight;
    static int map(int value, int extent, int design) {
        return static_cast<int>((static_cast<int64_t>(value) * extent + design / 2) / design);
    }
    int x(int value) const { return map(value, width, designWidth); }
    int y(int value) const { return map(value, height, designHeight); }
    int fontDpi() const { return (std::max)(1, (std::min)(map(96, width, designWidth), map(96, height, designHeight))); }
};
}
