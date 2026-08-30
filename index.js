// osr-capture: read back an Electron OSR shared GPU texture to a CPU buffer off the main thread.
// See index.d.ts for the full typed API. Summary:
//   readback(source, width, height, format = 0, poolKey?) => Promise<Buffer>
//     format 0 = BGRA (tightly-packed), 1 = UYVY (opaque), 2 = UYVA (colour + full-res alpha), 3 = RGBA.
//     Windows converts on the GPU (compute shader); Linux converts on the GPU too when the EGL/GLES3 path
//     initialized (see LINUX_GPU_READBACK.md), else CPU; mac converts on the CPU.
//   readbackConsume(source, w, h, format, poolKey, dstW?, dstH?) / readbackFinish(poolKey, w, h, format, dstW?, dstH?)
//     Two-phase readback (Windows always; Linux when the GPU path is active — absent otherwise): release the
//     shared texture right after the GPU consume, then do the slow copy.
//     dstW/dstH also GPU-downscale a small BGRA in the same pass -> readbackFinish resolves { main, scaled }.
//   releasePool(poolKey)                                  // free a poolKey's reused output buffers
//   convertBgraToUyvy / convertBgraToUyva(bgra, w, h)     // CPU fallback converters (sync)
//   downscaleBgra(bgra, srcW, srcH, dstW, dstH)           // CPU box-filter downscale (sync)
module.exports = require("./build/Release/osr_readback.node")
