# IGR UI

IGR UI is a Windows-native immediate-mode UI library with a React-oriented declarative bridge. It is designed for modern C++20, DirectX 11, DirectX 12, TypeScript-first tooling, and public open-source developer ergonomics.

The current bridge direction is Hermes-first: React-authored scene logic should run in a JS runtime such as `hermes-windows`, then emit the stable native `igr.document.v1` transport payload that the C++ runtime materializes and renders.

## Current state

The repository now contains the production foundation:

- a buildable C++ core with deterministic frame and widget modeling
- a live DirectX 11 backend with real device, swap chain, resize, render, present, theme, DirectWrite text, user-texture image paths, and custom draw primitives
- manual host-binding support for DX11, including host-managed present and resize flows, clip-rect scissoring, and overlay-safe no-clear composition
- a live DirectX 12 backend with owned-window rendering, the lower-memory software text atlas enabled by default, an optional D3D11On12 interop fallback, host-managed frame binding, descriptor-backed textures, custom draw primitives, and runtime validation for injected and external-host usage
- a shared shader system for DX11 and DX12 with HLSL and GLSL support, `shader_rect` and `shader_image` widgets, runtime compilation, and React/transport authoring support
- a shared overlay-input evaluation utility for injected/manual-host integrations
- a declarative materializer that maps React-style element trees onto the immediate-mode core
- a packaged Hermes runtime host that executes React-authored TSX scene bundles and materializes their transport payload into native frames
- native tests, a foundation example, a live Win32 DX11 sample, CI workflow, build scripts, and architecture docs

Both native backends now share the same document-driven widget set, resource registries, overlay input evaluation, host rebind and detach flows, backend frame stats, and a transport-backed JS bridge envelope. The Hermes bridge now runs in-process through the packaged Microsoft.JavaScript.Hermes runtime, with the TSX workspace app generating the same payloads that the external-host DX11 and DX12 tests consume. The remaining major work is bidirectional JS/native events, richer host-managed atlas integration when the host owns descriptor heaps, deeper performance instrumentation, and broader extension/plugin coverage.

## Repository layout

- `docs/`: architecture, vision, build notes, engineering log
- `include/`: public C++ headers
- `src/`: native UI core and bridge implementation
- `backends/dx11/`: DirectX 11 backend implementation
- `backends/dx12/`: DirectX 12 backend implementation
- `examples/`: runnable native samples
- `tests/`: native verification
- `scripts/`: Windows build and test entry points
- `npm/`: npm workspace and published package layout
- `typescript/`: shared TypeScript configuration and authoring assets
- `artifacts/hermes/`: generated Hermes-friendly JS and bytecode bundles

## Native build

```powershell
./scripts/build.ps1
./scripts/test.ps1
./scripts/benchmark-memory.ps1
./b/igr_win32_dx11_sample.exe --frames 120
```

## TypeScript and npm build

```powershell
./scripts/npm-install.ps1
./scripts/npm-test.ps1
./scripts/hermes-bundle.ps1
```

## Documentation

- [Vision](docs/vision.md)
- [Architecture](docs/architecture.md)
- [Backend Hosting](docs/backend-hosting.md)
- [DirectX 11 Backend](docs/dx11.md)
- [DirectX 12 Backend](docs/dx12.md)
- [Build](docs/build.md)
- [Extensions](docs/extensions.md)
- [Shaders](docs/shaders.md)
- [TypeScript Bridge](docs/typescript-bridge.md)
- [Hermes Bridge](docs/hermes.md)
- [Memory and Diagnostics](docs/memory.md)
- [Engineering Log](docs/engineering-log.md)
- [Contributing](CONTRIBUTING.md)
