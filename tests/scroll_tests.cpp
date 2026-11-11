#include "scroll_math.h"
#include <climits>
#include <iostream>
#include <stdexcept>
using namespace scrollmath;
static void check(bool result, const char* message) { if (!result) throw std::runtime_error(message); }
int main() {
    try {
        Range empty{0, 99, 100, 0};
        check(!empty.scrollable() && thumb(empty, 300, 24).length == 0, "Non-overflowing content has a thumb");
        Range page{0, 759, 400, 360};
        check(page.last() == 360 && page.clamp(10000) == 360 && page.clamp(-4) == 0, "Page-size clamping is wrong");
        auto end = thumb(page, 400, 24);
        check(end.start + end.length == 400, "Thumb does not reach the track end");
        check(drag(page, end, 10000) == 360 && drag(page, end, -100) == 0, "Out-of-track dragging is not clamped");
        Range large{0, 1000000, 500, 700000};
        auto geometry = thumb(large, 300, 24);
        check(geometry.length == 24, "Tiny proportional thumb violates minimum size");
        check(drag(large, geometry, geometry.travel) == large.last(), "Dragging truncates positions above 65535");
        Range extreme{0, INT_MAX, 1, INT_MAX};
        auto extremeThumb = thumb(extreme, 2000, 24);
        check(extremeThumb.start + extremeThumb.length == 2000 && drag(extreme, extremeThumb, extremeThumb.travel) == INT_MAX, "Scroll arithmetic overflow");
        Range offset{50, 1049, 100, 450};
        check(offset.last() == 950 && drag(offset, thumb(offset, 240, 24), 0) == 50, "Nonzero minimum lost");
        check(thumb(large, 0, 24).length == 0 && thumb(large, 8, 24).length == 8, "Short/zero track is invalid");
        for (int pixel = 0, previous = 0; pixel <= geometry.travel; ++pixel) {
            int value = drag(large, geometry, pixel);
            check(value >= previous && value <= large.last(), "Drag is not monotonic"); previous = value;
        }
        std::cout << "PASS: capsule thumb geometry, page bounds, short tracks and full-width drag positions.\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
