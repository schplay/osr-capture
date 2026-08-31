# osr-capture

Read back an Electron **offscreen (OSR) shared GPU texture** into a CPU buffer **off the main thread**.

## Why

Electron's offscreen rendering normally hands you a CPU `NativeImage` in the `paint` event, and turning it
into pixel data (`toBitmap()`) is a synchronous **main-thread** operation. For an app that captures several
high-resolution output windows (e.g. to send over NDI / WebRTC / RTMP / SDI), that readback on the main
thread becomes the bottleneck and can't scale to multiple 4K surfaces.

When offscreen rendering is enabled with `useSharedTexture: true`, Electron instead hands you a **GPU
texture handle** (no CPU copy is made). This addon opens that shared texture, converts and/or downscales it
on the GPU where supported, and maps the result into a Node `Buffer`. The GPU-to-CPU copy runs inside an
N-API async worker (a libuv background thread), so the readback never blocks the JavaScript main thread.

Built for FreeShow (https://github.com/ChurchApps/FreeShow), but generic.

## Platform support

| Platform | Backend | Status |
| --- | --- | --- |
| Windows | Direct3D 11 / D3D11On12 | Implemented and validated |
| macOS | Metal (IOSurface import) | Implemented and validated |
| Linux | EGL dmabuf import + GLES3 | Implemented and validated |

Every platform also has an automatic CPU fallback (see per-platform notes). Where the addon fails to load
or a readback isn't supported for the given frame, callers should fall back to CPU-mode offscreen capture.

### Platform notes

**Windows.** Opens the shared handle on a dedicated D3D11 device, converts/downscales on the GPU, and reads
back through a cached D3D12 readback heap with a copy-queue fence when D3D11On12 is available (plain D3D11
staging otherwise). The two-phase API is always exported.

**macOS.** Wraps the IOSurface zero-copy as an `MTLTexture` and converts/downscales with Metal compute, so
only the reduced result reaches the CPU; on Apple Silicon the output buffer is `StorageModeShared`, making
the copy-out a cached memcpy with no bus transfer. Falls back to `IOSurfaceLock` + row copy when there is
no Metal device, the surface is not BGRA, or the width is not a multiple of 4. The two-phase API is
exported when a Metal device exists.

**Linux.** Imports the dmabuf planes via EGL (handles tiled/compressed modifiers) and converts/downscales
with GLES3, reading back the converted output instead of raw BGRA. Falls back to a per-frame CPU `mmap`
(LINEAR modifier only) when EGL/dmabuf import is unavailable. The two-phase API is exported when the GPU
path initialized.

## API

Pixel `format` codes, accepted wherever a `format` argument appears:

| Code | Output | Size |
| --- | --- | --- |
| 0 | BGRA (default) | `width * 4 * height` |
| 1 | UYVY | `width * 2 * height` |
| 2 | UYVA (UYVY + alpha plane) | `width * 2 * height + width * height` |
| 3 | RGBA | `width * 4 * height` |

`source` is platform-dependent: on Windows/macOS the 8-byte `Buffer` from the paint event's
`texture.textureInfo.sharedTextureHandle`; on Linux `{ planes: textureInfo.planes, modifier:
textureInfo.modifier }`. `width`/`height` come from `textureInfo.codedSize`.

`poolKey` selects a per-output reused result-buffer pool (avoids a large per-frame allocation); pass a
stable id per capture surface, and call `releasePool` when that surface goes away.

### Single-phase

```ts
// resolves to the converted frame in the requested format
readback(source, width: number, height: number, format?: number, poolKey?: string): Promise<Buffer>
```

### Two-phase (GPU paths only; feature-detect with `typeof readbackConsume === "function"`)

Splits the readback so the Electron texture can be released the moment the GPU has consumed it, well before
the slower copy-out finishes. This keeps the compositor's frame pool full and lets the next frame's consume
overlap the previous frame's finish.

```ts
// enqueue the GPU consume (open/import + convert + optional BGRA downscale to dstW x dstH);
// resolves once the shared texture is safe to release
readbackConsume(source, width, height, format?, poolKey?, dstW?, dstH?): Promise<void>

// copy the converted result out; resolves to the frame Buffer, or { main, scaled } when a
// downscale target was given to readbackConsume
readbackFinish(poolKey, width, height, format?, dstW?, dstH?): Promise<Buffer | { main, scaled }>

// both phases in one call: onRelease fires the moment the GPU has consumed the shared texture
// (release the Electron texture there), the promise resolves with the copied-out result
readbackOnce(source, width, height, format, poolKey, dstW, dstH, onRelease): Promise<Buffer | { main, scaled }>
```

### Utilities

```ts
releasePool(poolKey?: string): void         // free a surface's pooled buffers (all pools when omitted)
convertBgraToUyvy(bgra, width, height): Buffer  // CPU convert helpers for buffers you already have
convertBgraToUyva(bgra, width, height): Buffer
downscaleBgra(bgra, srcW, srcH, dstW, dstH): Buffer
_readbackBackend(): string                  // diagnostic: which backend is active (e.g. "d3d11on12", "cpu")
```

## Usage (Electron main process)

```js
const osr = require("osr-capture")

const win = new BrowserWindow({
    show: false,
    webPreferences: { offscreen: { useSharedTexture: true } }
})

win.webContents.on("paint", async (event) => {
    const tex = event.texture
    const info = tex?.textureInfo
    if (!info) return
    try {
        const { width, height } = info.codedSize
        const source = process.platform === "linux" ? { planes: info.planes, modifier: info.modifier } : info.sharedTextureHandle
        const bgra = await osr.readback(source, width, height)
        // ... hand `bgra` to your encoder / sender ...
    } finally {
        // REQUIRED: release every texture or the compositor frame pool drains and paints stop
        tex.release()
    }
})
```

Notes:
- Always call `texture.release()` once you've copied (or decided to skip) the frame. With the two-phase
  API, release as soon as `readbackConsume` resolves (or in `readbackOnce`'s `onRelease`).
- Keep at most one readback per surface in flight, and throttle to your target frame rate.
- The addon can be used from a `worker_thread`; pools are per-environment.

### Diagnostics (Linux)

Set `FS_LINUX_READBACK=cpu` to force the CPU `mmap` fallback (bypassing the GPU path), and
`FS_LINUX_READBACK_FLIP=1` to flip rows if your driver delivers frames upside-down.

## Build

Native addon (node-gyp + `node-addon-api`), C++17. Per-platform requirements:

- **Windows**: Visual Studio Build Tools with the Windows SDK (`d3d11.h`, `d3d12.h`, `dxgi1_2.h`).
- **macOS**: Xcode Command Line Tools (Metal/IOSurface/Foundation are system frameworks).
- **Linux**: no development packages. The EGL/GLES headers are vendored (`third_party/khronos`) and the
  addon links the runtime libraries directly (`libEGL.so.1` / `libGLESv2.so.2`, shipped by
  `libegl1`/`libgles2`, present on any GPU-composited desktop).

Against Node:

```
npm install
```

Against a specific Electron ABI (what consumers actually need):

```
npx node-gyp rebuild --target=<electron-version> --dist-url=https://electronjs.org/headers --arch=x64
# or, when installed as a dependency: npx electron-rebuild -f -w osr-capture
```
