import { createMenuTransportPayload, type MenuFrameRequest } from "./menu-scene.js";

let publishedResources = false;

declare global {
  interface Window {
    __igrRenderMenuTransport?: (request?: MenuFrameRequest) => string;
  }

  var __igrRenderMenuTransport: ((request?: MenuFrameRequest) => string) | undefined;
}

export function renderMenuTransportForHermes(request: MenuFrameRequest = {}): string {
  const payload = createMenuTransportPayload(request, {
    includeResources: !publishedResources,
    resourceMode: "retain"
  });
  publishedResources = true;
  return payload;
}

globalThis.__igrRenderMenuTransport = renderMenuTransportForHermes;
