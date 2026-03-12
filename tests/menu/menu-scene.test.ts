import test from "node:test";
import assert from "node:assert/strict";

import { parseTransportEnvelope } from "@igr/core";

import { createMenuTransportEnvelope, createMenuTransportPayload } from "./menu-scene.js";

type TestNode = {
  key?: string;
  children?: TestNode[];
};

function hasKey(node: TestNode, key: string): boolean {
  if (node.key === key) {
    return true;
  }
  return Array.isArray(node.children) && node.children.some((child) => hasKey(child, key));
}

test("menu scene emits clickable controls and transport resources", () => {
  const envelope = createMenuTransportEnvelope({
    frameIndex: 9,
    viewport: { width: 1440, height: 900 },
    deltaSeconds: 1 / 120,
    state: {
      selectedTab: "react",
      showStats: false,
      glowEnabled: false,
      accentIndex: 2,
      applyCount: 4,
      lastAction: "State injected"
    }
  });

  assert.equal(envelope.session?.name, "react-test-menu");
  assert.equal(envelope.shaders?.length, 2);
  assert.equal(envelope.images?.length, 1);
  assert.ok(hasKey(envelope.root, "tab-react"));
  assert.ok(hasKey(envelope.root, "action-apply"));
  assert.equal(hasKey(envelope.root, "menu-stats-window"), false);
});

test("menu transport payload round-trips through the core parser", () => {
  const payload = createMenuTransportPayload({
    frameIndex: 14,
    viewport: { width: 1280, height: 720 },
    deltaSeconds: 1 / 60,
    state: {
      selectedTab: "render",
      showStats: true,
      glowEnabled: true,
      accentIndex: 3,
      applyCount: 5,
      lastAction: "Round-trip"
    }
  });

  const parsed = parseTransportEnvelope(payload);
  assert.equal(parsed.sequence, 14);
  assert.ok(hasKey(parsed.root, "toggle-glow"));
  assert.ok(hasKey(parsed.root, "menu-stats-window"));
});
