// osr-capture: read back an Electron OSR shared GPU texture into a CPU BGRA buffer, off the main thread.
//
// The GPU->CPU copy runs in an N-API AsyncWorker (a libuv background thread) so it never blocks the JS
// main thread. Per-platform readback lives in readback_{win,mac,linux}; this file is the N-API surface.
//
// Windows/macOS: readback(handle: Buffer /* uintptr_t */, width, height) => Promise<Buffer>
// Linux:         readback({ planes: {fd,stride,offset,size}[], modifier }, width, height) => Promise<Buffer>

#include <napi.h>

#include <cstring>

#include "osr_readback.h"

namespace {

class ReadbackWorker : public Napi::AsyncWorker {
public:
#if defined(_WIN32) || defined(__APPLE__)
    ReadbackWorker(Napi::Env env, uintptr_t handle, uint32_t w, uint32_t h, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), handle_(handle), w_(w), h_(h), deferred_(deferred) {}
#elif defined(__linux__)
    ReadbackWorker(Napi::Env env, std::vector<osrcap::DmabufPlane> planes, uint64_t modifier, uint32_t w, uint32_t h, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), planes_(std::move(planes)), modifier_(modifier), w_(w), h_(h), deferred_(deferred) {}
#endif

    void Execute() override {
        std::string err;
        bool ok = false;
#if defined(_WIN32) || defined(__APPLE__)
        ok = osrcap::ReadbackHandle(handle_, w_, h_, out_, err);
#elif defined(__linux__)
        ok = osrcap::ReadbackDmabuf(planes_, modifier_, w_, h_, out_, err);
#else
        err = "unsupported platform";
#endif
        if (!ok) SetError(err.empty() ? "readback failed" : err);
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        deferred_.Resolve(Napi::Buffer<uint8_t>::Copy(Env(), out_.data(), out_.size()));
    }

    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
#if defined(_WIN32) || defined(__APPLE__)
    uintptr_t handle_ = 0;
#elif defined(__linux__)
    std::vector<osrcap::DmabufPlane> planes_;
    uint64_t modifier_ = 0;
#endif
    uint32_t w_ = 0;
    uint32_t h_ = 0;
    Napi::Promise::Deferred deferred_;
    std::vector<uint8_t> out_;
};

Napi::Value Readback(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 3) {
        Napi::TypeError::New(env, "readback(handleOrInfo, width, height)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    uint32_t w = info[1].As<Napi::Number>().Uint32Value();
    uint32_t h = info[2].As<Napi::Number>().Uint32Value();
    auto deferred = Napi::Promise::Deferred::New(env);

#if defined(_WIN32) || defined(__APPLE__)
    uintptr_t handle = 0;
    Napi::Buffer<uint8_t> buf = info[0].As<Napi::Buffer<uint8_t>>();
    if (buf.Length() >= sizeof(uintptr_t)) std::memcpy(&handle, buf.Data(), sizeof(uintptr_t));
    (new ReadbackWorker(env, handle, w, h, deferred))->Queue();
#elif defined(__linux__)
    Napi::Object arg = info[0].As<Napi::Object>();
    std::vector<osrcap::DmabufPlane> planes;
    if (arg.Get("planes").IsArray()) {
        Napi::Array arr = arg.Get("planes").As<Napi::Array>();
        for (uint32_t i = 0; i < arr.Length(); ++i) {
            Napi::Object p = arr.Get(i).As<Napi::Object>();
            osrcap::DmabufPlane dp;
            dp.fd = p.Get("fd").As<Napi::Number>().Int32Value();
            dp.stride = p.Get("stride").As<Napi::Number>().Uint32Value();
            dp.offset = static_cast<uint64_t>(p.Get("offset").As<Napi::Number>().Int64Value());
            dp.size = static_cast<uint64_t>(p.Get("size").As<Napi::Number>().Int64Value());
            planes.push_back(dp);
        }
    }
    uint64_t modifier = 0;
    Napi::Value mod = arg.Get("modifier");
    if (mod.IsBigInt()) {
        bool lossless = false;
        modifier = mod.As<Napi::BigInt>().Uint64Value(&lossless);
    } else if (mod.IsNumber()) {
        modifier = static_cast<uint64_t>(mod.As<Napi::Number>().Int64Value());
    }
    (new ReadbackWorker(env, std::move(planes), modifier, w, h, deferred))->Queue();
#else
    deferred.Reject(Napi::Error::New(env, "unsupported platform").Value());
#endif

    return deferred.Promise();
}

}  // namespace

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    exports.Set("readback", Napi::Function::New(env, Readback));
    return exports;
}

NODE_API_MODULE(osr_readback, Init)
