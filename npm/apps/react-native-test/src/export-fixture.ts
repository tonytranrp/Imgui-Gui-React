import { mkdir, writeFile } from "node:fs/promises";
import path from "node:path";

import { createPhysicsTransportPayload } from "./index.js";

export async function exportPhysicsFixture(outputPath: string): Promise<void> {
  await mkdir(path.dirname(outputPath), { recursive: true });
  await writeFile(
    outputPath,
    `${createPhysicsTransportPayload({
      frameIndex: 77,
      viewport: { width: 1280, height: 720 },
      deltaSeconds: 1 / 60
    })}\n`,
    "utf8"
  );
}

if (import.meta.url === `file://${process.argv[1]?.replace(/\\/g, "/")}` && process.argv[2]) {
  await exportPhysicsFixture(path.resolve(process.argv[2]));
}
