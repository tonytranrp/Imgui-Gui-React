import assert from "node:assert/strict";
import test from "node:test";

import {
  countNodes,
  createNode,
  createTransportEnvelope,
  defineElement,
  fragment,
  parseTransportEnvelope,
  serializeDocument,
  serializeTransportEnvelope
} from "../src/index.js";

test("createNode normalizes text children into text nodes", () => {
  const node = createNode("window", { title: "Main" }, "hello", 42);
  assert.equal(node.children.length, 2);
  assert.equal(node.children[0]?.type, "text");
  assert.equal(node.children[1]?.props.value, "42");
});

test("countNodes walks nested trees", () => {
  const stack = createNode("stack", { axis: "vertical" }, createNode("button", { label: "Go" }));
  const root = fragment(stack, createNode("separator", {}));
  assert.equal(countNodes(root), 4);
});

test("defineElement creates typed custom widgets", () => {
  const custom = defineElement<{ value: string; tone?: string }>("tag");
  const node = custom({ value: "alpha", tone: "info" });
  assert.equal(node.type, "tag");
  assert.equal(node.props.tone, "info");
  assert.match(serializeDocument(node), /"tag"/);
});

test("serializeDocument preserves clip and image primitives", () => {
  const node = fragment(
    createNode(
      "clip_rect",
      { width: 96, height: 48 },
      createNode("image", { texture: "demo-image", width: 64, height: 32, label: "Preview" })
    )
  );
  const serialized = serializeDocument(node);
  assert.match(serialized, /"clip_rect"/);
  assert.match(serialized, /"demo-image"/);
});

test("transport envelope round-trips a declarative tree", () => {
  const root = fragment(createNode("text", { value: "hello", font: "body-md" }));
  const envelope = createTransportEnvelope(root, 7, {
    session: {
      name: "transport-model-test",
      host: {
        hostMode: "injected_overlay",
        clearTarget: false
      }
    },
    fonts: [{ key: "body-md", family: "Segoe UI", size: 15 }],
    images: [{ key: "preview-card", texture: "preview-texture", width: 96, height: 48 }]
  });
  const parsed = parseTransportEnvelope(serializeTransportEnvelope(envelope));
  assert.equal(parsed.kind, "igr.document.v1");
  assert.equal(parsed.sequence, 7);
  assert.equal(parsed.session?.name, "transport-model-test");
  assert.equal(parsed.images?.[0]?.texture, "preview-texture");
  assert.equal(parsed.root.children[0]?.props.font, "body-md");
});
