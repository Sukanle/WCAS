#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace skl::wcas::sdm {

/// Pixel layout stored in a shared frame slot.
enum class PixelFormat : std::uint32_t {
    Unknown = 0,
    Bgr8 = 1,
    Bgra8 = 2,
};

struct FrameInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    PixelFormat format = PixelFormat::Unknown;
    std::uint64_t timestamp_ns = 0;
};

/// A borrowed view into one shared-memory slot. The view is valid until
/// `release()` is called and must not outlive the Channel.
struct FrameView {
    const std::uint8_t *data = nullptr;
    std::size_t size = 0;
    FrameInfo info{};
    std::uint32_t seq = 0;
    std::uint32_t slot = 0;
};

struct Config {
    /// Number of frame slots. Three or four is a good latest-wins ring.
    std::uint32_t slot_count = 4;
    /// Bytes reserved per slot; must cover stride * height.
    std::uint32_t slot_bytes = 8u * 1'024u * 1'024u;
};

/// A cross-platform, named shared-memory frame channel.
///
/// The writer owns the segment and removes it on destruction. Readers open the
/// same name read/write. The channel intentionally uses only fixed-width PODs
/// and lock-free 32-bit atomics in shared memory; no pointers, STL containers or
/// process-local mutexes are placed in the segment.
///
/// This is a data-plane primitive: control/discovery is expected to happen over
/// the IPC control channel (UDS/named pipe), which passes the channel name and
/// frame metadata.
class Channel {
public:
    static std::unique_ptr<Channel> create(std::string name, const Config &config, std::string &error);
    static std::unique_ptr<Channel> open(std::string name, std::string &error);

    /// Removes a named segment. Safe to call when it does not exist.
    static bool remove(std::string_view name);

    /// Returns a process-unique name suitable for `create`.
    static std::string unique_name(std::string_view prefix = "wcas-sdm");

    ~Channel();
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;

    [[nodiscard]] const std::string &name() const noexcept;
    [[nodiscard]] std::uint32_t slot_count() const noexcept;
    [[nodiscard]] std::uint32_t slot_bytes() const noexcept;
    [[nodiscard]] bool is_owner() const noexcept;

    /// Copies one frame into the ring. If every slot is free/ready, the oldest
    /// ready slot is overwritten; slots currently held by a reader (READING) are
    /// never overwritten.
    bool write(const void *data, std::size_t size, const FrameInfo &info, std::string &error);

    /// Acquires the newest READY frame. The caller must call `release()`.
    bool read_latest(FrameView &view, std::string &error);

    /// Releases a view previously returned by `read_latest`.
    void release(FrameView &view) noexcept;

    /// Polls until a frame newer than `previous_seq` exists. This avoids
    /// platform-specific eventfd/kqueue code and is sufficient for the IPC
    /// data-plane first version.
    bool wait_for_frame(std::uint32_t previous_seq, std::chrono::milliseconds timeout);

    /// Last published sequence, 0 when no frame has been written.
    [[nodiscard]] std::uint32_t latest_seq() const noexcept;

private:
    struct Impl;
    explicit Channel(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}   // namespace skl::wcas::sdm
