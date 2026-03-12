# Architecture

## Overview

IGR UI is split into five layers:

1. `igr_core`
   Immediate-mode primitives, frame lifecycle, widget modeling, diagnostics, resource budgets, and deterministic IDs.
2. `igr_backend_dx11`
   DirectX 11 renderer integration and lifecycle management.
3. `igr_backend_dx12`
   DirectX 12 renderer integration and lifecycle management.
4. `igr_react_bridge`
   Declarative document materialization from React-style trees into the immediate-mode frame builder.
5. `npm/` and `typescript/`
   TypeScript authoring surface, React renderer integration, and later native bridge bindings.

## Native frame model

The C++ core is immediate-mode at runtime. Each frame produces a `FrameDocument` containing widget nodes, diagnostics, and deterministic IDs. This keeps the model easy to test, easy to serialize, and compatible with both hand-authored C++ and declarative bridge inputs.

The frame model now also includes:

- low-level custom draw primitives for shared DX11 and DX12 rendering
- an overlay-input evaluation utility for manual-host and injected usage
- resource-keyed text and image metadata that can flow through both native and JS bridge paths
- shared shader descriptors and per-draw shader uniform payloads that flow through native C++, TSX transport, and both DirectX backends

## React-oriented strategy

React-style authoring is implemented as a separate layer. The bridge produces a declarative document that is materialized into immediate-mode commands each frame. This preserves immediate-mode runtime characteristics, explicit native frame control, and ergonomic composition for TypeScript and React developers.

In the current milestone, that authoring layer is delivered as a custom JSX runtime plus a typed document model, a versioned transport envelope, and a Hermes-backed runtime bridge. A fuller `react-reconciler` based renderer can still be added later without changing the native frame model.

## Backend strategy

Both DirectX backends implement a shared renderer contract:

- initialize
- invalidate back-buffer resources
- resize
- render
- present
- frame stats
- shutdown
- capability reporting

The interface is shared. Resource ownership and lifecycle rules remain backend-specific.

## Shader strategy

Shader support is shared at the API level and backend-specific at the realization layer.

- authoring surface: `ShaderResourceDesc`, `ShaderUniformData`, `shader_rect`, and `shader_image`
- compiler surface: HLSL directly, GLSL through `glslang` plus `SPIRV-Cross`
- backend realization: DX11 and DX12 each compile/cache backend-native shader programs and apply the same constant-buffer ABI

The current v1 shader contract is intentionally constrained:

- one optional sampled texture
- one shared constant block per draw
- alpha, opaque, or additive blending

That gives the library a stable public shader surface without pretending DX11 and DX12 have identical internal resource models.

## Diagnostics and budgets

The native runtime now exposes a shared telemetry surface and shared resource-budget controls across DX11 and DX12.

- `DiagnosticsConfig` controls process-memory sampling, GPU-memory sampling, scope timing capture, and telemetry cadence.
- `ResourceBudgetConfig` controls retained wide-string cache size plus retained scratch scene, vertex, batch, and shader-constant capacity.
- Both backends expose `telemetry()` so native hosts, samples, and higher-level bridge code can inspect live resource pressure without coupling to renderer internals.

The key architectural rule is that diagnostics stay observational and budgets stay explicit. The renderer should not hide expensive retention behind undocumented heuristics when the user wants a lower-memory configuration.

## Host models

The repository now treats backend hosting as a first-class architecture concern:

1. `owned_window`
   The library owns window and swap-chain creation.
2. `external_swap_chain`
   The caller passes native DirectX objects manually.
3. `injected_overlay`
   The library is attached to a host application's render loop and must avoid assuming ownership of `Present`, `ResizeBuffers`, input, or render state.

This distinction is now part of the backend API surface, not just sample behavior.

## Implementation phases

### Phase 1: Foundation

- repo layout
- C++ core
- declarative materializer
- backend contracts
- tests, example, docs, build scripts

### Phase 2: DX11 renderer

- device and swap chain bootstrap
- render target lifecycle
- frame submission
- DirectWrite text rendering
- theme-driven widget rendering
- resize correctness in both backend-managed and host-managed modes
- runtime validation hooks

Status: implemented. The DX11 backend now owns a live D3D11 plus Direct2D/DirectWrite path, supports the current widget set, restores host state for overlay-friendly rendering, and supports both app-owned and host-managed swap-chain attachment paths.

### Phase 3: DX12 renderer

- device, queue, swap chain, command-list, and fence model
- descriptor heap ownership
- frame resource management
- resize and synchronization correctness
- host-managed and injected-overlay API support from the start

Status: implemented. The DX12 backend now supports owned-window rendering, host-managed per-frame binding through `Dx12FrameBinding`, descriptor-backed textures, explicit barriers, upload-buffer drawing, and runtime validation through both sample and test binaries. The current default text path is atlas-backed, with the older interop path retained as an explicit mode for compatibility and validation.

### Phase 4: JS and package integration

- TypeScript packages
- React renderer
- stable bridge transport
- npm publishing and versioning

### Phase 5: polish

- examples
- contributor docs
- extension and plugin model
- CI hardening
- runtime debugging workflows

Status: in progress. The repo now includes the first Windows CI workflow plus explicit DX11 and extension docs.

The bridge layer now also includes a native transport parser and serializer for `igr.document.v1`, a runtime bridge that can materialize runtime-produced transport payloads into frame documents, and a packaged Hermes runtime host so the JS authoring layer can run in-process without rewriting the renderer.
