// Platform readback backends: given a shared-texture handle (Windows/macOS) or dmabuf planes (Linux),
// produce a tightly-packed BGRA/RGBA buffer (width * 4 * height bytes). Each is called from an N-API
// async worker (background thread), so they must be self-contained and thread-safe.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace osrcap {

// format: 0 = BGRA (raw), 1 = UYVY (opaque), 2 = UYVA (colour + alpha). Only Windows honours 1/2 (GPU
// convert); mac/linux produce BGRA regardless, so callers request 0 there.
#if defined(_WIN32) || defined(__APPLE__)
// Windows: `handle` is a HANDLE to the shared D3D11 texture.
// macOS:   `handle` is an IOSurface* (valid in this process).
bool ReadbackHandle(uintptr_t handle, uint32_t width, uint32_t height, int format, std::vector<uint8_t>& out, std::string& err);
#endif

#if defined(_WIN32)
// Two-phase readback (Windows), so the CALLER can release the Electron shared texture as soon as the GPU has
// consumed it — instead of pinning it for the whole (slow) PCIe read-back, which drains Electron's frame pool
// and stalls the main process. Consume: open the shared texture, GPU-convert into an internal buffer keyed by
// `key`, and return only once the GPU has finished reading the shared texture. Finish: copy that buffer to
// `out` (the slow part). A `key` may have at most one consume outstanding at a time.
// dstW/dstH > 0 also GPU-downscales the shared BGRA into a small buffer (server/stage) in the same pass;
// ReadbackFinish then fills `scaledDst` (scaledSize bytes) with that tightly-packed BGRA. Pass 0 / nullptr to skip.
bool ReadbackConsume(uintptr_t handle, uint32_t width, uint32_t height, int format, const std::string& key, uint32_t dstW, uint32_t dstH, std::string& err);
bool ReadbackFinish(const std::string& key, uint8_t* dst, size_t dstSize, uint8_t* scaledDst, size_t scaledSize, std::string& err);
void ReadbackReleaseKey(const std::string& key);  // drop any pending consume for `key` (cleanup)
#endif

#if defined(__linux__)
struct DmabufPlane {
    int fd;
    uint32_t stride;
    uint64_t offset;
    uint64_t size;
};
bool ReadbackDmabuf(const std::vector<DmabufPlane>& planes, uint64_t modifier, uint32_t width, uint32_t height, int format, std::vector<uint8_t>& out, std::string& err);
#endif

}  // namespace osrcap
