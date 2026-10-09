// Unit coverage for the window-geometry helpers used to scope compositor
// screenshots. These are pure parsers, so the test feeds fixed sway/xdotool
// payloads and never touches a desktop session.

#include "skl/wcas/drm.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <string_view>

int main() {
    using namespace skl::wcas;

    // extract(): pull a numeric JSON field.
    assert(extract("\"x\": 12, \"y\": 34}", "x") == 12);
    assert(extract("\"height\": 800}", "height") == 800);
    assert(!extract("\"z\": 1}", "x").has_value());

    // parse(): assemble a rect from a JSON object.
    const auto rect = parse("{\"x\": 0, \"y\": 0, \"width\": 1280, \"height\": 800}");
    assert(rect.has_value());
    assert(rect->x == 0 && rect->y == 0 && rect->width == 1'280 && rect->height == 800);

    // parse_xdotool(): read the --shell key=value form.
    std::string_view xdotool = "WINDOW=8388610\nX=5\nY=6\nWIDTH=100\nHEIGHT=200\nSCREEN=0\n";
    const auto geo = parse_xdotool(xdotool);
    assert(geo.has_value());
    assert(geo->x == 5 && geo->y == 6 && geo->width == 100 && geo->height == 200);

    // from_swaytree_by_size_and_title(): match by size and optional title.
    const std::string tree =
        "{\"name\": \"Game Window\", \"rect\": {\"x\": 100, \"y\": 50, \"width\": 1280, "
        "\"height\": 720}}";
    const auto found = from_swaytree_by_size_and_title(tree, cv::Size(1'280, 720), std::string_view("Game"));
    assert(found.has_value());
    assert(found->x == 100 && found->y == 50 && found->width == 1'280 && found->height == 720);

    // Wrong size or title must not match.
    assert(!from_swaytree_by_size_and_title(tree, cv::Size(640, 480), std::nullopt).has_value());
    assert(!from_swaytree_by_size_and_title(tree, cv::Size(1'280, 720), std::string_view("Other")).has_value());

    // Title omitted still matches by size alone.
    const auto by_size = from_swaytree_by_size_and_title(tree, cv::Size(1'280, 720), std::nullopt);
    assert(by_size.has_value() && by_size->x == 100);

    std::printf("window geometry unit: ok\n");
    return 0;
}
