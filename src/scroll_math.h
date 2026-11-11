#pragma once
#include <algorithm>
#include <cstdint>

namespace scrollmath {
struct Range {
    int minimum = 0, maximum = 0;
    unsigned page = 0;
    int position = 0;
    int last() const {
        const auto value = static_cast<int64_t>(maximum) - (page ? static_cast<int64_t>(page) - 1 : 0);
        return static_cast<int>((std::max)(static_cast<int64_t>(minimum), value));
    }
    int clamp(int64_t value) const { return static_cast<int>((std::clamp)(value, static_cast<int64_t>(minimum), static_cast<int64_t>(last()))); }
    bool scrollable() const { return last() > minimum; }
};
struct Thumb { int start = 0, length = 0, travel = 0; };
inline Thumb thumb(const Range& range, int track, int minimumLength) {
    if (track <= 0 || !range.scrollable()) return {};
    const int64_t extent = (std::max)(int64_t(1), static_cast<int64_t>(range.maximum) - range.minimum + 1);
    int length = static_cast<int>((std::clamp)(static_cast<int64_t>(track) * range.page / extent,
        static_cast<int64_t>((std::min)(track, (std::max)(1, minimumLength))), static_cast<int64_t>(track)));
    int travel = track - length;
    int start = static_cast<int>(static_cast<int64_t>(travel) * (range.clamp(range.position) - static_cast<int64_t>(range.minimum)) /
        (range.last() - static_cast<int64_t>(range.minimum)));
    return {start, length, travel};
}
inline int drag(const Range& range, const Thumb& thumb, int pixel) {
    if (!thumb.travel) return range.clamp(range.position);
    auto point = (std::clamp)(pixel, 0, thumb.travel);
    return range.clamp(static_cast<int64_t>(range.minimum) +
        (static_cast<int64_t>(point) * (range.last() - static_cast<int64_t>(range.minimum)) + thumb.travel / 2) / thumb.travel);
}
}
