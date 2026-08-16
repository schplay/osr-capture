// Stage-0 harness for the global copy pool (Windows). Pure CPU — no GPU/Electron needed.
// Validates: byte-exact copies under concurrency, NO deadlock (the process hangs if the pool deadlocks),
// and that the §11 CV#1 knee-seeking calibrator SETTLES (no ringing). NOTE: this runs on cacheable RAM, so
// the converged active_n here is meaningless for WC/BAR staging sizing — only correctness and convergence
// BEHAVIOR are being tested. The real operating point is observable in-app ([COPY-POOL] under FS_CAP_STATS).
// Run: node test/copy-pool-selftest.js
const path = require("path")
const addon = require(path.join(__dirname, "..", "build", "Release", "osr_readback.node"))

if (typeof addon._copyPoolSelfTest !== "function") {
    console.error("addon has no _copyPoolSelfTest — rebuild the addon (Windows only)")
    process.exit(1)
}

const MiB = 1 << 20
const cases = [
    { bytes: 8 * MiB, concurrency: 1, iterations: 200 },
    { bytes: 8 * MiB, concurrency: 3, iterations: 200 },
    { bytes: 8 * MiB, concurrency: 6, iterations: 200 },
    { bytes: 16 * MiB, concurrency: 8, iterations: 100 },
]

let allOk = true
for (const c of cases) {
    const r = addon._copyPoolSelfTest(c.bytes, c.concurrency, c.iterations)
    const totalGiB = (c.bytes * c.concurrency * c.iterations) / (1 << 30)
    const gbps = totalGiB / (r.ms / 1000)
    console.log(
        `bytes=${c.bytes / MiB}MiB conc=${c.concurrency} iters=${c.iterations} -> ok=${r.ok} mismatches=${r.mismatches} active_n=${r.workers} ms=${r.ms.toFixed(1)} (${gbps.toFixed(2)} GiB/s aggregate)`
    )
    if (!r.ok) allOk = false
}

// Calibrator stability: hammer the pool with a STEADY load from a worker thread (bursty per-round loads would
// legitimately move the knee — that's tracking, not ringing) and sample the calibrated active_n from here.
// The knee-seeker must stop drifting once warmed up — a ±1 probe dither at the knee is by construction, so
// the settled trajectory may span 2 (knee at n, dither to n±1); more spread = base ringing = FAIL.
async function calibratorStability() {
    if (typeof addon._copyPoolActiveWorkers !== "function") {
        console.error("addon has no _copyPoolActiveWorkers — rebuild the addon")
        return false
    }
    const { Worker } = require("worker_threads")
    const hammer = new Worker(
        `const { workerData, parentPort } = require("worker_threads")
         const a = require(workerData.addonPath)
         const r = a._copyPoolSelfTest(workerData.bytes, workerData.concurrency, workerData.iterations)
         parentPort.postMessage(r.ok)`,
        { eval: true, workerData: { addonPath: path.join(__dirname, "..", "build", "Release", "osr_readback.node"), bytes: 16 * MiB, concurrency: 6, iterations: 400 } }
    )
    const traj = []
    const done = new Promise((resolve) => hammer.once("message", resolve))
    let running = true
    void done.then(() => (running = false))
    while (running) {
        await new Promise((r) => setTimeout(r, 500))
        if (running) traj.push(addon._copyPoolActiveWorkers())
    }
    const hammerOk = await done
    await hammer.terminate()
    if (!hammerOk) return false
    const tail = traj.slice(-6)
    const spread = Math.max(...tail) - Math.min(...tail)
    const stable = spread <= 2
    console.log(`calibrator active_n trajectory=[${traj.join(", ")}] tailSpread=${spread} -> ${stable ? "settled" : "RINGING"}`)
    return stable
}

calibratorStability().then((stable) => {
    if (!stable) allOk = false
    console.log(allOk ? "PASS" : "FAIL")
    process.exit(allOk ? 0 : 1)
})
