// macOS backend: Electron passes the shared texture as an IOSurface* (uintptr_t). IOSurfaces are
// CPU-mappable, so we lock, copy rows (accounting for row padding), and unlock. We do NOT own the
// surface (Electron holds it until texture.release()), so we only lock/read/unlock.
#include "osr_readback.h"

#import <IOSurface/IOSurface.h>

#include <cstring>

#include "convert.h"

namespace osrcap {

bool ReadbackHandle(uintptr_t handle, uint32_t width, uint32_t height, int format, std::vector<uint8_t>& out, std::string& err) {
    // The IOSurface readback is BGRA. When the caller asks for UYVY/UYVA we convert on the CPU here
    // (ConvertBgraInPlace) so macOS gets the same reduced NDI/SDI wire format as Windows. A GPU (Metal
    // compute) convert on the IOSurface is a future optimization — see osr-capture HANDOFF.
    IOSurfaceRef surface = reinterpret_cast<IOSurfaceRef>(handle);
    if (!surface) {
        err = "null IOSurface handle";
        return false;
    }

    IOReturn lockResult = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
    if (lockResult != kIOReturnSuccess) {
        err = "IOSurfaceLock failed";
        return false;
    }

    uint8_t* base = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(surface));
    const size_t rowPitch = IOSurfaceGetBytesPerRow(surface);
    if (!base) {
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
        err = "IOSurfaceGetBaseAddress returned null";
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    out.resize(rowBytes * height);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(out.data() + static_cast<size_t>(y) * rowBytes, base + static_cast<size_t>(y) * rowPitch, rowBytes);
    }

    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);

    // format 1 = UYVY, 2 = UYVA: convert the BGRA we just read; 0 leaves it as BGRA.
    ConvertBgraInPlace(out, width, height, format);
    return true;
}

}  // namespace osrcap
