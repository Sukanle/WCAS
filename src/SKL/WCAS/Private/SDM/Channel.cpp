#include "SKL/WCAS/Private/SDM/Channel.h"

#include <boost/interprocess/exceptions.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/permissions.hpp>
#include <boost/interprocess/shared_memory_object.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>

#ifdef _WIN32
#  include <process.h>
#else
#  include <unistd.h>
#endif

namespace skl::wcas::sdm {
namespace {

constexpr std::uint64_t kMagic = 0x5743415353444D31ULL;   // "WCASSDM1"
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kMaxSlots = 64;
constexpr std::size_t kAlignment = 64;

enum class SlotState : std::uint32_t {
    Free = 0,
    Writing = 1,
    Ready = 2,
    Reading = 3,
};

struct SharedHeader {
    std::uint64_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t slot_count = 0;
    std::uint32_t slot_bytes = 0;
    std::uint32_t header_bytes = 0;
    std::atomic<std::uint32_t> latest_seq{0};
};

struct SharedSlot {
    std::atomic<std::uint32_t> state{static_cast<std::uint32_t>(SlotState::Free)};
    std::atomic<std::uint32_t> seq{0};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;
    std::uint32_t format = 0;
    std::uint32_t data_size = 0;
    std::uint32_t data_offset = 0;
    std::uint64_t timestamp_ns = 0;
};

static_assert(
    std::atomic<std::uint32_t>::is_always_lock_free, "shared-memory sequence/state atomics must be lock-free");
static_assert(std::is_trivially_copyable_v<FrameInfo>);

std::size_t align_up(std::size_t value, std::size_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

std::uint32_t process_id() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(_getpid());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

bool is_newer(std::uint32_t lhs, std::uint32_t rhs) { return static_cast<std::int32_t>(lhs - rhs) > 0; }

}   // namespace

struct Channel::Impl {
    std::unique_ptr<boost::interprocess::shared_memory_object> shm;
    std::unique_ptr<boost::interprocess::mapped_region> region;
    std::string name;
    bool owner = false;
    SharedHeader *header = nullptr;
    SharedSlot *slots = nullptr;
    std::uint8_t *base = nullptr;
    std::uint32_t slot_count = 0;
    std::uint32_t slot_bytes = 0;
    std::uint32_t next_seq = 1;

    ~Impl() {
        region.reset();
        shm.reset();
        if (owner && !name.empty()) boost::interprocess::shared_memory_object::remove(name.c_str());
    }
};

Channel::Channel(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Channel::~Channel() = default;

std::string Channel::unique_name(std::string_view prefix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string(prefix) + "-" + std::to_string(process_id()) + "-" + std::to_string(stamp);
}

bool Channel::remove(std::string_view name) {
    if (name.empty()) return false;
    try {
        return boost::interprocess::shared_memory_object::remove(std::string(name).c_str());
    } catch (...) {
        return false;
    }
}

std::unique_ptr<Channel> Channel::create(std::string name, const Config &config, std::string &error) {
    error.clear();
    if (name.empty()) {
        error = "empty shared-memory name";
        return nullptr;
    }
    if (config.slot_count == 0 || config.slot_count > kMaxSlots) {
        error = "invalid slot_count";
        return nullptr;
    }
    if (config.slot_bytes == 0) {
        error = "invalid slot_bytes";
        return nullptr;
    }

    const std::uint32_t slot_bytes = static_cast<std::uint32_t>(align_up(config.slot_bytes, kAlignment));
    const std::size_t header_bytes =
        align_up(sizeof(SharedHeader) + static_cast<std::size_t>(config.slot_count) * sizeof(SharedSlot), kAlignment);
    const std::size_t segment_bytes = header_bytes + static_cast<std::size_t>(config.slot_count) * slot_bytes;
    if (segment_bytes > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        error = "shared-memory segment too large";
        return nullptr;
    }

    try {
        auto impl = std::make_unique<Impl>();
        impl->name = std::move(name);
        impl->slot_count = config.slot_count;
        impl->slot_bytes = slot_bytes;
        impl->shm = std::make_unique<boost::interprocess::shared_memory_object>(boost::interprocess::create_only,
            impl->name.c_str(), boost::interprocess::read_write, boost::interprocess::permissions(0600));
        impl->shm->truncate(static_cast<boost::interprocess::offset_t>(segment_bytes));
        impl->region =
            std::make_unique<boost::interprocess::mapped_region>(*impl->shm, boost::interprocess::read_write);
        impl->base = static_cast<std::uint8_t *>(impl->region->get_address());

        impl->header = new (impl->base) SharedHeader{};
        impl->header->magic = kMagic;
        impl->header->version = kVersion;
        impl->header->slot_count = impl->slot_count;
        impl->header->slot_bytes = impl->slot_bytes;
        impl->header->header_bytes = static_cast<std::uint32_t>(header_bytes);
        impl->header->latest_seq.store(0, std::memory_order_relaxed);

        impl->slots = reinterpret_cast<SharedSlot *>(impl->base + sizeof(SharedHeader));
        for (std::uint32_t i = 0; i < impl->slot_count; ++i) {
            auto *slot = new (&impl->slots[i]) SharedSlot{};
            slot->state.store(static_cast<std::uint32_t>(SlotState::Free), std::memory_order_relaxed);
            slot->seq.store(0, std::memory_order_relaxed);
            slot->data_offset =
                static_cast<std::uint32_t>(header_bytes + static_cast<std::size_t>(i) * impl->slot_bytes);
        }

        impl->owner = true;
        return std::unique_ptr<Channel>(new Channel(std::move(impl)));
    } catch (const std::exception &exception) {
        error = exception.what();
        return nullptr;
    }
}

std::unique_ptr<Channel> Channel::open(std::string name, std::string &error) {
    error.clear();
    if (name.empty()) {
        error = "empty shared-memory name";
        return nullptr;
    }

    try {
        auto impl = std::make_unique<Impl>();
        impl->name = std::move(name);
        impl->shm = std::make_unique<boost::interprocess::shared_memory_object>(
            boost::interprocess::open_only, impl->name.c_str(), boost::interprocess::read_write);
        impl->region =
            std::make_unique<boost::interprocess::mapped_region>(*impl->shm, boost::interprocess::read_write);
        impl->base = static_cast<std::uint8_t *>(impl->region->get_address());
        impl->header = reinterpret_cast<SharedHeader *>(impl->base);

        if (impl->header->magic != kMagic || impl->header->version != kVersion) {
            error = "shared-memory header mismatch";
            return nullptr;
        }
        impl->slot_count = impl->header->slot_count;
        impl->slot_bytes = impl->header->slot_bytes;
        if (impl->slot_count == 0
            || impl->slot_count > kMaxSlots
            || impl->header->header_bytes > impl->region->get_size()) {
            error = "shared-memory layout invalid";
            return nullptr;
        }
        const std::size_t required =
            impl->header->header_bytes + static_cast<std::size_t>(impl->slot_count) * impl->slot_bytes;
        if (required > impl->region->get_size()) {
            error = "shared-memory size mismatch";
            return nullptr;
        }
        impl->slots = reinterpret_cast<SharedSlot *>(impl->base + sizeof(SharedHeader));
        return std::unique_ptr<Channel>(new Channel(std::move(impl)));
    } catch (const std::exception &exception) {
        error = exception.what();
        return nullptr;
    }
}

const std::string &Channel::name() const noexcept { return impl_->name; }
std::uint32_t Channel::slot_count() const noexcept { return impl_->slot_count; }
std::uint32_t Channel::slot_bytes() const noexcept { return impl_->slot_bytes; }
bool Channel::is_owner() const noexcept { return impl_->owner; }
std::uint32_t Channel::latest_seq() const noexcept {
    return impl_->header ? impl_->header->latest_seq.load(std::memory_order_acquire) : 0;
}

bool Channel::write(const void *data, std::size_t size, const FrameInfo &info, std::string &error) {
    error.clear();
    if (data == nullptr || size == 0) {
        error = "empty frame";
        return false;
    }
    if (size > impl_->slot_bytes) {
        error = "frame larger than slot";
        return false;
    }
    if (info.stride != 0
        && info.height != 0
        && static_cast<std::uint64_t>(info.stride) * info.height > impl_->slot_bytes) {
        error = "stride * height larger than slot";
        return false;
    }

    auto try_acquire = [&](SlotState expected) -> int {
        for (std::uint32_t i = 0; i < impl_->slot_count; ++i) {
            std::uint32_t state = impl_->slots[i].state.load(std::memory_order_acquire);
            if (state != static_cast<std::uint32_t>(expected)) continue;
            if (impl_->slots[i].state.compare_exchange_strong(
                    state, static_cast<std::uint32_t>(SlotState::Writing), std::memory_order_acq_rel)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };

    int index = try_acquire(SlotState::Free);
    if (index < 0) {
        // Latest-wins: overwrite the oldest READY slot, never a slot a reader
        // is currently holding.
        int oldest = -1;
        std::uint32_t oldest_seq = 0;
        for (std::uint32_t i = 0; i < impl_->slot_count; ++i) {
            if (impl_->slots[i].state.load(std::memory_order_acquire) != static_cast<std::uint32_t>(SlotState::Ready)) {
                continue;
            }
            const std::uint32_t seq = impl_->slots[i].seq.load(std::memory_order_acquire);
            if (oldest < 0 || is_newer(oldest_seq, seq)) {
                oldest = static_cast<int>(i);
                oldest_seq = seq;
            }
        }
        if (oldest >= 0) {
            std::uint32_t expected = static_cast<std::uint32_t>(SlotState::Ready);
            if (impl_->slots[oldest].state.compare_exchange_strong(
                    expected, static_cast<std::uint32_t>(SlotState::Writing), std::memory_order_acq_rel)) {
                index = oldest;
            }
        }
    }
    if (index < 0) {
        error = "no writable slot (all frames are being read)";
        return false;
    }

    SharedSlot &slot = impl_->slots[index];
    const std::uint32_t seq = impl_->next_seq++;
    if (impl_->next_seq == 0) impl_->next_seq = 1;

    slot.width = info.width;
    slot.height = info.height;
    slot.stride = info.stride;
    slot.format = static_cast<std::uint32_t>(info.format);
    slot.timestamp_ns = info.timestamp_ns;
    slot.data_size = static_cast<std::uint32_t>(size);
    std::memcpy(impl_->base + slot.data_offset, data, size);

    slot.seq.store(seq, std::memory_order_release);
    slot.state.store(static_cast<std::uint32_t>(SlotState::Ready), std::memory_order_release);
    impl_->header->latest_seq.store(seq, std::memory_order_release);
    return true;
}

bool Channel::read_latest(FrameView &view, std::string &error) {
    error.clear();
    view = {};

    for (int attempt = 0; attempt < 8; ++attempt) {
        int best = -1;
        std::uint32_t best_seq = 0;
        for (std::uint32_t i = 0; i < impl_->slot_count; ++i) {
            if (impl_->slots[i].state.load(std::memory_order_acquire) != static_cast<std::uint32_t>(SlotState::Ready)) {
                continue;
            }
            const std::uint32_t seq = impl_->slots[i].seq.load(std::memory_order_acquire);
            if (best < 0 || is_newer(seq, best_seq)) {
                best = static_cast<int>(i);
                best_seq = seq;
            }
        }
        if (best < 0) break;

        std::uint32_t expected = static_cast<std::uint32_t>(SlotState::Ready);
        if (!impl_->slots[best].state.compare_exchange_strong(
                expected, static_cast<std::uint32_t>(SlotState::Reading), std::memory_order_acq_rel)) {
            continue;
        }

        const SharedSlot &slot = impl_->slots[best];
        view.data = impl_->base + slot.data_offset;
        view.size = slot.data_size;
        view.info.width = slot.width;
        view.info.height = slot.height;
        view.info.stride = slot.stride;
        view.info.format = static_cast<PixelFormat>(slot.format);
        view.info.timestamp_ns = slot.timestamp_ns;
        view.seq = slot.seq.load(std::memory_order_acquire);
        view.slot = static_cast<std::uint32_t>(best);
        return true;
    }

    error = "no ready frame";
    return false;
}

void Channel::release(FrameView &view) noexcept {
    if (!impl_ || view.slot >= impl_->slot_count) return;
    SharedSlot &slot = impl_->slots[view.slot];
    if (slot.seq.load(std::memory_order_acquire) == view.seq
        && slot.state.load(std::memory_order_acquire) == static_cast<std::uint32_t>(SlotState::Reading)) {
        slot.state.store(static_cast<std::uint32_t>(SlotState::Free), std::memory_order_release);
    }
    view = {};
}

bool Channel::wait_for_frame(std::uint32_t previous_seq, std::chrono::milliseconds timeout) {
    if (!impl_ || !impl_->header) return false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (is_newer(impl_->header->latest_seq.load(std::memory_order_acquire), previous_seq)) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

}   // namespace skl::wcas::sdm
