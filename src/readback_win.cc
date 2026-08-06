// Windows backend: open Electron's shared D3D11 texture, optionally convert BGRA -> UYVY/UYVA on the GPU
// with a compute shader, and read the result back to a CPU buffer.
//
// format: 0 = BGRA (raw copy), 1 = UYVY (opaque, width*2*height), 2 = UYVA (UYVY plane + width*height alpha).
// For UYVY/UYVA the conversion runs on the GPU (no CPU colour-conversion traffic) and the smaller converted
// buffer is what gets read back. Each pool entry owns an independent device/context so outputs run in
// parallel; the immediate context is only ever used by one readback at a time.
#include "osr_readback.h"

#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <smmintrin.h>  // SSE4.1: _mm_stream_load_si128 (MOVNTDQA) for fast reads of WC GPU-mapped memory

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {
// Copy FROM GPU-mapped memory with MOVNTDQA streaming loads. `src` must be 16-byte aligned.
inline void StreamCopyFromWC(void* dst, const void* src, size_t bytes) {
    auto* s = static_cast<const __m128i*>(src);
    auto* d = static_cast<__m128i*>(dst);
    size_t blocks = bytes / 16;
    for (size_t i = 0; i < blocks; ++i) {
        __m128i v = _mm_stream_load_si128(s + i);
        _mm_storeu_si128(d + i, v);
    }
    _mm_sfence();
    size_t done = blocks * 16;
    if (done < bytes) std::memcpy(static_cast<char*>(dst) + done, static_cast<const char*>(src) + done, bytes - done);
}

// Reading the GPU-mapped readback staging is LATENCY-bound (BAR/VRAM over PCIe): a single thread stalls on
// each transaction (~400 MB/s), so a 4K frame's ~16-25MB takes 60-80ms. Splitting the read across threads
// overlaps the latencies and multiplies effective throughput. Chunks are 16-byte aligned for MOVNTDQA.
inline void ParallelStreamCopyFromWC(void* dst, const void* src, size_t bytes, unsigned threads) {
    if (threads <= 1 || bytes < (1u << 16)) {
        StreamCopyFromWC(dst, src, bytes);
        return;
    }
    size_t chunk = (bytes / threads) & ~size_t(15);
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (unsigned t = 1; t < threads; ++t) {
        size_t off = static_cast<size_t>(t) * chunk;
        size_t len = (t == threads - 1) ? bytes - off : chunk;
        pool.emplace_back([=]() { StreamCopyFromWC(static_cast<char*>(dst) + off, static_cast<const char*>(src) + off, len); });
    }
    StreamCopyFromWC(dst, src, chunk);  // this thread does chunk 0
    for (auto& th : pool) th.join();
}
}  // namespace

using Microsoft::WRL::ComPtr;

namespace osrcap {

namespace {

// BGRA -> UYVY (+ optional alpha plane). Each thread handles 4 horizontal pixels so every write to the raw
// output buffer is 4-byte aligned (2 UYVY words + 1 packed-alpha word). BT.601 full range, integer.
const char* kConvertHLSL = R"HLSL(
Texture2D<float4> gSrc : register(t0);
RWByteAddressBuffer gDst : register(u0);
cbuffer Params : register(b0) {
    uint gWidth;
    uint gHeight;
    uint gUyvySize;
    uint gWriteAlpha;
};
int toByte(float v) { return (int)(v * 255.0 + 0.5); }
[numthreads(16, 16, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint px = tid.x * 4;
    uint y = tid.y;
    if (px >= gWidth || y >= gHeight) return;
    float4 c0 = gSrc.Load(int3(px + 0, y, 0));
    float4 c1 = gSrc.Load(int3(px + 1, y, 0));
    float4 c2 = gSrc.Load(int3(px + 2, y, 0));
    float4 c3 = gSrc.Load(int3(px + 3, y, 0));
    int r0=toByte(c0.r),g0=toByte(c0.g),b0=toByte(c0.b);
    int r1=toByte(c1.r),g1=toByte(c1.g),b1=toByte(c1.b);
    int r2=toByte(c2.r),g2=toByte(c2.g),b2=toByte(c2.b);
    int r3=toByte(c3.r),g3=toByte(c3.g),b3=toByte(c3.b);
    int y0=(77*r0+150*g0+29*b0)>>8;
    int y1=(77*r1+150*g1+29*b1)>>8;
    int y2=(77*r2+150*g2+29*b2)>>8;
    int y3=(77*r3+150*g3+29*b3)>>8;
    int u01=clamp(((-43*r0-85*g0+128*b0)>>8)+128,0,255);
    int v01=clamp(((128*r0-107*g0-21*b0)>>8)+128,0,255);
    int u23=clamp(((-43*r2-85*g2+128*b2)>>8)+128,0,255);
    int v23=clamp(((128*r2-107*g2-21*b2)>>8)+128,0,255);
    uint word0 = (uint)u01 | ((uint)y0<<8) | ((uint)v01<<16) | ((uint)y1<<24);
    uint word1 = (uint)u23 | ((uint)y2<<8) | ((uint)v23<<16) | ((uint)y3<<24);
    uint uyvyOffset = y * gWidth * 2 + tid.x * 8;
    gDst.Store(uyvyOffset, word0);
    gDst.Store(uyvyOffset + 4, word1);
    if (gWriteAlpha) {
        uint aword = (uint)toByte(c0.a) | ((uint)toByte(c1.a)<<8) | ((uint)toByte(c2.a)<<16) | ((uint)toByte(c3.a)<<24);
        gDst.Store(gUyvySize + y * gWidth + tid.x * 4, aword);
    }
}
)HLSL";

struct Params {
    uint32_t width;
    uint32_t height;
    uint32_t uyvySize;
    uint32_t writeAlpha;
};

// Box-filter downscale of the shared BGRA texture straight to a small tightly-packed BGRA buffer, ON THE GPU.
// Used for server/stage/preview consumers of a mixed output: instead of reading back the full 4K frame and
// downscaling on the CPU, the GPU produces a small buffer so only a few MB cross PCIe. Output byte order is
// B,G,R,A (matches the CPU BGRA readback + nativeImage.createFromBitmap). Src/Dst dims fit in uint32 for 4K.
const char* kDownscaleHLSL = R"HLSL(
Texture2D<float4> gSrc : register(t0);
RWByteAddressBuffer gDst : register(u0);
cbuffer DsParams : register(b0) {
    uint gSrcW; uint gSrcH; uint gDstW; uint gDstH;
};
int toByte(float v) { return (int)(v * 255.0 + 0.5); }
[numthreads(8, 8, 1)]
void DSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gDstW || tid.y >= gDstH) return;
    uint sx0 = tid.x * gSrcW / gDstW;
    uint sx1 = (tid.x + 1) * gSrcW / gDstW; if (sx1 <= sx0) sx1 = sx0 + 1;
    uint sy0 = tid.y * gSrcH / gDstH;
    uint sy1 = (tid.y + 1) * gSrcH / gDstH; if (sy1 <= sy0) sy1 = sy0 + 1;
    float4 acc = float4(0, 0, 0, 0);
    uint cnt = 0;
    for (uint y = sy0; y < sy1; ++y) {
        for (uint x = sx0; x < sx1; ++x) { acc += gSrc.Load(int3(x, y, 0)); ++cnt; }
    }
    acc /= cnt;
    uint word = (uint)toByte(acc.b) | ((uint)toByte(acc.g) << 8) | ((uint)toByte(acc.r) << 16) | ((uint)toByte(acc.a) << 24);
    gDst.Store((tid.y * gDstW + tid.x) * 4, word);
}
)HLSL";

struct DsParams {
    uint32_t srcW;
    uint32_t srcH;
    uint32_t dstW;
    uint32_t dstH;
};

struct ReadbackContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11Buffer> paramsCb;

    // BGRA path staging
    ComPtr<ID3D11Texture2D> staging;
    UINT stagingW = 0, stagingH = 0;

    // compute path output buffer + its staging
    ComPtr<ID3D11Buffer> outBuf;
    ComPtr<ID3D11UnorderedAccessView> outUav;
    ComPtr<ID3D11Buffer> outStaging;
    size_t outBufSize = 0;

    // GPU-downscale (server/stage) path: its own shader + output buffer/staging
    ComPtr<ID3D11ComputeShader> dsShader;
    ComPtr<ID3D11Buffer> dsParamsCb;
    ComPtr<ID3D11Buffer> dsOutBuf;
    ComPtr<ID3D11UnorderedAccessView> dsOutUav;
    ComPtr<ID3D11Buffer> dsOutStaging;
    size_t dsOutBufSize = 0;

    bool inUse = false;

    // two-phase state: GPU pipeline fence + the pending result waiting to be copied out
    ComPtr<ID3D11Query> flushQuery;
    size_t pendingTotal = 0;
    bool pendingIsConvert = false;
    std::vector<uint8_t> pendingBgra;
    // pending GPU-downscaled buffer (0 = none for this frame); dsOutStaging holds it after the GPU wait
    size_t pendingScaledTotal = 0;

    bool EnsureDevice() {
        if (device1) return true;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &device, &fl, &context))) return false;
        return SUCCEEDED(device.As(&device1));
    }

    bool EnsureShader() {
        if (shader) return true;
        ComPtr<ID3DBlob> blob, errBlob;
        HRESULT hr = D3DCompile(kConvertHLSL, strlen(kConvertHLSL), "convert.hlsl", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &blob, &errBlob);
        if (FAILED(hr)) return false;
        if (FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader))) return false;
        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = sizeof(Params);
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(device->CreateBuffer(&cb, nullptr, &paramsCb));
    }

    bool EnsureStaging(UINT w, UINT h) {
        if (staging && stagingW == w && stagingH == h) return true;
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &tex))) return false;
        staging = tex; stagingW = w; stagingH = h;
        return true;
    }

    bool EnsureOutBuffer(size_t bytes) {
        if (outBuf && outBufSize == bytes) return true;
        outBuf.Reset(); outUav.Reset(); outStaging.Reset();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (UINT)bytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &outBuf))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements = (UINT)(bytes / 4);
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(device->CreateUnorderedAccessView(outBuf.Get(), &ud, &outUav))) return false;
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = (UINT)bytes;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateBuffer(&sd, nullptr, &outStaging))) return false;
        outBufSize = bytes;
        return true;
    }

    bool EnsureDsShader() {
        if (dsShader) return true;
        ComPtr<ID3DBlob> blob, errBlob;
        HRESULT hr = D3DCompile(kDownscaleHLSL, strlen(kDownscaleHLSL), "downscale.hlsl", nullptr, nullptr, "DSMain", "cs_5_0", 0, 0, &blob, &errBlob);
        if (FAILED(hr)) return false;
        if (FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &dsShader))) return false;
        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = sizeof(DsParams);
        cb.Usage = D3D11_USAGE_DEFAULT;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return SUCCEEDED(device->CreateBuffer(&cb, nullptr, &dsParamsCb));
    }

    bool EnsureDsOutBuffer(size_t bytes) {
        if (dsOutBuf && dsOutBufSize == bytes) return true;
        dsOutBuf.Reset(); dsOutUav.Reset(); dsOutStaging.Reset();
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = (UINT)bytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &dsOutBuf))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.FirstElement = 0;
        ud.Buffer.NumElements = (UINT)(bytes / 4);
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(device->CreateUnorderedAccessView(dsOutBuf.Get(), &ud, &dsOutUav))) return false;
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = (UINT)bytes;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateBuffer(&sd, nullptr, &dsOutStaging))) return false;
        dsOutBufSize = bytes;
        return true;
    }

    // GPU box-downscale `shared` (BGRA) -> dsOutStaging (small tightly-packed BGRA). Queues GPU work only;
    // the caller's WaitGpu() covers completion, and ReadPending() maps dsOutStaging out.
    bool DownscaleToStaging(ID3D11Texture2D* shared, uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH, std::string& err) {
        if (!EnsureDsShader()) { err = "downscale shader init failed"; return false; }
        if (!EnsureDsOutBuffer((size_t)dstW * dstH * 4)) { err = "downscale output buffer creation failed"; return false; }
        ComPtr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(shared, &sv, &srv))) { err = "downscale SRV creation failed"; return false; }
        DsParams p{ srcW, srcH, dstW, dstH };
        context->UpdateSubresource(dsParamsCb.Get(), 0, nullptr, &p, 0, 0);
        ID3D11ShaderResourceView* srvs[] = { srv.Get() };
        ID3D11UnorderedAccessView* uavs[] = { dsOutUav.Get() };
        ID3D11Buffer* cbs[] = { dsParamsCb.Get() };
        context->CSSetShader(dsShader.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->CSSetConstantBuffers(0, 1, cbs);
        context->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
        ID3D11ShaderResourceView* nullSrv[] = { nullptr };
        ID3D11UnorderedAccessView* nullUav[] = { nullptr };
        context->CSSetShaderResources(0, 1, nullSrv);
        context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        context->CopyResource(dsOutStaging.Get(), dsOutBuf.Get());
        return true;
    }

    bool ReadbackBgra(ID3D11Texture2D* shared, uint32_t width, uint32_t height, std::vector<uint8_t>& out, std::string& err) {
        if (!EnsureStaging(width, height)) { err = "staging texture creation failed"; return false; }
        context->CopyResource(staging.Get(), shared);
        D3D11_MAPPED_SUBRESOURCE map = {};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) { err = "Map failed"; return false; }
        const size_t rowBytes = (size_t)width * 4;
        out.resize(rowBytes * height);
        const uint8_t* src = (const uint8_t*)map.pData;
        // stream-copy each row: map.pData / RowPitch are 16-byte aligned (WC memory) -> use MOVNTDQA
        for (UINT y = 0; y < height; ++y) StreamCopyFromWC(out.data() + (size_t)y * rowBytes, src + (size_t)y * map.RowPitch, rowBytes);
        context->Unmap(staging.Get(), 0);
        return true;
    }

    bool ReadbackConvert(ID3D11Texture2D* shared, uint32_t width, uint32_t height, bool alpha, std::vector<uint8_t>& out, std::string& err) {
        if (!EnsureShader()) { err = "compute shader init failed"; return false; }
        const size_t uyvySize = (size_t)width * 2 * height;
        const size_t total = alpha ? uyvySize + (size_t)width * height : uyvySize;
        if (!EnsureOutBuffer(total)) { err = "output buffer creation failed"; return false; }

        ComPtr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(shared, &sv, &srv))) { err = "SRV creation failed"; return false; }

        Params p{ width, height, (uint32_t)uyvySize, alpha ? 1u : 0u };
        context->UpdateSubresource(paramsCb.Get(), 0, nullptr, &p, 0, 0);

        ID3D11ShaderResourceView* srvs[] = { srv.Get() };
        ID3D11UnorderedAccessView* uavs[] = { outUav.Get() };
        ID3D11Buffer* cbs[] = { paramsCb.Get() };
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->CSSetConstantBuffers(0, 1, cbs);
        const UINT gx = (width / 4 + 15) / 16;
        const UINT gy = (height + 15) / 16;
        context->Dispatch(gx, gy, 1);

        ID3D11ShaderResourceView* nullSrv[] = { nullptr };
        ID3D11UnorderedAccessView* nullUav[] = { nullptr };
        context->CSSetShaderResources(0, 1, nullSrv);
        context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);

        context->CopyResource(outStaging.Get(), outBuf.Get());
        D3D11_MAPPED_SUBRESOURCE map = {};
        // Map blocks until the GPU has finished the dispatch + copy
        if (FAILED(context->Map(outStaging.Get(), 0, D3D11_MAP_READ, 0, &map))) { err = "Map failed"; return false; }
        out.resize(total);
        ParallelStreamCopyFromWC(out.data(), map.pData, total, 8);
        context->Unmap(outStaging.Get(), 0);
        return true;
    }

    bool Readback(uintptr_t handle, uint32_t width, uint32_t height, int format, std::vector<uint8_t>& out, std::string& err) {
        if (!EnsureDevice()) { err = "D3D11CreateDevice failed"; return false; }

        ComPtr<ID3D11Texture2D> shared;
        HRESULT hr = device1->OpenSharedResource1(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&shared));
        if (FAILED(hr)) hr = device->OpenSharedResource(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&shared));
        if (FAILED(hr) || !shared) { err = "OpenSharedResource failed"; return false; }

        ComPtr<IDXGIKeyedMutex> keyedMutex;
        bool haveKeyedMutex = SUCCEEDED(shared.As(&keyedMutex)) && keyedMutex;
        if (haveKeyedMutex && keyedMutex->AcquireSync(0, 1000) != S_OK) { err = "keyed mutex AcquireSync failed/timed out"; return false; }

        bool ok;
        if (format == 1 || format == 2) ok = ReadbackConvert(shared.Get(), width, height, format == 2, out, err);
        else ok = ReadbackBgra(shared.Get(), width, height, out, err);

        if (haveKeyedMutex) keyedMutex->ReleaseSync(0);
        return ok;
    }

    // ---- two-phase: consume (GPU, releases the shared texture fast) then read (slow PCIe copy) ----

    // Block until the GPU has finished all queued work (so the shared texture has been fully read).
    void WaitGpu() {
        if (!flushQuery) {
            D3D11_QUERY_DESC qd = {};
            qd.Query = D3D11_QUERY_EVENT;
            device->CreateQuery(&qd, &flushQuery);
        }
        context->End(flushQuery.Get());
        context->Flush();
        while (context->GetData(flushQuery.Get(), nullptr, 0, 0) != S_OK) std::this_thread::yield();
    }

    // GPU-convert the shared texture into outStaging (no CPU read yet). Mirrors ReadbackConvert up to the copy.
    bool ConvertToStaging(ID3D11Texture2D* shared, uint32_t width, uint32_t height, bool alpha, std::string& err) {
        if (!EnsureShader()) { err = "compute shader init failed"; return false; }
        const size_t uyvySize = (size_t)width * 2 * height;
        const size_t total = alpha ? uyvySize + (size_t)width * height : uyvySize;
        if (!EnsureOutBuffer(total)) { err = "output buffer creation failed"; return false; }

        ComPtr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateShaderResourceView(shared, &sv, &srv))) { err = "SRV creation failed"; return false; }

        Params p{ width, height, (uint32_t)uyvySize, alpha ? 1u : 0u };
        context->UpdateSubresource(paramsCb.Get(), 0, nullptr, &p, 0, 0);
        ID3D11ShaderResourceView* srvs[] = { srv.Get() };
        ID3D11UnorderedAccessView* uavs[] = { outUav.Get() };
        ID3D11Buffer* cbs[] = { paramsCb.Get() };
        context->CSSetShader(shader.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, srvs);
        context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
        context->CSSetConstantBuffers(0, 1, cbs);
        context->Dispatch((width / 4 + 15) / 16, (height + 15) / 16, 1);
        ID3D11ShaderResourceView* nullSrv[] = { nullptr };
        ID3D11UnorderedAccessView* nullUav[] = { nullptr };
        context->CSSetShaderResources(0, 1, nullSrv);
        context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
        context->CopyResource(outStaging.Get(), outBuf.Get());
        return true;
    }

    // Phase 1: open + GPU-convert into staging, then WAIT for the GPU so the caller can release the shared
    // texture immediately (short — ~the GPU time). The slow PCIe read is deferred to ReadPending.
    // dstW/dstH > 0 additionally GPU-downscales the shared BGRA to a small buffer (for server/stage) in the
    // SAME GPU pass, so a mixed output reads back UYVY (NDI) + a few-MB scaled BGRA instead of the full frame.
    bool ConsumeShared(uintptr_t handle, uint32_t width, uint32_t height, int format, uint32_t dstW, uint32_t dstH, std::string& err) {
        if (!EnsureDevice()) { err = "D3D11CreateDevice failed"; return false; }
        ComPtr<ID3D11Texture2D> shared;
        HRESULT hr = device1->OpenSharedResource1(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&shared));
        if (FAILED(hr)) hr = device->OpenSharedResource(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&shared));
        if (FAILED(hr) || !shared) { err = "OpenSharedResource failed"; return false; }

        ComPtr<IDXGIKeyedMutex> keyedMutex;
        bool haveKeyedMutex = SUCCEEDED(shared.As(&keyedMutex)) && keyedMutex;
        if (haveKeyedMutex && keyedMutex->AcquireSync(0, 1000) != S_OK) { err = "keyed mutex AcquireSync failed/timed out"; return false; }

        bool ok = true;
        if (format == 1 || format == 2) {
            pendingIsConvert = true;
            const size_t uyvySize = (size_t)width * 2 * height;
            pendingTotal = (format == 2) ? uyvySize + (size_t)width * height : uyvySize;
            ok = ConvertToStaging(shared.Get(), width, height, format == 2, err);
        } else {
            // BGRA (multi-consumer / non-off-main): no split benefit; do the full read now into a vector.
            pendingIsConvert = false;
            ok = ReadbackBgra(shared.Get(), width, height, pendingBgra, err);
            pendingTotal = pendingBgra.size();
        }
        // optional GPU downscale for server/stage, queued before the single WaitGpu below
        pendingScaledTotal = 0;
        if (ok && dstW > 0 && dstH > 0) {
            if (DownscaleToStaging(shared.Get(), width, height, dstW, dstH, err)) pendingScaledTotal = (size_t)dstW * dstH * 4;
            else ok = false;
        }
        WaitGpu();  // ensure the GPU is done reading `shared` before it is released by the caller
        if (haveKeyedMutex) keyedMutex->ReleaseSync(0);
        return ok;
    }

    // Phase 2: copy the pending result into `dst` (the slow WC/PCIe read for the convert path). When a GPU
    // downscale was requested, also copy the small scaled BGRA into `scaledDst` (cheap — only a few MB).
    bool ReadPending(uint8_t* dst, size_t dstSize, uint8_t* scaledDst, size_t scaledSize, std::string& err) {
        size_t n = pendingTotal < dstSize ? pendingTotal : dstSize;
        if (!pendingIsConvert) {
            std::memcpy(dst, pendingBgra.data(), n);
            pendingBgra.clear();
        } else {
            D3D11_MAPPED_SUBRESOURCE map = {};
            if (FAILED(context->Map(outStaging.Get(), 0, D3D11_MAP_READ, 0, &map))) { err = "Map failed"; return false; }
            ParallelStreamCopyFromWC(dst, map.pData, n, 8);
            context->Unmap(outStaging.Get(), 0);
        }
        if (pendingScaledTotal > 0 && scaledDst && scaledSize > 0) {
            size_t sn = pendingScaledTotal < scaledSize ? pendingScaledTotal : scaledSize;
            D3D11_MAPPED_SUBRESOURCE smap = {};
            if (FAILED(context->Map(dsOutStaging.Get(), 0, D3D11_MAP_READ, 0, &smap))) { err = "scaled Map failed"; return false; }
            ParallelStreamCopyFromWC(scaledDst, smap.pData, sn, 4);
            context->Unmap(dsOutStaging.Get(), 0);
        }
        pendingScaledTotal = 0;
        return true;
    }
};

constexpr size_t kMaxPool = 16;
std::vector<std::unique_ptr<ReadbackContext>> g_pool;
std::mutex g_poolMutex;
std::condition_variable g_poolCv;

ReadbackContext* AcquireContext() {
    std::unique_lock<std::mutex> lock(g_poolMutex);
    for (;;) {
        for (auto& c : g_pool) {
            if (!c->inUse) { c->inUse = true; return c.get(); }
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

// contexts holding a consumed-but-not-yet-read frame, keyed by the caller's key (output id)
std::map<std::string, ReadbackContext*> g_pending;
std::mutex g_pendingMutex;

}  // namespace

bool ReadbackHandle(uintptr_t handle, uint32_t width, uint32_t height, int format, std::vector<uint8_t>& out, std::string& err) {
    ReadbackContext* ctx = AcquireContext();
    bool ok = ctx->Readback(handle, width, height, format, out, err);
    ReleaseContext(ctx);
    return ok;
}

bool ReadbackConsume(uintptr_t handle, uint32_t width, uint32_t height, int format, const std::string& key, uint32_t dstW, uint32_t dstH, std::string& err) {
    ReadbackContext* ctx = AcquireContext();
    bool ok = ctx->ConsumeShared(handle, width, height, format, dstW, dstH, err);
    if (!ok) {
        ReleaseContext(ctx);
        return false;
    }
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    auto it = g_pending.find(key);
    if (it != g_pending.end()) ReleaseContext(it->second);  // stale (shouldn't happen with single-in-flight)
    g_pending[key] = ctx;
    return true;
}

bool ReadbackFinish(const std::string& key, uint8_t* dst, size_t dstSize, uint8_t* scaledDst, size_t scaledSize, std::string& err) {
    ReadbackContext* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        auto it = g_pending.find(key);
        if (it == g_pending.end()) {
            err = "no pending readback for key";
            return false;
        }
        ctx = it->second;
        g_pending.erase(it);
    }
    bool ok = ctx->ReadPending(dst, dstSize, scaledDst, scaledSize, err);
    ReleaseContext(ctx);
    return ok;
}

void ReadbackReleaseKey(const std::string& key) {
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    auto it = g_pending.find(key);
    if (it != g_pending.end()) {
        ReleaseContext(it->second);
        g_pending.erase(it);
    }
}

}  // namespace osrcap
