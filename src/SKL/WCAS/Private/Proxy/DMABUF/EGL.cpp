#include <stdlib.h>

#include <mutex>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN32
#else
#  include <gbm.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

#include "SKL/WCAS/Private/Proxy/DMABUF/EGL.h"
#include "SKL/WCAS/Private/Proxy/DMABUF/Vulkan.h"   // trace_capture

#include <fmt/format.h>

#ifdef WCAS_HAVE_EGL

#  include <EGL/egl.h>
#  include <EGL/eglext.h>
#  include <GLES2/gl2.h>
#  include <GLES2/gl2ext.h>

namespace skl::wcas::proxy {
namespace {

/// Lazily created EGL display + GLES2 context bound to a GBM device. EGL/GBM
/// state is process-global, so a single instance is shared and serialized.
struct EglDevice {
    int device_fd = -1;   // kept open for the lifetime of a device-platform display
    gbm_device *gbm = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    std::string vendor;
    bool have_modifiers = false;
    bool valid = false;
    std::mutex mutex;

    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture = nullptr;
    PFNEGLQUERYDMABUFFORMATSEXTPROC query_formats = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_modifiers = nullptr;
};

/// Render node paths, discrete (NVIDIA) first: the compressed DMA-BUFs are
/// produced by the GPU the app renders on, and only that GPU's EGL can decode
/// its own layout.
std::vector<std::string> render_node_candidates() {
    std::vector<std::string> nodes;
    const std::string base = "/dev/dri/";
    for (int index = 128; index <= 140; ++index) {
        const std::string path = base + "renderD" + std::to_string(index);
        if (::access(path.c_str(), R_OK | W_OK) == 0) nodes.push_back(path);
    }
    std::stable_partition(nodes.begin(), nodes.end(), [](const std::string &path) {
        // /dev/dri/renderD128 -> /sys/class/drm/renderD128/device/vendor
        const std::string name = path.substr(path.rfind('/') + 1);
        const std::string sysfs = "/sys/class/drm/" + name + "/device/vendor";
        const int fd = ::open(sysfs.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        char buffer[16]{};
        const ssize_t read_bytes = ::read(fd, buffer, sizeof(buffer) - 1);
        ::close(fd);
        return read_bytes > 0 && std::string(buffer).find("0x10de") != std::string::npos;   // NVIDIA
    });
    return nodes;
}

void destroy_egl_device(EglDevice &d) {
    if (d.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(d.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (d.context != EGL_NO_CONTEXT) eglDestroyContext(d.display, d.context);
        if (d.surface != EGL_NO_SURFACE) eglDestroySurface(d.display, d.surface);
        eglTerminate(d.display);
    }
    if (d.gbm != nullptr) gbm_device_destroy(d.gbm);
    if (d.device_fd >= 0) ::close(d.device_fd);
    d.gbm = nullptr;
    d.device_fd = -1;
    d.display = EGL_NO_DISPLAY;
    d.context = EGL_NO_CONTEXT;
    d.surface = EGL_NO_SURFACE;
    d.vendor.clear();
    d.have_modifiers = false;
    d.valid = false;
    d.create_image = nullptr;
    d.destroy_image = nullptr;
    d.image_target_texture = nullptr;
    d.query_formats = nullptr;
    d.query_modifiers = nullptr;
}

void initialize_egl_device(EglDevice &d) {
    // NVIDIA's GBM backend has to be selected explicitly; only set it when the
    // caller has not. Only used by the GBM-platform fallback below.
    if (::getenv("GBM_BACKEND") == nullptr) ::setenv("GBM_BACKEND", "nvidia-drm", 1);

    const std::vector<std::string> nodes = render_node_candidates();
    if (nodes.empty()) {
        trace_capture("egl backend: no /dev/dri render node");
        return;
    }

    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));

    for (const std::string &path : nodes) {
        int node_fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (node_fd < 0) continue;

        // 1) EGL_PLATFORM_DEVICE_EXT pins the vendor that owns the node (libglvnd
        //    routes an NVIDIA fd to NVIDIA's EGL). This is the reliable way to
        //    bind the discrete GPU on a hybrid system.
        if (get_platform_display != nullptr) {
            d.display = get_platform_display(
                EGL_PLATFORM_DEVICE_EXT, reinterpret_cast<void *>(static_cast<intptr_t>(node_fd)), nullptr);
            if (d.display != EGL_NO_DISPLAY && eglInitialize(d.display, nullptr, nullptr) == EGL_TRUE) {
                const char *vendor = eglQueryString(d.display, EGL_VENDOR);
                d.vendor = vendor != nullptr ? vendor : "";
                d.device_fd = node_fd;
                node_fd = -1;
                break;
            }
            if (d.display != EGL_NO_DISPLAY) {
                eglTerminate(d.display);
                d.display = EGL_NO_DISPLAY;
            }
        }

        // 2) GBM platform: the route the compositor (wlroots) uses.
        if (gbm_device *gbm = gbm_create_device(node_fd); gbm != nullptr) {
            if (get_platform_display != nullptr)
                d.display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
            else
                d.display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbm));
            if (d.display != EGL_NO_DISPLAY && eglInitialize(d.display, nullptr, nullptr) == EGL_TRUE) {
                const char *vendor = eglQueryString(d.display, EGL_VENDOR);
                d.vendor = vendor != nullptr ? vendor : "";
                d.gbm = gbm;
                d.device_fd = node_fd;
                node_fd = -1;
                break;
            }
            if (d.display != EGL_NO_DISPLAY) {
                eglTerminate(d.display);
                d.display = EGL_NO_DISPLAY;
            }
            gbm_device_destroy(gbm);
        }
        ::close(node_fd);
    }

    if (d.display == EGL_NO_DISPLAY) {
        trace_capture("egl backend: eglInitialize failed on every render node");
        return;
    }

    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        trace_capture("egl backend: eglBindAPI(EGL_OPENGL_ES_API) failed");
        destroy_egl_device(d);
        return;
    }

    const char *extensions = eglQueryString(d.display, EGL_EXTENSIONS);
    const std::string extension_list = extensions != nullptr ? extensions : "";
    const bool have_dma_buf_import = extension_list.find("EGL_EXT_image_dma_buf_import") != std::string::npos;
    d.have_modifiers = extension_list.find("EGL_EXT_image_dma_buf_import_modifiers") != std::string::npos;
    if (!have_dma_buf_import) {
        trace_capture("egl backend: EGL_EXT_image_dma_buf_import not advertised");
        destroy_egl_device(d);
        return;
    }

    const EGLint config_attribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config = nullptr;
    EGLint config_count = 0;
    if (eglChooseConfig(d.display, config_attribs, &config, 1, &config_count) != EGL_TRUE || config_count == 0) {
        trace_capture("egl backend: eglChooseConfig found no RGBA/ES2 config");
        destroy_egl_device(d);
        return;
    }

    // A 1x1 pbuffer is enough: reading happens from a framebuffer object, not
    // the default surface, and it avoids depending on
    // EGL_KHR_surfaceless_context.
    const EGLint surface_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    d.surface = eglCreatePbufferSurface(d.display, config, surface_attribs);

    const EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    d.context = eglCreateContext(d.display, config, EGL_NO_CONTEXT, context_attribs);
    if (d.context == EGL_NO_CONTEXT) {
        trace_capture("egl backend: eglCreateContext failed");
        destroy_egl_device(d);
        return;
    }
    if (eglMakeCurrent(d.display, d.surface, d.surface, d.context) != EGL_TRUE) {
        trace_capture("egl backend: eglMakeCurrent failed");
        destroy_egl_device(d);
        return;
    }

    d.create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    d.destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    d.image_target_texture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (d.create_image == nullptr || d.destroy_image == nullptr || d.image_target_texture == nullptr) {
        trace_capture("egl backend: EGLImage entry points unavailable");
        destroy_egl_device(d);
        return;
    }

    // Capability query entry points (from EGL_EXT_image_dma_buf_import_modifiers).
    // Optional: without them we cannot pre-check a format/modifier and fall back
    // to attempting the import.
    if (d.have_modifiers) {
        d.query_formats =
            reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
        d.query_modifiers =
            reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    }

    trace_capture(fmt::format("egl backend: ready (vendor='{}', modifiers={}, query={})", d.vendor,
        d.have_modifiers ? "yes" : "no", d.query_modifiers != nullptr ? "yes" : "no"));
    d.valid = true;
}

EglDevice &egl_device() {
    // `EglDevice` holds a mutex, so it is neither copyable nor movable: build it
    // in place instead of returning it from a factory.
    static EglDevice device;
    static std::once_flag initialized;
    std::call_once(initialized, [] { initialize_egl_device(device); });
    return device;
}

}   // namespace

bool egl_dmabuf_available() { return egl_device().valid; }

bool egl_dmabuf_import_supported(uint32_t fourcc, uint64_t modifier) {
    auto &d = egl_device();
    if (!d.valid) return false;
    // Without the modifiers extension (and its query entry points) we cannot ask
    // the driver, so allow the import to be attempted.
    if (d.query_formats == nullptr || d.query_modifiers == nullptr) return true;

    std::lock_guard<std::mutex> lock(d.mutex);
    EGLint format_count = 0;
    if (d.query_formats(d.display, 0, nullptr, &format_count) != EGL_TRUE || format_count <= 0) return false;
    std::vector<EGLint> formats(format_count);
    if (d.query_formats(d.display, format_count, formats.data(), &format_count) != EGL_TRUE) return false;
    if (std::find(formats.begin(), formats.end(), static_cast<EGLint>(fourcc)) == formats.end()) return false;

    // A linear modifier is implicitly available for any listed format; the driver
    // does not always enumerate modifier 0 explicitly.
    if (modifier == 0) return true;

    EGLint modifier_count = 0;
    if (d.query_modifiers(d.display, static_cast<EGLint>(fourcc), 0, nullptr, nullptr, &modifier_count) != EGL_TRUE
        || modifier_count <= 0)
        return false;
    std::vector<EGLuint64KHR> modifiers(modifier_count);
    if (d.query_modifiers(
            d.display, static_cast<EGLint>(fourcc), modifier_count, modifiers.data(), nullptr, &modifier_count)
        != EGL_TRUE)
        return false;
    return std::find(modifiers.begin(), modifiers.end(), static_cast<EGLuint64KHR>(modifier)) != modifiers.end();
}

optional<std::vector<uint8_t>> egl_read_dmabuf(
    int fd, uint32_t width, uint32_t height, uint32_t stride, uint64_t modifier, uint32_t fourcc, uint32_t offset) {
    if (fd < 0 || width == 0 || height == 0) return std::nullopt;
    auto &d = egl_device();
    if (!d.valid) return std::nullopt;

    std::lock_guard<std::mutex> lock(d.mutex);
    // The GL context is per-thread state; rebind in case another thread changed
    // it. (The mutex serializes imports, so this is cheap and correct.)
    eglMakeCurrent(d.display, d.surface, d.surface, d.context);

    std::vector<EGLint> attribs;
    attribs.push_back(EGL_WIDTH);
    attribs.push_back(static_cast<EGLint>(width));
    attribs.push_back(EGL_HEIGHT);
    attribs.push_back(static_cast<EGLint>(height));
    attribs.push_back(EGL_LINUX_DRM_FOURCC_EXT);
    attribs.push_back(static_cast<EGLint>(fourcc));
    attribs.push_back(EGL_DMA_BUF_PLANE0_FD_EXT);
    attribs.push_back(fd);
    attribs.push_back(EGL_DMA_BUF_PLANE0_OFFSET_EXT);
    attribs.push_back(static_cast<EGLint>(offset));
    attribs.push_back(EGL_DMA_BUF_PLANE0_PITCH_EXT);
    attribs.push_back(static_cast<EGLint>(stride));
    if (d.have_modifiers) {
        attribs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
        attribs.push_back(static_cast<EGLint>(modifier & 0xffffffffULL));
        attribs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
        attribs.push_back(static_cast<EGLint>(modifier >> 32));
    } else if (modifier != 0) {
        trace_capture(
            fmt::format("egl backend: non-linear modifier 0x{:016x} but no modifier import extension", modifier));
        return std::nullopt;
    }
    attribs.push_back(EGL_NONE);

    const EGLImageKHR image = d.create_image(d.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
    if (image == EGL_NO_IMAGE_KHR) {
        trace_capture(
            fmt::format("egl backend: eglCreateImageKHR failed for {}x{} fourcc=0x{:08x} modifier=0x{:016x} "
                        "(EGL error 0x{:x})",
                width, height, fourcc, modifier, eglGetError()));
        return std::nullopt;
    }

    GLuint texture = 0;
    GLuint framebuffer = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    d.image_target_texture(GL_TEXTURE_2D, image);

    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

    optional<std::vector<uint8_t>> result;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        trace_capture("egl backend: imported texture is not framebuffer-complete");
    } else {
        const size_t row_bytes = static_cast<size_t>(width) * 4U;
        std::vector<uint8_t> readback(row_bytes * height);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
            readback.data());
        if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
            trace_capture(fmt::format("egl backend: glReadPixels failed (GL error 0x{:x})", error));
        } else {
            // No extra flip: an EGLImage imported from a DMA-BUF exposes the
            // buffer's first row as GL texel row 0, so glReadPixels already
            // returns the image top-to-bottom (flipping here inverted it).
            result = std::move(readback);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (framebuffer != 0) glDeleteFramebuffers(1, &framebuffer);
    if (texture != 0) glDeleteTextures(1, &texture);
    d.destroy_image(d.display, image);
    return result;
}

}   // namespace skl::wcas::proxy

#else   // !WCAS_HAVE_EGL

namespace skl::wcas::proxy {

bool egl_dmabuf_available() { return false; }

bool egl_dmabuf_import_supported(uint32_t fourcc, uint64_t modifier) {
    (void)fourcc;
    (void)modifier;
    return false;
}

optional<std::vector<uint8_t>> egl_read_dmabuf(
    int fd, uint32_t width, uint32_t height, uint32_t stride, uint64_t modifier, uint32_t fourcc, uint32_t offset) {
    (void)fd;
    (void)width;
    (void)height;
    (void)stride;
    (void)modifier;
    (void)fourcc;
    (void)offset;
    return std::nullopt;
}

}   // namespace skl::wcas::proxy

#endif
