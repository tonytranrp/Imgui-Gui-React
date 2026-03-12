# Memory and Diagnostics

IGR UI now exposes enough telemetry to separate library-owned memory from unavoidable process, runtime, and graphics-driver overhead.

## Current findings

Measured in the current release representative workload:

- DX11 sample: about `61.6 MB` private, `53.5 MB` working set, about `19.2 MB` committed GPU memory
- DX12 sample: about `90.4 MB` private, `86.0 MB` working set, about `51.3 MB` committed GPU memory
- React menu harness: about `75.5 MB` private, `79.5 MB` working set during short warmup, then about `76.7 MB` private and `50.4 MB` working set after a 30-second steady-state warmup

The important architectural result is that the backend-owned allocations are much smaller than the full process totals. In the DX12 sample, live backend telemetry at a render breakpoint showed:

- tracked renderer resources around `1.1 MB`
- DX12 text atlas CPU bitmap around `1.0 MB`
- scene plus scratch buffers well below `100 KB`

That means most remaining DX12 memory is process, device, swap-chain, runtime, and driver overhead, not a large leak in the immediate-mode scene data.

## What changed

- lowered default retained-capacity budgets for scene scratch, vertices, batches, cached wide strings, and shader constants
- made scratch-storage trimming capacity-aware instead of keeping the full configured budget alive after small frames
- made DX12 release text-atlas staging memory after stable frames instead of holding the software bitmap and upload buffer indefinitely
- added a bytecode-only option to the Hermes runtime surface and switched the live menu harness to avoid source-bundle fallback when bytecode is requested
- added an opt-in Hermes working-set trim path that runs only after explicit GC, so low-memory hosts can trade some page-fault cost for a lower steady-state working set
- improved DX12 resource telemetry so texture resources report estimated bytes instead of only their width field
- fixed the release test/benchmark scripts so they use shorter build directories and the benchmark harness measures the main release binaries by default

## Benchmarking

Run the native benchmark harness with:

```powershell
./scripts/benchmark-memory.ps1
```

Useful options:

```powershell
./scripts/benchmark-memory.ps1 -Configuration Release
./scripts/benchmark-memory.ps1 -WarmupSeconds 5
./scripts/benchmark-memory.ps1 -Dx12Args "--frames 600000 --text-interop"
./scripts/benchmark-memory.ps1 -Configuration Release -BuildDirName b-release
./scripts/benchmark-memory.ps1 -MenuArgs "--source-runtime"
```

The script reports:

- private bytes
- working set
- per-process GPU dedicated usage
- per-process GPU shared usage
- per-process GPU committed memory

## Practical guidance

- Prefer `Release` for meaningful memory numbers. Debug builds keep extra CRT, symbols, and validation overhead alive.
- Prefer the DX12 atlas text path unless you need interop-specific validation. It reduces working-set pressure and avoids the heavier D3D11On12 text stack.
- Prefer Hermes source mode when you need the most conservative runtime-validation path. Use bytecode mode only after that exact host/runtime combination has passed live validation.
- The current `< 50 MB` process-private target is not realistic for every DX12 or Hermes-backed scenario. The stable floor is now dominated by Windows, DXGI, D3D runtime, swap-chain, driver, and Hermes baseline costs rather than large library-owned caches.
- Use backend telemetry to judge library regressions, not only process totals. Process totals include Windows, DXGI, D3D runtime, and JS runtime baseline costs that the UI library does not fully control.
