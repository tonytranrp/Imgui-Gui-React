import path from "node:path";
import { fileURLToPath } from "node:url";
import { mkdir } from "node:fs/promises";
import { spawnSync } from "node:child_process";

import { build } from "esbuild";

const toolsDir = path.dirname(fileURLToPath(import.meta.url));
const workspaceRoot = path.resolve(toolsDir, "..");
const repoRoot = path.resolve(workspaceRoot, "..");
const outputDir = path.resolve(repoRoot, "artifacts/hermes");
const nodePaths = [path.resolve(workspaceRoot, "node_modules")];
const hermesVersion = process.env.IGR_HERMES_VERSION ?? "0.1.27";
const hermesPackageRoot =
  process.env.IGR_HERMES_PACKAGE_DIR ?? path.resolve(repoRoot, `.cache/hermes/${hermesVersion}`);
const packagedHermesCompiler = path.resolve(hermesPackageRoot, "tools/native/release/x86/hermes.exe");

await mkdir(outputDir, { recursive: true });

async function buildHermesBundle({ entryPoint, globalName, jsBundlePath, bannerVersion }) {
  await build({
    entryPoints: [entryPoint],
    bundle: true,
    format: "iife",
    globalName,
    platform: "neutral",
    target: "es2020",
    outfile: jsBundlePath,
    sourcemap: false,
    legalComments: "none",
    nodePaths,
    banner: {
      js: `globalThis.__${globalName}Version = '${bannerVersion}';`
    }
  });
}

let hermesCompiler = process.env.HERMESC;
if (!hermesCompiler) {
  const whereResult = spawnSync("where", ["hermesc"], {
    cwd: repoRoot,
    encoding: "utf8",
    stdio: ["ignore", "pipe", "ignore"]
  });
  if (whereResult.status === 0) {
    hermesCompiler = whereResult.stdout.split(/\r?\n/).find(Boolean);
  }
}
if (!hermesCompiler) {
  const hermesExeWhere = spawnSync("where", ["hermes.exe"], {
    cwd: repoRoot,
    encoding: "utf8",
    stdio: ["ignore", "pipe", "ignore"]
  });
  if (hermesExeWhere.status === 0) {
    hermesCompiler = hermesExeWhere.stdout.split(/\r?\n/).find(Boolean);
  }
}
if (!hermesCompiler) {
  const fs = await import("node:fs/promises");
  try {
    await fs.access(packagedHermesCompiler);
    hermesCompiler = packagedHermesCompiler;
  } catch {
    hermesCompiler = undefined;
  }
}

function emitBytecode(hermesCompilerPath, jsBundlePath, bytecodePath) {
  const compileResult = spawnSync(hermesCompilerPath, ["-emit-binary", "-out", bytecodePath, jsBundlePath], {
    cwd: repoRoot,
    encoding: "utf8",
    stdio: "inherit"
  });
  if ((compileResult.status ?? 1) !== 0) {
    process.exit(compileResult.status ?? 1);
  }
}

const bundleJobs = [
  {
    entryPoint: path.resolve(workspaceRoot, "apps/react-native-test/src/hermes-entry.ts"),
    globalName: "igrHermesBundle",
    jsBundlePath: path.resolve(outputDir, "react-native-test.bundle.js"),
    bytecodePath: path.resolve(outputDir, "react-native-test.bundle.hbc"),
    bannerVersion: "0.1.0-alpha"
  },
  {
    entryPoint: path.resolve(repoRoot, "tests/menu/hermes-entry.ts"),
    globalName: "igrMenuHermesBundle",
    jsBundlePath: path.resolve(outputDir, "react-menu-test.bundle.js"),
    bytecodePath: path.resolve(outputDir, "react-menu-test.bundle.hbc"),
    bannerVersion: "0.1.0-alpha"
  }
];

for (const job of bundleJobs) {
  await buildHermesBundle(job);
  if (hermesCompiler) {
    emitBytecode(hermesCompiler, job.jsBundlePath, job.bytecodePath);
    console.log(`Built Hermes bytecode bundle at ${job.bytecodePath}`);
  } else {
    console.log(`Built JavaScript Hermes entry bundle at ${job.jsBundlePath}; hermesc was not found, so bytecode emission was skipped.`);
  }

  console.log(`Built Hermes source bundle at ${job.jsBundlePath}`);
}
