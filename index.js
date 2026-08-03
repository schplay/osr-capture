// osr-capture: read back an Electron OSR shared GPU texture to a CPU BGRA buffer off the main thread.
// readback(handle: Buffer(8), width: number, height: number) => Promise<Buffer>  // tightly-packed BGRA
module.exports = require("./build/Release/osr_readback.node")
