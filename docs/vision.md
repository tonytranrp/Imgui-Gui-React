# Vision

IGR UI is a Windows-first immediate-mode UI library that treats React-style authoring as a developer experience layer, not as a web-platform constraint.

## Product goals

- Build a production-grade Windows UI library for DirectX 11 and DirectX 12.
- Preserve immediate-mode rendering performance and deterministic frame control.
- Offer a clean declarative bridge that feels natural to React and TypeScript developers.
- Support public-library packaging, extensibility, testing, and maintainable long-term evolution.

## Non-goals

- Cross-platform backends in the first product line.
- DOM emulation or browser-specific abstractions.
- Weak abstractions that erase real DirectX lifecycle differences.

## Design principles

- Keep the UI core backend-agnostic where possible and backend-aware where necessary.
- Make ownership, frame lifetime, and resource lifetime explicit.
- Build a stable public API surface before layering on advanced features.
- Favor a narrow, strong set of extension points over a wide set of leaky hooks.
- Keep TypeScript support first-class without forcing JS-driven rendering costs into the native core.

