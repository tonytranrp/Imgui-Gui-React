/** @jsxImportSource @igr/react */

import assert from "node:assert/strict";
import test from "node:test";

import { countNodes, renderDocument, renderTransportDocument } from "../src/index.js";

test("jsx runtime builds a serializable document tree", () => {
  const document = (
    <fragment>
      <window title="Main" x={24} y={24} width={320} height={200}>
        <stack axis="vertical">
          <text value="Hello JSX" />
          <button label="Open" enabled />
          <checkbox label="Enabled" checked />
          <clip_rect width={120} height={48}>
            <text value="Clipped content" />
            <fill_rect width={24} height={12} color="#62C7FF" />
            <line x1={0} y1={22} x2={120} y2={22} thickness={2} color="#224055" />
          </clip_rect>
          <image texture="demo-image" width={96} height={48} label="Preview" />
          <progress_bar label="Load" value={0.6} />
        </stack>
      </window>
    </fragment>
  );

  assert.equal(countNodes(document), 12);
  assert.match(renderDocument(document), /"Main"/);
  assert.match(renderDocument(document), /"Hello JSX"/);
  assert.match(renderDocument(document), /"checkbox"/);
  assert.match(renderDocument(document), /"clip_rect"/);
  assert.match(renderDocument(document), /"image"/);
  assert.match(renderDocument(document), /"progress_bar"/);
  assert.match(renderDocument(document), /"fill_rect"/);
  assert.match(renderDocument(document), /"line"/);
});

test("renderTransportDocument emits the native bridge envelope", () => {
  const payload = renderTransportDocument(<text value="Transport" font="mono-sm" />, 3, {
    session: {
      name: "transport-test",
      host: {
        hostMode: "injected_overlay",
        presentationMode: "host_managed"
      }
    },
    fonts: [{ key: "mono-sm", family: "Consolas", size: 13 }]
  });
  assert.match(payload, /igr\.document\.v1/);
  assert.match(payload, /mono-sm/);
  assert.match(payload, /"sequence":3/);
  assert.match(payload, /transport-test/);
  assert.match(payload, /injected_overlay/);
});
