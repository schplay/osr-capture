// osr-capture: read back an Electron OSR shared GPU texture into a CPU BGRA buffer, off the main thread.
//
// The GPU->CPU copy runs in an N-API AsyncWorker (a libuv background thread) so it never blocks the JS
// main thread. Per-platform readback lives in readback_{win,mac,linux}; this file is the N-API surface.
//
// Windows/macOS: readback(handle: Buffer /* uintptr_t */, width, height) => Promise<Buffer>
// Linux:         readback({ planes: {fd,stride,offset,size}[], modifier }, width, height) => Promise<Buffer>

#include <napi.h>

#include <cstring>
#include <map>
#include <string>

#include "osr_readback.h"

namespace {

// Output byte size per format: 0=BGRA (w*4*h), 1=UYVY (w*2*h), 2=UYVA (w*2*h + w*h = w*3*h).
size_t OutputSize(uint32_t w, uint32_t h, int format) {
    size_t px = static_cast<size_t>(w) * h;
    if (format == 1) return px * 2;
    if (format == 2) return px * 3;
    return px * 4;
}

// Per-output pool of REUSED V8 output buffers, keyed by the caller's poolKey (FreeShow passes the output id).
// Reusing buffers avoids a per-frame ~16MB V8 allocation + zero-fill on the main thread (which caused GC
// jitter), and the worker fills the buffer off-thread so OnOK does no copy. POOL_N buffers rotate so the
// worker fills slot R%N while JS still holds the previous slot: with single-in-flight readback per output,
// a buffer is not overwritten for N readbacks, well after JS has finished reading it (send copies it out).
constexpr int POOL_N = 3;
struct BufferPool {
    size_t size = 0;
    int next = 0;
    Napi::Reference<Napi::Buffer<uint8_t>> bufs[POOL_N];
};

// Per-ENV pool map, held as N-API instance data. The addon is loaded in BOTH the main thread and the NDI
// worker thread. A single process-global map was (a) a data race — both threads insert/erase concurrently —
// and (b) worse, the worker's releasePool() could destroy Napi::References that were CREATED on the main env
// (main-path readback uses a bare `id` key; the worker uses `id#slot`), and destroying a reference on a
// different env than created it is undefined behaviour. Per-env instance data gives each thread its own pools,
// finalized on that env's own teardown (on the right thread), so references are only ever created and
// destroyed on their owning env, with no shared mutable state to race on.
struct AddonData {
    std::map<std::string, BufferPool> pools;
};

class ReadbackWorker : public Napi::AsyncWorker {
public:
#if defined(_WIN32) || defined(__APPLE__)
    ReadbackWorker(Napi::Env env, uintptr_t handle, uint32_t w, uint32_t h, int format, uint8_t* dst, size_t dstSize, Napi::Buffer<uint8_t> result, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), handle_(handle), w_(w), h_(h), format_(format), dst_(dst), dstSize_(dstSize), resultRef_(Napi::Persistent(result)), deferred_(deferred) {}
#elif defined(__linux__)
    ReadbackWorker(Napi::Env env, std::vector<osrcap::DmabufPlane> planes, uint64_t modifier, uint32_t w, uint32_t h, int format, uint8_t* dst, size_t dstSize, Napi::Buffer<uint8_t> result, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), planes_(std::move(planes)), modifier_(modifier), w_(w), h_(h), format_(format), dst_(dst), dstSize_(dstSize), resultRef_(Napi::Persistent(result)), deferred_(deferred) {}
#endif

    void Execute() override {
        std::string err;
        bool ok = false;
#if defined(_WIN32) || defined(__APPLE__)
        ok = osrcap::ReadbackHandle(handle_, w_, h_, format_, out_, err);
#elif defined(__linux__)
        ok = osrcap::ReadbackDmabuf(planes_, modifier_, w_, h_, format_, out_, err);
#else
        err = "unsupported platform";
#endif
        if (!ok) {
            SetError(err.empty() ? "readback failed" : err);
            return;
        }
        // Copy into the caller-owned V8 buffer HERE (libuv worker thread) so OnOK (main thread) does no copy —
        // the full-frame memcpy no longer congests the JS event loop.
        if (out_.size() != dstSize_) {
            SetError("readback produced " + std::to_string(out_.size()) + " bytes, expected " + std::to_string(dstSize_));
            return;
        }
        std::memcpy(dst_, out_.data(), out_.size());
    }

    void OnOK() override {
        Napi::HandleScope scope(Env());
        deferred_.Resolve(resultRef_.Value());
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
    int format_ = 0;
    uint8_t* dst_ = nullptr;
    size_t dstSize_ = 0;
    Napi::Reference<Napi::Buffer<uint8_t>> resultRef_;
    Napi::Promise::Deferred deferred_;
    std::vector<uint8_t> out_;
};

// Acquire the output buffer for this readback: a rotating pooled (reused) buffer when a poolKey is given,
// else a fresh per-call buffer (probe/other callers). Reallocates a pool when the frame size changes.
Napi::Buffer<uint8_t> AcquireOutputBuffer(Napi::Env env, const std::string& poolKey, size_t outSize) {
    if (poolKey.empty()) return Napi::Buffer<uint8_t>::New(env, outSize);
    AddonData* data = env.GetInstanceData<AddonData>();
    if (!data) return Napi::Buffer<uint8_t>::New(env, outSize);
    BufferPool& pool = data->pools[poolKey];
    if (pool.size != outSize) {
        for (int i = 0; i < POOL_N; ++i) pool.bufs[i] = Napi::Persistent(Napi::Buffer<uint8_t>::New(env, outSize));
        pool.size = outSize;
        pool.next = 0;
    }
    Napi::Buffer<uint8_t> b = pool.bufs[pool.next].Value();
    pool.next = (pool.next + 1) % POOL_N;
    return b;
}

Napi::Value Readback(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() < 3) {
        Napi::TypeError::New(env, "readback(handleOrInfo, width, height)").ThrowAsJavaScriptException();
        return env.Undefined();
    }
    uint32_t w = info[1].As<Napi::Number>().Uint32Value();
    uint32_t h = info[2].As<Napi::Number>().Uint32Value();
    int format = (info.Length() > 3 && info[3].IsNumber()) ? info[3].As<Napi::Number>().Int32Value() : 0;
    std::string poolKey = (info.Length() > 4 && info[4].IsString()) ? info[4].As<Napi::String>().Utf8Value() : std::string();
    auto deferred = Napi::Promise::Deferred::New(env);

    // Acquire the (reused, when pooled) output buffer up front; the worker fills it off-thread.
    size_t outSize = OutputSize(w, h, format);
    Napi::Buffer<uint8_t> result = AcquireOutputBuffer(env, poolKey, outSize);
    uint8_t* dst = result.Data();

#if defined(_WIN32) || defined(__APPLE__)
    uintptr_t handle = 0;
    Napi::Buffer<uint8_t> buf = info[0].As<Napi::Buffer<uint8_t>>();
    if (buf.Length() >= sizeof(uintptr_t)) std::memcpy(&handle, buf.Data(), sizeof(uintptr_t));
    (new ReadbackWorker(env, handle, w, h, format, dst, outSize, result, deferred))->Queue();
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
    (new ReadbackWorker(env, std::move(planes), modifier, w, h, format, dst, outSize, result, deferred))->Queue();
#else
    deferred.Reject(Napi::Error::New(env, "unsupported platform").Value());
#endif

    return deferred.Promise();
}

#if defined(_WIN32)
// Two-phase readback (Windows): consume opens+GPU-converts the shared texture and returns once the GPU has
// read it (so the caller can release the Electron texture); finish does the slow PCIe copy into a pooled
// buffer. This keeps the shared texture pinned only for the GPU phase, not the whole readback.
class ConsumeWorker : public Napi::AsyncWorker {
public:
    ConsumeWorker(Napi::Env env, uintptr_t handle, uint32_t w, uint32_t h, int format, std::string key, uint32_t dstW, uint32_t dstH, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), handle_(handle), w_(w), h_(h), format_(format), key_(std::move(key)), dstW_(dstW), dstH_(dstH), deferred_(deferred) {}
    void Execute() override {
        std::string err;
        if (!osrcap::ReadbackConsume(handle_, w_, h_, format_, key_, dstW_, dstH_, err)) SetError(err.empty() ? "consume failed" : err);
    }
    void OnOK() override {
        Napi::HandleScope s(Env());
        deferred_.Resolve(Env().Undefined());
    }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    uintptr_t handle_;
    uint32_t w_, h_;
    int format_;
    std::string key_;
    uint32_t dstW_, dstH_;
    Napi::Promise::Deferred deferred_;
};

class FinishWorker : public Napi::AsyncWorker {
public:
    // scaledDst/scaledSize + scaled buffer optional: when present, the promise resolves to { main, scaled }
    // (both reused pooled buffers); otherwise it resolves to just the main Buffer (backward-compatible).
    FinishWorker(Napi::Env env, std::string key, uint8_t* dst, size_t dstSize, Napi::Buffer<uint8_t> result, uint8_t* scaledDst, size_t scaledSize, bool hasScaled, Napi::Buffer<uint8_t> scaledResult, Napi::Promise::Deferred deferred)
        : Napi::AsyncWorker(env), key_(std::move(key)), dst_(dst), dstSize_(dstSize), resultRef_(Napi::Persistent(result)),
          scaledDst_(scaledDst), scaledSize_(scaledSize), hasScaled_(hasScaled), deferred_(deferred) {
        if (hasScaled) scaledRef_ = Napi::Persistent(scaledResult);
    }
    void Execute() override {
        std::string err;
        if (!osrcap::ReadbackFinish(key_, dst_, dstSize_, scaledDst_, scaledSize_, err)) SetError(err.empty() ? "finish failed" : err);
    }
    void OnOK() override {
        Napi::HandleScope s(Env());
        if (hasScaled_) {
            Napi::Object o = Napi::Object::New(Env());
            o.Set("main", resultRef_.Value());
            o.Set("scaled", scaledRef_.Value());
            deferred_.Resolve(o);
        } else {
            deferred_.Resolve(resultRef_.Value());
        }
    }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }

private:
    std::string key_;
    uint8_t* dst_;
    size_t dstSize_;
    Napi::Reference<Napi::Buffer<uint8_t>> resultRef_;
    uint8_t* scaledDst_;
    size_t scaledSize_;
    bool hasScaled_;
    Napi::Reference<Napi::Buffer<uint8_t>> scaledRef_;
    Napi::Promise::Deferred deferred_;
};

Napi::Value ReadbackConsumeJs(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    uint32_t w = info[1].As<Napi::Number>().Uint32Value();
    uint32_t h = info[2].As<Napi::Number>().Uint32Value();
    int format = (info.Length() > 3 && info[3].IsNumber()) ? info[3].As<Napi::Number>().Int32Value() : 0;
    std::string key = (info.Length() > 4 && info[4].IsString()) ? info[4].As<Napi::String>().Utf8Value() : std::string();
    // optional GPU downscale target (server/stage): consume(source, w, h, format, key, dstW, dstH)
    uint32_t dstW = (info.Length() > 5 && info[5].IsNumber()) ? info[5].As<Napi::Number>().Uint32Value() : 0;
    uint32_t dstH = (info.Length() > 6 && info[6].IsNumber()) ? info[6].As<Napi::Number>().Uint32Value() : 0;
    auto deferred = Napi::Promise::Deferred::New(env);
    uintptr_t handle = 0;
    Napi::Buffer<uint8_t> buf = info[0].As<Napi::Buffer<uint8_t>>();
    if (buf.Length() >= sizeof(uintptr_t)) std::memcpy(&handle, buf.Data(), sizeof(uintptr_t));
    (new ConsumeWorker(env, handle, w, h, format, std::move(key), dstW, dstH, deferred))->Queue();
    return deferred.Promise();
}

Napi::Value ReadbackFinishJs(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    std::string key = info[0].As<Napi::String>().Utf8Value();
    uint32_t w = info[1].As<Napi::Number>().Uint32Value();
    uint32_t h = info[2].As<Napi::Number>().Uint32Value();
    int format = (info.Length() > 3 && info[3].IsNumber()) ? info[3].As<Napi::Number>().Int32Value() : 0;
    // optional GPU-downscaled BGRA (server/stage): finish(key, w, h, format, dstW, dstH) -> { main, scaled }
    uint32_t dstW = (info.Length() > 4 && info[4].IsNumber()) ? info[4].As<Napi::Number>().Uint32Value() : 0;
    uint32_t dstH = (info.Length() > 5 && info[5].IsNumber()) ? info[5].As<Napi::Number>().Uint32Value() : 0;
    auto deferred = Napi::Promise::Deferred::New(env);
    size_t outSize = OutputSize(w, h, format);
    Napi::Buffer<uint8_t> result = AcquireOutputBuffer(env, key, outSize);

    bool hasScaled = dstW > 0 && dstH > 0;
    size_t scaledSize = hasScaled ? (size_t)dstW * dstH * 4 : 0;
    // separate reused pool for the scaled buffer so it never aliases the main (UYVY) pool for this output
    Napi::Buffer<uint8_t> scaledResult = hasScaled ? AcquireOutputBuffer(env, key + "#scaled", scaledSize) : Napi::Buffer<uint8_t>::New(env, 0);
    (new FinishWorker(env, std::move(key), result.Data(), outSize, result, hasScaled ? scaledResult.Data() : nullptr, scaledSize, hasScaled, scaledResult, deferred))->Queue();
    return deferred.Promise();
}
#endif

// Free a pool's reused buffers when its output is removed (else the per-output buffers leak for the process
// lifetime). No-op if the key is unknown.
Napi::Value ReleasePool(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    if (info.Length() > 0 && info[0].IsString()) {
        std::string key = info[0].As<Napi::String>().Utf8Value();
        // Only ever touch THIS env's pools (see AddonData) so we never destroy another thread/env's references.
        if (AddonData* data = env.GetInstanceData<AddonData>()) {
            data->pools.erase(key);
            data->pools.erase(key + "#scaled");
        }
#if defined(_WIN32)
        osrcap::ReadbackReleaseKey(key);
#endif
    }
    return env.Undefined();
}

}  // namespace

namespace osrcap {
void RegisterConvert(Napi::Env env, Napi::Object exports);
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    // Per-env pool storage (see AddonData). N-API runs Init once per env (main thread + each worker), and
    // finalizes this instance data on that env's teardown, on the env's own thread — the correct place to
    // destroy its Napi::References.
    env.SetInstanceData(new AddonData());
    exports.Set("readback", Napi::Function::New(env, Readback));
    exports.Set("releasePool", Napi::Function::New(env, ReleasePool));
#if defined(_WIN32)
    exports.Set("readbackConsume", Napi::Function::New(env, ReadbackConsumeJs));
    exports.Set("readbackFinish", Napi::Function::New(env, ReadbackFinishJs));
#endif
    osrcap::RegisterConvert(env, exports);
    return exports;
}

NODE_API_MODULE(osr_readback, Init)
