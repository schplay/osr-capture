# osr-capture

Read back an Electron **offscreen (OSR) shared GPU texture** into a CPU buffer **off the main thread**.

## Why

Electron's offscreen rendering normally hands you a CPU `NativeImage` in the `paint` event, and turning it
into pixel data (`toBitmap()`) is a synchronous **main-thread** operation. For an app that captures several
high-resolution output windows (e.g. to send over NDI / WebRTC / RTMP / SDI), that readback on the main
thread becomes the bottleneck and can't scale to multiple 4K surfaces.

When offscreen rendering is enabled with `useSharedTexture: true`, Electron instead hands you a **GPU
texture handle** (no CPU copy is made). This addon opens that shared texture, copies it into a staging
texture and maps it into a Node `Buffer` — and it does the GPU→CPU copy inside an N-API async worker (a
libuv background thread), so the readback never blocks the JavaScript main thread.

Built for FreeShow (https://github.com/ChurchApps/FreeShow), but generic.

## Platform support

| Platform | Backend | Status |
| --- | --- | --- |
| Windows | Direct3D 11 (`OpenSharedResource` + staging texture) | Implemented & validated |
| macOS | IOSurface (`IOSurfaceLock` + row copy) | Implemented — needs testing on macOS |
| Linux | GPU readback (EGL dmabuf import + GLES3 convert), CPU dmabuf `mmap` fallback | Implemented — the GPU path handles tiled/compressed modifiers and converts on the GPU (reads back the converted output instead of raw BGRA, far faster); when EGL/dmabuf import is unavailable it falls back automatically to a per-frame CPU `mmap` (LINEAR modifier only). The two-phase API is available on Linux when the GPU path initialized (feature-detect via presence of `readbackConsume`, same as Windows) |

Where the addon fails to load or a readback isn't supported for the given frame, callers should fall back
to CPU-mode offscreen capture.

## API

```ts
import osr from "osr-capture"

// source (Windows/macOS): the 8-byte Buffer from paint's texture.textureInfo.sharedTextureHandle
// source (Linux):         { planes: texture.textureInfo.planes, modifier: texture.textureInfo.modifier }
// width/height:           texture.textureInfo.codedSize
// resolves to a tightly-packed BGRA buffer (width * 4 * height bytes)
osr.readback(source: Buffer | { planes, modifier }, width: number, height: number): Promise<Buffer>
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
- Always call `texture.release()` once you've copied (or decided to skip) the frame.
- Readbacks are serialized internally (single D3D11 device/context); throttle to your target frame rate and
  keep at most one readback per surface in flight.

### Diagnostics (Linux)

Set `FS_LINUX_READBACK=cpu` to force the CPU `mmap` fallback (bypassing the GPU path), and
`FS_LINUX_READBACK_FLIP=1` to flip rows if your driver delivers frames upside-down.

## Build

Native addon (node-gyp + `node-addon-api`). Requires a C++17 toolchain and the Windows SDK (`d3d11.h`,
`dxgi1_2.h`). On Linux, building needs `libegl1-mesa-dev` and `libgles2-mesa-dev`; at runtime the addon
links against `libegl1` and `libgles2`.

Against Node:

```
npm install
```

Against a specific Electron ABI (what consumers actually need):

```
npx node-gyp rebuild --target=<electron-version> --dist-url=https://electronjs.org/headers --arch=x64
# or, when installed as a dependency: npx electron-rebuild -f -w osr-capture
```
