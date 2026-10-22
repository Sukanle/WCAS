#include "skl/wcas/wcas.h"
#include <opencv2/imgcodecs.hpp>
#include <fmt/std.h>
int main(int argc, char **argv) {
    if (argc < 2) {
        fmt::println(stderr, "usage: wcas-example <program> [arguments...]");
        return 2;
    }
    auto harness = skl::wcas::ComputerUse::create();
    if (!harness) {
        fmt::println(stderr, "[{}] {}", harness.error(), harness.error().message());
        return 1;
    }
    skl::wcas::LaunchConfig config(argv[1]);
    for (int i = 2; i < argc; ++i)
        config.arg(argv[i]);
    auto app = harness.value().launch(config);
    if (!app) {
        fmt::println(stderr, "[{}] {}", app.error(), app.error().message());
        return 1;
    }
    auto frame = app.value().capture();
    if (frame)
        cv::imwrite("wcas-capture.png", frame.value());
    else
        fmt::println(stderr, "[{}] {}", frame.error(), frame.error().message());
    return 0;
}
