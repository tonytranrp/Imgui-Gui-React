import { createPhysicsTransportPayload, type PhysicsFrameRequest } from "./index.js";

export type HermesFrameRequest = PhysicsFrameRequest;

declare global {
  interface Window {
    __igrRenderTransport?: (request?: HermesFrameRequest) => string;
  }

  var __igrRenderTransport: ((request?: HermesFrameRequest) => string) | undefined;
}

export function renderTransportForHermes(request: HermesFrameRequest = {}): string {
  return createPhysicsTransportPayload(request);
}

globalThis.__igrRenderTransport = renderTransportForHermes;
