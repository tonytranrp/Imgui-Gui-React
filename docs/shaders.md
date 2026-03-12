# Shaders

## Overview

IGR UI now supports user-authored shader widgets on both DX11 and DX12.

Supported entry points:

- native C++ via `FrameBuilder::shader_rect()` and `FrameBuilder::shader_image()`
- React/TSX via `shader_rect` and `shader_image`
- transport resources via the `shaders` array on `igr.document.v1`

Supported source languages:

- HLSL
- GLSL

GLSL is translated through `glslang` and `SPIRV-Cross`, then compiled into DirectX bytecode by the shared shader compiler.

## Resource model

Shaders are registered per backend with `register_shader(key, ShaderResourceDesc)`.

`ShaderResourceDesc` currently supports:

- optional `vertex` stage
- required `pixel` stage
- `samples_texture`
- `blend_mode`

This is the v1 contract. It is intentionally narrow and stable:

- one optional sampled texture
- one per-draw constant block
- alpha, opaque, or additive blending

## Uniform ABI

Both DX11 and DX12 use the same per-draw constant layout:

```hlsl
cbuffer IgrShaderConstants : register(b0) {
  float4 igrTint;
  float4 igrParam0;
  float4 igrParam1;
  float4 igrParam2;
  float4 igrParam3;
  float4 igrRect;
  float4 igrViewportAndTime;
  float4 igrFrameData;
};
```

Field meaning:

- `igrTint`: widget tint after resource tinting
- `igrParam0..igrParam3`: user-defined parameters from `ShaderUniformData`
- `igrRect`: `x`, `y`, `width`, `height`
- `igrViewportAndTime`: `viewportWidth`, `viewportHeight`, `timeSeconds`, `deltaSeconds`
- `igrFrameData`: `frameIndex`, reserved, reserved, reserved

For pixel shaders that use the built-in vertex stage, the expected input shape is:

```hlsl
struct PSInput {
  float4 position : SV_POSITION;
  float4 color : COLOR;
  float2 uv : TEXCOORD0;
};
```

## Native example

```cpp
igr::ShaderResourceDesc shader{};
shader.pixel.language = igr::ShaderLanguage::hlsl;
shader.pixel.entry_point = "main";
shader.pixel.source = R"(
cbuffer IgrShaderConstants : register(b0) {
  float4 igrTint;
  float4 igrParam0;
  float4 igrParam1;
  float4 igrParam2;
  float4 igrParam3;
  float4 igrRect;
  float4 igrViewportAndTime;
  float4 igrFrameData;
};

struct PSInput {
  float4 position : SV_POSITION;
  float4 color : COLOR;
  float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
  return float4(input.uv.x, input.uv.y, 0.5, 1.0) * igrTint * input.color;
}
)";
backend.register_shader("pulse-shader", shader);

igr::ShaderUniformData uniforms{};
uniforms.tint = {0.72f, 0.90f, 1.0f, 0.9f};
uniforms.params[0] = {1.0f, 1.0f / 60.0f, 1280.0f, 720.0f};
builder.shader_rect("pulse", "pulse-shader", {{12.0f, 16.0f}, {180.0f, 32.0f}}, {}, {}, uniforms);
```

## React example

```tsx
const shaders = [
  {
    key: "pulse-shader",
    pixel: {
      language: "hlsl",
      entryPoint: "main",
      source: `...`
    },
    samplesTexture: false,
    blendMode: "alpha"
  }
];

<shader_rect
  key="pulse"
  shader="pulse-shader"
  x={12}
  y={16}
  width={180}
  height={32}
  tint="#9CE0FFFF"
  param0="42, 0.016667, 1280, 720"
/>
```

## Current limits

- the shared ABI is fixed to one constant block and one optional texture
- transport/session resource application is still host-driven, not automatic inside `RuntimeDocumentBridge`
- DX12 still uses the shared SM5 compile path; the shader authoring contract is stable, but DXC integration can still be added later without changing the API
