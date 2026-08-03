// osr-capture: read back an Electron OSR shared GPU texture (D3D11, Windows) into a CPU BGRA buffer.
//
// Electron's offscreen `useSharedTexture` paint event provides `event.texture.textureInfo` with an
// 8-byte `sharedTextureHandle` Buffer (a Windows shared NT HANDLE, duplicated into this process),
// pixelFormat "bgra" and codedSize. This addon opens that shared texture, copies it into a STAGING
// texture and Maps it into a Node Buffer. The GPU->CPU copy+map is the expensive part, so it runs in an
// N-API AsyncWorker (a libuv background thread) instead of the main JS thread.
//
// readback(handle: Buffer, width: number, height: number) => Promise<Buffer>  // tightly-packed BGRA

#include <napi.h>

#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// Single shared device/context + staging texture. All D3D use is serialized by g_mutex because the
// D3D11 immediate context is not thread-safe and multiple outputs' async readbacks may overlap.
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
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, &g_device, &fl, &g_context);
    if (FAILED(hr)) return false;
    if (FAILED(g_device.As(&g_device1))) return false;
    return true;
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

class ReadbackWorker : public Napi::AsyncWorker {
public:
    ReadbackWorker(Napi::Env env, HANDLE handle, UINT w, UINT h, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), handle_(handle), w_(w), h_(h), deferred_(deferred) {}

    void Execute() override {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (!EnsureDevice()) {
            SetError("D3D11CreateDevice failed");
            return;
        }

        // The handle may be an NT handle (OpenSharedResource1) or a legacy shared handle
        // (OpenSharedResource); try the modern path first, then fall back.
        ComPtr<ID3D11Texture2D> shared;
        HRESULT hr = g_device1->OpenSharedResource1(handle_, IID_PPV_ARGS(&shared));
        if (FAILED(hr)) hr = g_device->OpenSharedResource(handle_, IID_PPV_ARGS(&shared));
        if (FAILED(hr) || !shared) {
            SetError("OpenSharedResource failed");
            return;
        }

        // Chromium shared images are often protected by a keyed mutex (key 0). Acquire it before the
        // copy if present; tolerate its absence.
        ComPtr<IDXGIKeyedMutex> keyedMutex;
        bool haveKeyedMutex = SUCCEEDED(shared.As(&keyedMutex)) && keyedMutex;
        if (haveKeyedMutex) {
            HRESULT acq = keyedMutex->AcquireSync(0, 1000);
            if (acq != S_OK) {
                SetError("keyed mutex AcquireSync failed/timed out");
                return;
            }
        }

        if (!EnsureStaging(w_, h_)) {
            if (haveKeyedMutex) keyedMutex->ReleaseSync(0);
            SetError("staging texture creation failed");
            return;
        }

        g_context->CopyResource(g_staging.Get(), shared.Get());
        if (haveKeyedMutex) keyedMutex->ReleaseSync(0);

        D3D11_MAPPED_SUBRESOURCE map = {};
        hr = g_context->Map(g_staging.Get(), 0, D3D11_MAP_READ, 0, &map);
        if (FAILED(hr)) {
            SetError("Map failed");
            return;
        }

        const size_t rowBytes = static_cast<size_t>(w_) * 4;
        result_.resize(rowBytes * h_);
        const uint8_t* src = static_cast<const uint8_t*>(map.pData);
        for (UINT y = 0; y < h_; ++y) {
            memcpy(result_.data() + static_cast<size_t>(y) * rowBytes, src + static_cast<size_t>(y) * map.RowPitch, rowBytes);
        }
        g_context->Unmap(g_staging.Get(), 0);
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        deferred_.Resolve(Napi::Buffer<uint8_t>::Copy(Env(), result_.data(), result_.size()));
    }

    void OnError(const Napi::Error& e) override {
        deferred_.Reject(e.Value());
    }

private:
    HANDLE handle_;
    UINT w_;
    UINT h_;
    Napi::Promise::Deferred deferred_;
    std::vector<uint8_t> result_;
};

Napi::Value Readback(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();

    if (info.Length() < 3 || !info[0].IsBuffer()) {
        Napi::TypeError::New(env, "readback(handle: Buffer, width, height)").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Napi::Buffer<uint8_t> handleBuf = info[0].As<Napi::Buffer<uint8_t>>();
    UINT w = info[1].As<Napi::Number>().Uint32Value();
    UINT h = info[2].As<Napi::Number>().Uint32Value();

    HANDLE handle = nullptr;
    if (handleBuf.Length() >= sizeof(void*)) memcpy(&handle, handleBuf.Data(), sizeof(void*));

    auto deferred = Napi::Promise::Deferred::New(env);
    (new ReadbackWorker(env, handle, w, h, deferred))->Queue();
    return deferred.Promise();
}

}  // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("readback", Napi::Function::New(env, Readback));
    return exports;
}

NODE_API_MODULE(osr_readback, Init)
