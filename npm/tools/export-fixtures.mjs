import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { mkdir } from "node:fs/promises";

const toolsDir = path.dirname(fileURLToPath(import.meta.url));
const workspaceRoot = path.resolve(toolsDir, "..");
const appDistEntry = path.resolve(workspaceRoot, "apps/react-native-test/dist/src/export-fixture.js");

const fixtureModule = await import(pathToFileURL(appDistEntry).href);

if (typeof fixtureModule.exportPhysicsFixture !== "function") {
  console.error("The react-native-test fixture exporter was not found.");
  process.exit(1);
}

const outputPath = path.resolve(workspaceRoot, "../tests/fixtures/react-native-test.physics.json");
await mkdir(path.dirname(outputPath), { recursive: true });
await fixtureModule.exportPhysicsFixture(outputPath);

console.log(`Exported React fixture to ${outputPath}`);
