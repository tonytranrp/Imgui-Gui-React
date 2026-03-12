import test from "node:test";
import assert from "node:assert/strict";

import { parseTransportEnvelope } from "@igr/core";

import { createPhysicsTransportEnvelope, createPhysicsTransportPayload, renderTransportForHermes } from "../src/index.js";

test("physics scene emits an external-host transport envelope", () => {
  const envelope = createPhysicsTransportEnvelope({
    frameIndex: 9,
    viewport: { width: 1600, height: 900 },
    deltaSeconds: 1 / 144
  });

  assert.equal(envelope.kind, "igr.document.v1");
  assert.equal(envelope.sequence, 9);
  assert.equal(envelope.session?.host?.hostMode, "injected_overlay");
  assert.equal(envelope.session?.host?.presentationMode, "host_managed");
  assert.equal(envelope.fonts?.length, 2);
  assert.equal(envelope.images?.[0]?.texture, "physics-gradient");
  assert.equal(envelope.root.children.length, 2);
  assert.match(JSON.stringify(envelope.root), /1600x900/);
});

test("physics scene payload round-trips through the core parser", () => {
  const payload = createPhysicsTransportPayload({
    frameIndex: 11,
    viewport: { width: 1024, height: 768 },
    deltaSeconds: 1 / 120
  });
  const envelope = parseTransportEnvelope(payload);

  assert.equal(envelope.sequence, 11);
  assert.equal(envelope.session?.name, "react-native-test-physics");
  assert.equal(envelope.images?.[0]?.key, "physics-gradient-card");
  assert.equal(envelope.root.children[0]?.type, "window");
  assert.match(JSON.stringify(envelope.root), /Viewport=1024x768 frame=11/);
});

test("Hermes entrypoint emits the current transport payload", () => {
  const payload = renderTransportForHermes({
    frameIndex: 12,
    viewport: { width: 800, height: 600 },
    deltaSeconds: 1 / 90
  });
  const envelope = parseTransportEnvelope(payload);

  assert.equal(envelope.sequence, 12);
  assert.equal(envelope.session?.host?.hostMode, "injected_overlay");
  assert.match(JSON.stringify(envelope.root), /800x600/);
});
