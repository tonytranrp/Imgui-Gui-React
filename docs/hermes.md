# Hermes Bridge

## Why Hermes here

The correct Hermes migration is not to replace the native renderer. `hermes-windows` is a JavaScript engine host for Windows and React-oriented runtimes, not a DirectX UI backend.

That means the long-lived assets stay where they already belong:

- C++ immediate-mode frame model
- DX11 renderer
- DX12 renderer
- host-managed overlay contracts

The JS side moves from "emit JSON from tests" toward "run React-authored scene logic inside a Hermes runtime and hand the transport payload to native."

## Current implementation

The repo now has three bridge layers:

- `igr::react::TransportEnvelope`
  Stable payload contract between JS and native.
- `igr::react::RuntimeDocumentBridge`
  Runtime-agnostic native host that asks a JS runtime for a transport payload and materializes it into a `FrameDocument`.
- `igr::react::HermesTransportRuntime`
  Hermes-specific runtime seam and bundle contract.

`HermesTransportRuntime` now hosts the packaged Microsoft.JavaScript.Hermes runtime directly on Windows. It resolves the configured JS or bytecode bundle, executes it in-process, resolves the configured global entrypoint, and returns `igr.document.v1` payloads to the native materializer without rewriting the renderers or reworking the transport contract again.

## Bundle flow

The React workspace app `npm/apps/react-native-test` now ships a dedicated Hermes entrypoint:

- `src/hermes-entry.ts`
  Publishes `globalThis.__igrRenderTransport(...)`.
- `scripts/hermes-bundle.ps1`
  Builds the TS workspaces and emits `artifacts/hermes/react-native-test.bundle.js`.
- `npm run hermes:bundle`
  Uses `esbuild` to create a single JS bundle.
- `hermes.exe` / `hermesc`
  The bundle step now checks `PATH`, `$env:HERMESC`, and the repo-local `.cache/hermes/<version>/tools/native/release/x86/hermes.exe` package tool so bytecode emission works from the same Hermes package the native build uses.

## Runtime host flow

The current host path is:

1. Create a `HermesTransportRuntime`.
2. Load either the JS bundle or the `.hbc` bytecode artifact from `artifacts/hermes/`.
3. Create the Hermes runtime and Node-API environment from the packaged Microsoft.JavaScript.Hermes binaries.
4. Execute the bundle and resolve the configured global entrypoint.
5. Call that entrypoint for each frame with `sequence`, `frameIndex`, `deltaSeconds`, and `viewport`.
6. Receive `igr.document.v1`.
7. Materialize it through `RuntimeDocumentBridge`.
8. Render it through DX11 or DX12 using the existing native backends.

The native test suite now exercises this path directly in `igr_react_runtime_bridge_tests`, `igr_dx11_external_host_tests`, and `igr_dx12_contract_tests`.

## Current limits

The remaining Hermes-side work is no longer runtime creation. It is host hardening:

- applying `session.host` and resource deltas automatically at the native host layer instead of doing that policy manually in tests and samples
- adding a bidirectional event/input channel from native interaction analysis back into JS
- broadening the TSX app layer beyond the current physics-scene fixture app

## Why not rewrite the renderers

The DX11 and DX12 backends already own the hard native problems:

- swap-chain and resize behavior
- host-managed attach and detach
- overlay-safe present behavior
- resource registration
- clip/scissor behavior
- text and image composition

Moving the authoring/runtime layer to Hermes should reduce JS runtime mismatch and make React-style integration cleaner, but it should not invalidate the native rendering architecture.
