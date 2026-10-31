#include "SKL/WCAS/WCAS.h"
#include "SKL/WCAS/Error.h"
#include <opencv2/imgcodecs.hpp>
#include <fmt/std.h>
int main(int argc, char **argv) {
    if (argc < 2) {
        fmt::println(stderr, "usage: wcas-example <program> [arguments...]");
        return 2;
    }
    auto harness = skl::wcas::ComputerUse::create();
    if (!harness) {
        fmt::println(stderr, "[{}:{}] {}", harness.error().domain, harness.error().value,
            skl::wcas::error_message(harness.error()));
        return 1;
    }
    skl::wcas::LaunchConfig config(argv[1]);
    for (int i = 2; i < argc; ++i)
        config.arg(argv[i]);
    auto app = harness.value().launch(config);
    if (!app) {
        fmt::println(
            stderr, "[{}:{}] {}", app.error().domain, app.error().value, skl::wcas::error_message(app.error()));
        return 1;
    }
    auto frame = app.value().capture();
    if (frame)
        cv::imwrite("wcas-capture.png", frame.value());
    else
        fmt::println(
            stderr, "[{}:{}] {}", frame.error().domain, frame.error().value, skl::wcas::error_message(frame.error()));
    return 0;
}
