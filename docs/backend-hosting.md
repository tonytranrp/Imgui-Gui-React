# Backend Hosting Models

## Why this exists

There are two very different ways to use this library:

1. An app-owned renderer, window, and swap chain.
2. A host-managed renderer, where the library is attached to somebody else's DirectX runtime, including injected DLL overlays.

Those paths cannot share the same assumptions.

## Shared host modes

The backend contract now models three host modes through `BackendHostOptions`:

- `owned_window`
  The library owns window and swap-chain creation.
- `external_swap_chain`
  The caller owns the swap chain and passes native objects in explicitly.
- `injected_overlay`
  The caller is typically inside a hooked `Present` or `ResizeBuffers` path and does not own the game window or presentation loop.

## DX11 current criteria

DX11 now supports the first round of host-managed behavior:

- swap-chain attachment without blind renderer creation
- device and immediate-context derivation from an external swap chain
- optional window derivation from the swap chain
- host-managed `present()` mode
- host-managed no-clear overlay composition through `clear_target = false`
- explicit `invalidate_back_buffer_resources()` before host `ResizeBuffers`
- render-state restoration so overlay rendering does not leave the host D3D11 context dirty
- clip-rect composition that resolves into DX11 scissor rectangles and clipped text draws
- caller-registered user textures for document-driven `image` widgets
- caller-registered font and image resources
- explicit `rebind_host()` and `detach_host()` lifecycle hooks
- backend frame stats for overlay instrumentation

This is the minimum architecture needed for a game-overlay-friendly DX11 path.

## DX11 remaining todos

The highest-value remaining DX11 work is now:

1. Higher-level input event translation on top of the existing capture helper
2. Richer atlas packing and image metadata beyond the current registry
3. Broader custom draw and extension hooks
4. Deeper overlay performance profiling and batching review

## DX12 required criteria

DX12 should not imitate the DX11 sample path mechanically. For host-managed and injected usage, the API must require explicit caller-owned objects:

- `ID3D12Device*`
- `ID3D12CommandQueue*`
- `IDXGISwapChain3*`
- `ID3D12GraphicsCommandList*` in host-managed and injected modes
- current back-buffer index
- render-target descriptor ownership rules
- descriptor heap ownership rules
- frame-resource and fence model
- descriptor-backed user-texture registration rules for React and immediate-mode `image` widgets

DX12 must also follow the same host lifecycle ideas:

- host-managed `present()`
- `invalidate_back_buffer_resources()` before host resize
- no blind queue, allocator, or swap-chain creation in injected mode
- explicit state boundaries around command-list recording
- explicit per-frame binding validation in code before any draw path exists
- explicit tracking of whether the host already set descriptor heaps or render targets for the frame

The current DX12 code path enforces that through `Dx12FrameBinding` plus `bind_frame()`, and now also records a real render pass in both owned-window and host-managed modes.

DX12 now also exposes:

- caller-registered font and image resources
- explicit `rebind_host()` and `detach_host()` hooks
- backend frame stats for overlay instrumentation

## Design rule

If the caller does not own the window or the renderer, the backend must assume manual passing and minimal interference. That rule now applies to both DX11 and DX12 API design.
