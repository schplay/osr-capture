// Windows backend: Electron passes a HANDLE to a shared D3D11 texture. Open it on our own device, copy
// into a STAGING texture and Map it into a CPU buffer. All D3D use is serialized (the immediate context
// is not thread-safe and multiple outputs' async readbacks may overlap).
#include "osr_readback.h"

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstring>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace osrcap {

namespace {

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11Device1> g_device1;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11Texture2D> g_staging;
UINT g_stagingW = 0;
UINT g_stagingH = 0;
std::mutex g_mutex;

bool EnsureDevice() {
    if (g_device1) return true;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &g_device, &fl, &g_context);
    if (FAILED(hr)) return false;
    return SUCCEEDED(g_device.As(&g_device1));
}

bool EnsureStaging(UINT w, UINT h) {
    if (g_staging && g_stagingW == w && g_stagingH == h) return true;
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
    if (FAILED(g_device->CreateTexture2D(&d, nullptr, &tex))) return false;
    g_staging = tex;
    g_stagingW = w;
    g_stagingH = h;
    return true;
}

}  // namespace

bool ReadbackHandle(uintptr_t handle, uint32_t width, uint32_t height, std::vector<uint8_t>& out, std::string& err) {
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!EnsureDevice()) {
        err = "D3D11CreateDevice failed";
        return false;
    }

    ComPtr<ID3D11Texture2D> shared;
    HRESULT hr = g_device1->OpenSharedResource1(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&shared));
    if (FAILED(hr)) hr = g_device->OpenSharedResource(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&shared));
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

    g_context->CopyResource(g_staging.Get(), shared.Get());
    if (haveKeyedMutex) keyedMutex->ReleaseSync(0);

    D3D11_MAPPED_SUBRESOURCE map = {};
    hr = g_context->Map(g_staging.Get(), 0, D3D11_MAP_READ, 0, &map);
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
    g_context->Unmap(g_staging.Get(), 0);
    return true;
}

}  // namespace osrcap
