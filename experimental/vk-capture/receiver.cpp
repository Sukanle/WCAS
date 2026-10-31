// Minimal receiver for the experimental WCAS Vulkan layer.
// It verifies that the layer can export linear DMA-BUF frames over a Unix
// socket. It is a test/diagnostic tool, not the production FrameBus consumer.

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct FrameHeader {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t sequence;
};

int receive_fd(int socket_fd, FrameHeader &header) {
    iovec vector{&header, sizeof(header)};
    char control[CMSG_SPACE(sizeof(int))]{};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    const ssize_t received = ::recvmsg(socket_fd, &message, 0);
    if (received < static_cast<ssize_t>(sizeof(header))) return -1;
    for (cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg != nullptr; cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            int fd = -1;
            std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
            return fd;
        }
    }
    return -1;
}

}   // namespace

int main(int argc, char **argv) {
    std::string socket_path = "/tmp/wcas-vk.sock";
    std::string output;
    bool once = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
            socket_path = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            output = argv[++i];
        else if (std::strcmp(argv[i], "--once") == 0)
            once = true;
    }

    const int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        std::perror("socket");
        return 1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    ::unlink(socket_path.c_str());
    if (::bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        std::perror("bind");
        ::close(listener);
        return 1;
    }
    if (::listen(listener, 4) < 0) {
        std::perror("listen");
        ::close(listener);
        return 1;
    }
    std::printf("listening on %s\n", socket_path.c_str());

    while (true) {
        const int client = ::accept4(listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) {
            std::perror("accept");
            break;
        }
        std::printf("client connected\n");
        uint64_t frames = 0;
        while (true) {
            FrameHeader header{};
            const int fd = receive_fd(client, header);
            if (fd < 0) break;
            ++frames;
            std::printf("frame %llu: %ux%u stride=%u format=%u fd=%d\n",
                static_cast<unsigned long long>(header.sequence), header.width, header.height, header.stride,
                header.format, fd);
            if (!output.empty() && frames == 1) {
                const size_t size = static_cast<size_t>(header.stride) * header.height;
                if (void *map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0); map != MAP_FAILED) {
                    if (FILE *file = std::fopen(output.c_str(), "wb"); file != nullptr) {
                        std::fwrite(map, 1, size, file);
                        std::fclose(file);
                        std::printf("wrote %s (%zu bytes)\n", output.c_str(), size);
                    }
                    ::munmap(map, size);
                }
            }
            ::close(fd);
            if (once) break;
        }
        std::printf("client closed after %llu frames\n", static_cast<unsigned long long>(frames));
        ::close(client);
        if (once) break;
    }

    ::close(listener);
    ::unlink(socket_path.c_str());
    return 0;
}
