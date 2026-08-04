// Windows backend: Electron passes a HANDLE to a shared D3D11 texture. Open it, copy into a STAGING
// texture and Map it into a CPU buffer.
//
// To let multiple outputs' readbacks run in PARALLEL rather than serializing on one device/context, each
// readback borrows a context from a small pool. Every pool entry owns its own D3D11 device + immediate
// context + staging texture, so they are independent across threads (the immediate context is not
// thread-safe, but each is only ever used by one readback at a time). Concurrency is naturally bounded by
// libuv's async-work thread pool, so the pool grows only to that many entries.
#include "osr_readback.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace osrcap {

namespace {

struct ReadbackContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    UINT stagingW = 0;
    UINT stagingH = 0;
    bool inUse = false;

    bool EnsureDevice() {
        if (device1) return true;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, &fl, &context);
        if (FAILED(hr)) return false;
        return SUCCEEDED(device.As(&device1));
    }

    bool EnsureStaging(UINT w, UINT h) {
        if (staging && stagingW == w && stagingH == h) return true;
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &tex))) return false;
        staging = tex;
        stagingW = w;
        stagingH = h;
        return true;
    }

    bool Readback(uintptr_t handle, uint32_t width, uint32_t height, std::vector<uint8_t>& out, std::string& err) {
        if (!EnsureDevice()) {
            err = "D3D11CreateDevice failed";
            return false;
        }

        ComPtr<ID3D11Texture2D> shared;
        HRESULT hr = device1->OpenSharedResource1(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&shared));
        if (FAILED(hr)) hr = device->OpenSharedResource(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&shared));
        if (FAILED(hr) || !shared) {
            err = "OpenSharedResource failed";
            return false;
        }

        ComPtr<IDXGIKeyedMutex> keyedMutex;
        bool haveKeyedMutex = SUCCEEDED(shared.As(&keyedMutex)) && keyedMutex;
        if (haveKeyedMutex && keyedMutex->AcquireSync(0, 1000) != S_OK) {
            err = "keyed mutex AcquireSync failed/timed out";
            return false;
        }

        if (!EnsureStaging(width, height)) {
            if (haveKeyedMutex) keyedMutex->ReleaseSync(0);
            err = "staging texture creation failed";
            return false;
        }

        context->CopyResource(staging.Get(), shared.Get());
        if (haveKeyedMutex) keyedMutex->ReleaseSync(0);

        D3D11_MAPPED_SUBRESOURCE map = {};
        hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
        if (FAILED(hr)) {
            err = "Map failed";
            return false;
        }

        const size_t rowBytes = static_cast<size_t>(width) * 4;
        out.resize(rowBytes * height);
        const uint8_t* src = static_cast<const uint8_t*>(map.pData);
        for (UINT y = 0; y < height; ++y) {
            std::memcpy(out.data() + static_cast<size_t>(y) * rowBytes, src + static_cast<size_t>(y) * map.RowPitch, rowBytes);
        }
        context->Unmap(staging.Get(), 0);
        return true;
    }
};

constexpr size_t kMaxPool = 16;
std::vector<std::unique_ptr<ReadbackContext>> g_pool;  // unique_ptr keeps entries pointer-stable across growth
std::mutex g_poolMutex;
std::condition_variable g_poolCv;

// borrow an idle context (creating one if the pool hasn't reached its cap); block if all are busy
ReadbackContext* AcquireContext() {
    std::unique_lock<std::mutex> lock(g_poolMutex);
    for (;;) {
        for (auto& c : g_pool) {
            if (!c->inUse) {
                c->inUse = true;
                return c.get();
            }
        }
        if (g_pool.size() < kMaxPool) {
            g_pool.push_back(std::make_unique<ReadbackContext>());
            g_pool.back()->inUse = true;
            return g_pool.back().get();
        }
        g_poolCv.wait(lock);
    }
}

void ReleaseContext(ReadbackContext* ctx) {
    {
        std::lock_guard<std::mutex> lock(g_poolMutex);
        ctx->inUse = false;
    }
    g_poolCv.notify_one();
}

}  // namespace

bool ReadbackHandle(uintptr_t handle, uint32_t width, uint32_t height, std::vector<uint8_t>& out, std::string& err) {
    // device creation / OpenSharedResource / CopyResource / Map all run on the borrowed context, off the
    // shared pool lock, so distinct outputs read back concurrently.
    ReadbackContext* ctx = AcquireContext();
    bool ok = ctx->Readback(handle, width, height, out, err);
    ReleaseContext(ctx);
    return ok;
}

}  // namespace osrcap
