# TypeScript Bridge

## Current implementation

The current TypeScript bridge provides two packages, a native transport envelope, and a runtime-host seam:

- `@igr/core`
  A typed declarative document model with factories, serialization helpers, custom draw nodes, and transport resource typing.
- `@igr/react`
  A JSX runtime that maps React-style authoring into `@igr/core` document trees.
- native bridge transport
  A versioned `igr.document.v1` envelope that the C++ bridge can parse and materialize directly.
- runtime bridge seam
  `RuntimeDocumentBridge` plus `HermesTransportRuntime` now run the TSX-authored scene logic through the packaged Hermes runtime and hand native a transport payload.

This gives the project a clean declarative authoring layer immediately, plus a concrete bridge payload that can cross the JS/native boundary without inventing a second tree format later.

## Why this design

- It keeps the native runtime immediate-mode and deterministic.
- It gives TypeScript users a strongly typed authoring experience now.
- It leaves room for a future `react-reconciler` layer without rewriting the document model or the native materializer seam.

## Transport schema

The transport envelope now carries three resource lanes:

- `fonts`
- `images`
- `shaders`

Shader resources use:

- `key`
- optional `vertex` stage
- required `pixel` stage
- optional `samplesTexture`
- optional `blendMode`

The document layer now also accepts a `shader_rect` element with:

- `shader`
- `x`, `y`, `width`, `height`
- optional `texture`
- optional `resource`
- optional `tint`
- optional `param0` through `param3` as string payloads

It also accepts `shader_image` with:

- `shader`
- `texture`
- optional `resource`
- `width`, `height`
- optional `label`
- optional `tint`
- optional `param0` through `param3`

Shader resources are preserved in the `shaders` lane of the transport envelope and can be authored directly from TSX scene code. The current runtime still expects the host/backend integration layer to apply those resource registrations explicitly before rendering.

## Extension strategy

Custom widgets are supported through `defineElement()`. Third-party packages can create typed extension factories without modifying the native core or the base JSX runtime.

The transport helpers include:

- `createTransportEnvelope()`
- `serializeTransportEnvelope()`
- `parseTransportEnvelope()`
- `renderTransportDocument()`

The runtime seam also includes:

- `RuntimeDocumentBridge`
- `StaticTransportRuntime`
- `HermesTransportRuntime`

## Next step

The next bridge milestone should add a bidirectional event channel from native interaction analysis back into JS, then promote resource/session application from test helpers into a public host-side bridge layer.
