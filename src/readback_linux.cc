// Linux backend: Electron passes the frame as dmabuf plane(s) ({fd, stride, offset, size}) plus a DRM
// format modifier. For LINEAR buffers (modifier 0 / DRM_FORMAT_MOD_LINEAR) the dmabuf is CPU-mappable, so
// we mmap the fd and copy rows by stride. For tiled/compressed modifiers the bytes are not linear in CPU
// memory and this path cannot produce correct pixels — that case needs a GPU import (EGL
// EGL_LINUX_DMA_BUF_EXT -> texture -> glReadPixels), which is a follow-up (see README).
#include "osr_readback.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#include "convert.h"

namespace osrcap {

namespace {
// DRM_FORMAT_MOD_LINEAR
constexpr uint64_t kModifierLinear = 0;
// DRM_FORMAT_MOD_INVALID is sometimes reported for implicit/linear layouts
constexpr uint64_t kModifierInvalid = 0x00ffffffffffffffULL;
}  // namespace

bool ReadbackDmabuf(const std::vector<DmabufPlane>& planes, uint64_t modifier, uint32_t width, uint32_t height, int format, std::vector<uint8_t>& out, std::string& err) {
    // LINEAR dmabufs are read back as BGRA (below). When the caller asks for UYVY/UYVA we convert on the
    // CPU (ConvertBgraInPlace) so Linux gets the same reduced NDI/SDI wire format as Windows. A GPU (EGL/GL
    // import + shader) convert for tiled buffers is future work — see osr-capture HANDOFF.
    if (planes.empty()) {
        err = "no dmabuf planes";
        return false;
    }
    if (modifier != kModifierLinear && modifier != kModifierInvalid) {
        // Non-linear (tiled/compressed) layout: needs EGL/GL import to read back correctly.
        err = "non-linear dmabuf modifier requires GPU import (not yet implemented)";
        return false;
    }

    const DmabufPlane& plane = planes[0];  // single-plane BGRA/RGBA
    const size_t mapLength = static_cast<size_t>(plane.offset) + static_cast<size_t>(plane.stride) * height;

    void* mapped = mmap(nullptr, mapLength, PROT_READ, MAP_SHARED, plane.fd, 0);
    if (mapped == MAP_FAILED) {
        err = "mmap of dmabuf failed";
        return false;
    }

    const uint8_t* base = static_cast<const uint8_t*>(mapped) + plane.offset;
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    out.resize(rowBytes * height);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * rowBytes, base + static_cast<size_t>(y) * plane.stride, rowBytes);
    }

    munmap(mapped, mapLength);

    // format 1 = UYVY, 2 = UYVA: convert the BGRA we just read; 0 leaves it as BGRA.
    ConvertBgraInPlace(out, width, height, format);
    return true;
}

}  // namespace osrcap
