import { readdir } from "node:fs/promises";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const rootDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(rootDir, "../..");
const workspaceDirs = [
  path.resolve(rootDir, "../packages"),
  path.resolve(rootDir, "../apps"),
  path.resolve(repoRoot, "tests/menu/dist")
];

async function walk(directory, files = []) {
  const entries = await readdir(directory, { withFileTypes: true });

  for (const entry of entries) {
    const resolved = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      await walk(resolved, files);
      continue;
    }

    if (entry.isFile() && entry.name.endsWith(".test.js")) {
      files.push(resolved);
    }
  }

  return files;
}

const testFiles = [];
for (const workspaceDir of workspaceDirs) {
  try {
    await walk(workspaceDir, testFiles);
  } catch (error) {
    if (error && typeof error === "object" && "code" in error && error.code === "ENOENT") {
      continue;
    }
    throw error;
  }
}
if (testFiles.length === 0) {
  console.error("No compiled test files were found under npm workspaces.");
  process.exit(1);
}

const result = spawnSync(process.execPath, ["--test", "--test-concurrency=1", "--test-isolation=none", ...testFiles], {
  cwd: path.resolve(rootDir, ".."),
  stdio: "inherit"
});

process.exit(result.status ?? 1);
