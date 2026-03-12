# Contributing

## Scope

This repository is currently focused on Windows-native UI rendering with:

- modern C++
- DirectX 11
- DirectX 12
- React-oriented declarative authoring
- TypeScript and npm packaging

Please keep changes aligned with that scope. Do not add unrelated rendering backends or web-only abstractions without an explicit architecture decision.

## Local validation

Before sending a change upstream, run:

```powershell
./scripts/test.ps1
./scripts/npm-test.ps1
./build/nmake-debug/igr_win32_dx11_sample.exe --frames 120
```

If you touch the DX11 backend, also validate the live sample under the VS Code `C++: Win32 DX11 Sample` launch configuration.

## Engineering rules

- keep the immediate-mode core backend-agnostic where possible
- keep backend lifecycle code explicit
- prefer RAII and deterministic ownership
- preserve clean public APIs over short-term convenience
- add or update tests for behavioral changes
- update docs as the architecture evolves

## DX11 changes

DX11 work must preserve:

- swap-chain resize correctness
- render-target recreation
- Direct2D and DirectWrite text rendering
- hidden-window backend test coverage
- live sample validation

If a DX11 change breaks runtime validation, fix that before moving on to adjacent refactors.

## DX12 changes

DX12 work should follow the existing backend contract and keep synchronization, descriptor ownership, and frame-resource lifetime explicit. Do not weaken DX12 design constraints just to mirror DX11 structure mechanically.
