import type { TransportShaderResource } from "@igr/core";

import { menuAccentShaderSource, menuHeaderShaderSource } from "./generated/menu-shaders.generated.js";

export const menuShaders: readonly TransportShaderResource[] = [
  {
    key: "menu-banner-hlsl",
    pixel: {
      language: "hlsl",
      entryPoint: "main",
      source: menuHeaderShaderSource
    },
    samplesTexture: false,
    blendMode: "alpha"
  },
  {
    key: "menu-preview-glsl",
    pixel: {
      language: "glsl",
      entryPoint: "main",
      source: menuAccentShaderSource
    },
    samplesTexture: true,
    blendMode: "alpha"
  }
];
