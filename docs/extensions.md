# Extensions

## Goal

IGR UI should be extensible without forcing downstream projects to fork the core library just to add custom declarative widgets or package-level helpers.

## TypeScript Extension Path

The current extension surface is `defineElement()` in `@igr/core`.

Example:

```ts
import { defineElement } from "@igr/core";

export interface BadgeProps {
  label: string;
  tone?: "info" | "warning" | "success";
}

export const badge = defineElement<BadgeProps>("badge");
```

That gives extension packages:

- typed authoring
- JSX-friendly composition through `@igr/react`
- serializable declarative trees
- clean package boundaries

## Native Extension Path

On the native side, custom elements currently materialize through explicit support in [src/react/document.cpp](/C:/Users/tonyi/OneDrive/Documents/GitHub/Imgui-Gui-React/src/react/document.cpp). That is the correct first milestone because it keeps unsupported element handling explicit and testable.

The native runtime now also exposes low-level shared draw primitives through the frame builder:

- `fill_rect()`
- `stroke_rect()`
- `draw_line()`

That gives downstream packages a sanctioned way to add accents, graph rules, separators, and overlay diagnostics without forking the DX11 or DX12 backends.

The next extension milestone should introduce a registry-based materializer hook so downstream packages can register custom element translators without modifying the core bridge.

## Current Recommendation

For the present DX11 milestone:

- use `defineElement()` for TypeScript-side custom authoring
- keep native materialization explicit
- add tests whenever new element types are introduced
- avoid stringly typed extension magic in the core runtime
