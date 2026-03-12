# Engineering Log

## 2026-03-11

- Repository foundation pass:
  - audited the repository and confirmed it started as an empty shell
  - chose the core architecture: native C++20 immediate-mode runtime plus a React-oriented declarative bridge
  - established the repo layout, build scripts, docs, and native verification path
- Core and bridge foundation:
  - implemented deterministic frame and widget modeling
  - implemented the backend contracts for DX11 and DX12
  - implemented declarative materialization for React-style trees
  - added native tests and the foundation sample
- TypeScript/npm foundation:
  - added `@igr/core` typed document model utilities
  - added `@igr/react` JSX runtime utilities
  - added workspace build/test flow and transport helpers
- DX11 production milestone:
  - implemented real device, swap chain, render target, resize, render, and present flow
  - added Direct2D/DirectWrite text, theme-driven widget rendering, user-texture image paths, clip/scissor support, and host-safe no-clear composition
  - added host-managed present/resize lifecycle support and render-state restoration for overlay-style use
  - added hidden-window backend tests and a live Win32 DX11 sample
- DX12 production milestone:
  - implemented owned-window rendering with a real draw path
  - implemented host-managed `Dx12FrameBinding`, descriptor-backed textures, and contract validation for external/injected hosts
  - added DirectWrite text for owned-window DX12 through D3D11On12 interop
  - fixed host-managed startup and rebind crashes and hardened resize/device-object behavior
- Shared backend parity:
  - added shared custom draw primitives: `fill_rect`, `stroke_rect`, `draw_line`
  - aligned DX11 and DX12 widget chrome, image handling, resource registries, and frame stats
  - added shared overlay-input evaluation utilities
  - added host `rebind_host()` and `detach_host()` flows to both backends
- Runtime quality and validation:
  - moved the default native build output to `./b/`
  - switched live samples to the Windows GUI subsystem
  - registered fonts and image resources in both samples so declarative and native paths exercise the same contracts
  - fixed sample texture byte order and image layout sizing
  - revalidated DX11 and DX12 samples through live runs and debugger inspection
- Hermes-first bridge foundation:
  - added a TSX workspace app under `npm/apps/react-native-test` that emits an external-host physics scene
  - exported that scene into `tests/fixtures/react-native-test.physics.json` so native DX11 and DX12 host-managed tests consume a TSX-authored payload
  - added `RuntimeDocumentBridge` and `StaticTransportRuntime` so JS-produced transport payloads can be materialized into native frames through a runtime seam
  - fixed a JSX-runtime bug where intrinsic children leaked back into serialized props and broke native transport parsing
- Hermes runtime host completion:
  - wired `HermesTransportRuntime` to the packaged `Microsoft.JavaScript.Hermes` runtime using the Windows Node-API hosting surface
  - taught CMake to provision the Hermes package in `.cache/hermes/<version>/` and link the native bridge against the real runtime
  - updated the TSX Hermes entrypoint and physics scene so frame requests can carry `frameIndex`, `viewport`, and `deltaSeconds`
  - upgraded the native runtime, DX11 external-host, and DX12 contract tests to execute the bundled TSX scene through Hermes instead of treating Hermes as unsupported
  - updated the Hermes bundle tooling so bytecode compilation can reuse the repo-local packaged Hermes toolchain instead of relying only on `PATH`
- React transport shader schema pass:
  - added `shaders` to the transport envelope alongside fonts and images
  - added typed transport shader resources for HLSL/GLSL stage payloads, sampling flags, and blend modes
  - added `shader_rect` authoring support in TSX and native document materialization with compatibility shims for older `FrameBuilder` signatures
  - updated the external-host physics TSX scene and native bridge tests to cover shader transport payloads
- Shared shader runtime completion:
  - restored a coherent shared resource type surface and frame timing metadata for shader-driven rendering
  - implemented a shared DX11/DX12 shader compiler path for HLSL and GLSL sources and removed the obsolete fragment-only shader toolchain
  - added DX11 and DX12 runtime support for both `shader_rect` and `shader_image`
  - registered and rendered demo shader widgets in both live Win32 samples
  - added native shader coverage in core, DX11, DX12, and compiler-specific tests
  - documented the v1 shader ABI, authoring path, and current limits
- Memory and edge-case hardening pass:
  - replaced hot-path backend string-key copies with transparent `std::string_view` lookup for DX11 and DX12 scene generation, validation, and `has_*` queries
  - moved DX12 off per-frame by-value `Scene` and draw-batch construction onto reusable scratch buffers matching the DX11 model
  - reduced retained transient memory by explicitly releasing scratch buffers, texture registries, shader caches, and bytecode buffers on shutdown and host detach
  - stopped DX12 text-target rebuilds from recreating the entire interop stack on every resize when the existing interop device can be reused
  - tightened resource validation so invalid font sizes, invalid image descriptors, foreign-device DX11 textures, wrong DX12 heap types, missing CPU descriptors, and non-shader-visible heaps fail early
  - extended backend regression coverage for the stricter DX11 and DX12 resource-registration rules
- Diagnostics and DX12 text-atlas pass:
  - added shared backend telemetry snapshots for process memory, GPU memory, resource estimates, and top runtime scopes
  - moved React/Hermes transport resource application into the runtime bridge so native render paths no longer need backend-specific test glue
  - added a software text-atlas path for DX12 while keeping conservative D3D11On12 plus DirectWrite interop as the backend default text renderer
  - updated the DX12 sample to opt into atlas mode by default and added a `--text-interop` switch for live fallback validation
  - cut the DX12 sample private bytes from roughly `192 MB` to roughly `94 MB` in the current representative sample workload while keeping text visible
  - surfaced runtime telemetry in the DX11 and DX12 samples so memory, scratch usage, text-path state, and hot scopes are visible in-library
- DX12 atlas stability follow-up:
  - fixed the atlas upload-buffer lifecycle so `CopyTextureRegion` no longer records a zeroed footprint into owned-window command lists
  - added D3D12 info-queue detail to DX12 command-list reset and close failures so invalid recording state is diagnosable from test output
  - aligned the DX12 sample, docs, and backend tests around the real mode contract: interop remains the backend default and atlas is an explicit low-memory mode
  - extended the DX12 backend regression binary to cover both explicit atlas mode and explicit interop mode

## Current milestone

DX11 and DX12 now expose the same document-level widget and custom-draw surface, the same overlay-input evaluation model, shared resource registration, host detach/rebind hooks, backend frame stats, a cross-backend shader ABI, and a transport-backed JS/native document envelope. Owned-window DX12 renders real DirectWrite text, and the Hermes bridge is now a real in-process runtime host instead of a stub.

## Next milestone

- host-side application of `session.host` and resource deltas
- richer atlas/resource packing, especially for DX12 host-managed overlays
- bidirectional JS/native events and interaction transport
- broader extension/plugin coverage and higher-level React app surfaces
