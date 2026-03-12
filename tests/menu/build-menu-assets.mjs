import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { mkdir, readFile, rm, writeFile } from "node:fs/promises";
import { spawnSync } from "node:child_process";

const menuDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(menuDir, "..", "..");
const npmRoot = path.resolve(repoRoot, "npm");
const esbuildEntry = path.resolve(npmRoot, "node_modules/esbuild/lib/main.js");
const generatedDir = path.resolve(menuDir, "generated");
const distDir = path.resolve(menuDir, "dist");
const tsconfigPath = path.resolve(menuDir, "tsconfig.json");
const tscEntry = path.resolve(npmRoot, "node_modules/typescript/bin/tsc");
const nodePaths = [path.resolve(npmRoot, "node_modules")];
const { build } = await import(pathToFileURL(esbuildEntry).href);

async function writeGeneratedShaders() {
  await mkdir(generatedDir, { recursive: true });
  const headerShader = await readFile(path.resolve(menuDir, "menu-header.hlsl"), "utf8");
  const accentShader = await readFile(path.resolve(menuDir, "menu-accent.glsl"), "utf8");
  const generatedSource = [
    `export const menuHeaderShaderSource = ${JSON.stringify(headerShader)};`,
    `export const menuAccentShaderSource = ${JSON.stringify(accentShader)};`,
    ""
  ].join("\n");
  await writeFile(path.resolve(generatedDir, "menu-shaders.generated.ts"), generatedSource, "utf8");
}

function runTypeScriptCheck() {
  const compileResult = spawnSync(process.execPath, [tscEntry, "-p", tsconfigPath, "--noEmit"], {
    cwd: repoRoot,
    stdio: "inherit"
  });
  if ((compileResult.status ?? 1) !== 0) {
    process.exit(compileResult.status ?? 1);
  }
}

async function bundleMenuEntries() {
  await rm(distDir, { recursive: true, force: true });
  await mkdir(distDir, { recursive: true });

  await build({
    entryPoints: {
      "menu-scene": path.resolve(menuDir, "menu-scene.tsx"),
      "menu-scene.test": path.resolve(menuDir, "menu-scene.test.ts"),
      "export-fixture": path.resolve(menuDir, "export-fixture.ts"),
      "hermes-entry": path.resolve(menuDir, "hermes-entry.ts")
    },
    outdir: distDir,
    bundle: true,
    format: "esm",
    platform: "node",
    target: "es2022",
    sourcemap: false,
    legalComments: "none",
    logLevel: "info",
    nodePaths
  });
}

await writeGeneratedShaders();
runTypeScriptCheck();
await bundleMenuEntries();

console.log(`Built menu assets under ${distDir}`);
