#include "layout_math.h"
#include <iostream>
#include <stdexcept>
using namespace layoutmath;
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
int main() {
    try {
        for (Viewport v : {Viewport{640,480}, {740,580}, {1020,760}, {1530,1140}, {2040,1520}, {800,1200}, {1920,600}}) {
            check(v.x(0) == 0 && v.y(0) == 0 && v.x(designWidth) == v.width && v.y(designHeight) == v.height, "viewport boundaries changed");
            for (int x = 0; x < designWidth; ++x) check(v.x(x) <= v.x(x+1) && v.x(x+1) <= v.width, "horizontal scaling overflow");
            for (int y = 0; y < designHeight; ++y) check(v.y(y) <= v.y(y+1) && v.y(y+1) <= v.height, "vertical scaling overflow");
            check(v.x(992) <= v.width && v.y(744) <= v.height, "bottom/right controls outside client area");
            check(v.fontDpi() > 0, "invalid font scaling");
        }
        check(Viewport{1020,760}.fontDpi() == 96 && Viewport{2040,1520}.fontDpi() == 192, "font scale does not track window size");
        check(Viewport{1020,380}.fontDpi() == 48 && Viewport{510,760}.fontDpi() == 48, "fonts do not fit the shorter dimension");
        check(Viewport{1530,1140}.x(252) == 378 && Viewport{1530,1140}.y(398) == 597, "proportional coordinates incorrect");
        std::cout << "Proportional viewport boundaries, small/wide/tall windows and uniform typography passed.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
