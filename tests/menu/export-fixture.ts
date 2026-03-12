import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";

import { createMenuTransportPayload } from "./menu-scene.js";

export async function exportMenuFixture(outputPath: string): Promise<void> {
  await mkdir(path.dirname(outputPath), { recursive: true });
  await writeFile(
    outputPath,
    `${createMenuTransportPayload({
      frameIndex: 12,
      viewport: { width: 1280, height: 720 },
      deltaSeconds: 1 / 60,
      state: {
        selectedTab: "overview",
        showStats: true,
        glowEnabled: true,
        accentIndex: 1,
        applyCount: 2,
        lastAction: "Fixture export"
      }
    })}\n`,
    "utf8"
  );
}

if (import.meta.url === `file://${process.argv[1]?.replace(/\\/g, "/")}` && process.argv[2]) {
  await exportMenuFixture(path.resolve(process.argv[2]));
}
