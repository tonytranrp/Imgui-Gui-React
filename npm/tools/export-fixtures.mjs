import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { mkdir } from "node:fs/promises";

const toolsDir = path.dirname(fileURLToPath(import.meta.url));
const workspaceRoot = path.resolve(toolsDir, "..");
const repoRoot = path.resolve(workspaceRoot, "..");
const appDistEntry = path.resolve(workspaceRoot, "apps/react-native-test/dist/src/export-fixture.js");
const menuDistEntry = path.resolve(repoRoot, "tests/menu/dist/export-fixture.js");

const fixtureModule = await import(pathToFileURL(appDistEntry).href);
const menuFixtureModule = await import(pathToFileURL(menuDistEntry).href);

if (typeof fixtureModule.exportPhysicsFixture !== "function") {
  console.error("The react-native-test fixture exporter was not found.");
  process.exit(1);
}
if (typeof menuFixtureModule.exportMenuFixture !== "function") {
  console.error("The menu fixture exporter was not found.");
  process.exit(1);
}

const outputPathArg = process.argv[2];
const outputPath = outputPathArg
  ? path.resolve(workspaceRoot, outputPathArg)
  : path.resolve(workspaceRoot, "apps/react-native-test/dist/react-native-test.physics.json");
const menuOutputPath = path.resolve(repoRoot, "tests/menu/dist/react-menu.fixture.json");
await mkdir(path.dirname(outputPath), { recursive: true });
await mkdir(path.dirname(menuOutputPath), { recursive: true });
await fixtureModule.exportPhysicsFixture(outputPath);
await menuFixtureModule.exportMenuFixture(menuOutputPath);

console.log(`Exported React fixture to ${outputPath}`);
console.log(`Exported menu fixture to ${menuOutputPath}`);
