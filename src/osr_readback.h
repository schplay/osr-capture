// Platform readback backends: given a shared-texture handle (Windows/macOS) or dmabuf planes (Linux),
// produce a tightly-packed BGRA/RGBA buffer (width * 4 * height bytes). Each is called from an N-API
// async worker (background thread), so they must be self-contained and thread-safe.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace osrcap {

#if defined(_WIN32) || defined(__APPLE__)
// Windows: `handle` is a HANDLE to the shared D3D11 texture.
// macOS:   `handle` is an IOSurface* (valid in this process).
bool ReadbackHandle(uintptr_t handle, uint32_t width, uint32_t height, std::vector<uint8_t>& out, std::string& err);
#elif defined(__linux__)
struct DmabufPlane {
    int fd;
    uint32_t stride;
    uint64_t offset;
    uint64_t size;
};
bool ReadbackDmabuf(const std::vector<DmabufPlane>& planes, uint64_t modifier, uint32_t width, uint32_t height, std::vector<uint8_t>& out, std::string& err);
#endif

}  // namespace osrcap
