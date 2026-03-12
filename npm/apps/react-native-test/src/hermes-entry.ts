import { createPhysicsTransportPayload, type PhysicsFrameRequest } from "./index.js";

let publishedResources = false;

export type HermesFrameRequest = PhysicsFrameRequest;

declare global {
  interface Window {
    __igrRenderTransport?: (request?: HermesFrameRequest) => string;
  }

  var __igrRenderTransport: ((request?: HermesFrameRequest) => string) | undefined;
}

export function renderTransportForHermes(request: HermesFrameRequest = {}): string {
  const payload = createPhysicsTransportPayload(request, {
    includeResources: !publishedResources,
    resourceMode: "retain"
  });
  publishedResources = true;
  return payload;
}

globalThis.__igrRenderTransport = renderTransportForHermes;
