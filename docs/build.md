# Build

## Requirements

- Windows
- Visual Studio with C++ build tools
- Windows SDK
- PowerShell
- Node.js and npm for the TypeScript workspace

## Native commands

```powershell
./scripts/build.ps1
./scripts/test.ps1
./b/igr_win32_dx11_sample.exe --frames 120
```

The scripts resolve the latest installed Visual Studio instance with `vswhere`, load the developer environment, configure CMake with NMake, and build or test the native targets.

The default debug output now lives in `./b/` instead of `./build/nmake-debug/`. That avoids the NMake dependency-path issue that can appear in deeper workspace paths on Windows.

When Hermes is enabled, CMake downloads or reuses the packaged `Microsoft.JavaScript.Hermes` runtime under `.cache/hermes/<version>/` and links the native bridge against it automatically.

## TypeScript and npm commands

```powershell
./scripts/npm-install.ps1
./scripts/npm-test.ps1
./scripts/hermes-bundle.ps1
```

The Hermes bundle step emits `artifacts/hermes/react-native-test.bundle.js` and, when the Hermes compiler is available from `PATH`, `$env:HERMESC`, or the repo-local package cache, also emits `artifacts/hermes/react-native-test.bundle.hbc`.

## Notes

- The first milestone focuses on the native foundation and the TypeScript declarative bridge foundation.
- The DirectX 11 backend now creates the device, swap chain, render target, shaders, Direct2D text path, clip/scissor path, and theme-driven widget renderer for the live sample path.
- The DirectX 12 backend now owns a real draw path, explicit host rebinding hooks, and transport/resource validation for hostile host runtimes.

## Runtime validation

- Use [examples/win32-dx11-sample/main.cpp](/C:/Users/tonyi/OneDrive/Documents/GitHub/Imgui-Gui-React/examples/win32-dx11-sample/main.cpp) for live DX11 validation.
- Use [examples/win32-dx12-sample/main.cpp](/C:/Users/tonyi/OneDrive/Documents/GitHub/Imgui-Gui-React/examples/win32-dx12-sample/main.cpp) for live DX12 validation.
- Use the `C++: Win32 DX11 Sample` launch configuration in [.vscode/launch.json](/C:/Users/tonyi/OneDrive/Documents/GitHub/Imgui-Gui-React/.vscode/launch.json) for debugger-based inspection.
- Use the `C++: Win32 DX12 Sample` and `C++: DX12 Contract Test` launch configurations in [.vscode/launch.json](/C:/Users/tonyi/OneDrive/Documents/GitHub/Imgui-Gui-React/.vscode/launch.json) for runtime debugger-based validation of the DX12 render and host lifecycle paths.
- Use [tests/dx11_backend_tests.cpp](/C:/Users/tonyi/OneDrive/Documents/GitHub/Imgui-Gui-React/tests/dx11_backend_tests.cpp) for hidden-window render, present, resize, and render-state restoration coverage.
- Use [tests/dx11_external_host_tests.cpp](/C:/Users/tonyi/OneDrive/Documents/GitHub/Imgui-Gui-React/tests/dx11_external_host_tests.cpp) for manual host attachment and host-managed present and resize coverage.
- Use [tests/react_runtime_bridge_tests.cpp](/C:/Users/tonyi/OneDrive/Documents/GitHub/Imgui-Gui-React/tests/react_runtime_bridge_tests.cpp) for in-process Hermes runtime loading and payload materialization validation.
- Use [tests/dx12_contract_tests.cpp](/C:/Users/tonyi/OneDrive/Documents/GitHub/Imgui-Gui-React/tests/dx12_contract_tests.cpp) for DX12 host-frame binding validation and Hermes-authored external-host coverage.
