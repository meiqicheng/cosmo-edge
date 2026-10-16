#!/usr/bin/env node
// Refresh the ScenarioBench benchmarkTool hashes recorded in the v1.1
// release manifest from the tracked tool sources. The manifest must never be
// hand-edited for hashes; run this script after any tracked ScenarioBench
// source change and commit the result together with the tool change.
//
// Usage: node scripts/refresh-v1.1-tool-hashes.mjs
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const workspace = path.resolve(here, '..');
const manifestPath = path.join(
  workspace,
  'docs', 'benchmarks', 'scenario-bench', 'v1.1', 'release-manifest.json',
);

const benchmarkToolFiles = {
  cliSha256: 'tools/scenario-bench/src/cli.js',
  evaluatorSha256: 'tools/scenario-bench/src/step-evaluator.js',
  metricsSamplerSha256: 'tools/scenario-bench/src/metrics-sampler.js',
  reportWriterSha256: 'tools/scenario-bench/src/report-writer.js',
  vlmReadinessSha256: 'tools/scenario-bench/src/vlm-readiness.js',
  scenarioPackageSha256: 'tools/scenario-bench/src/scenario-package.js',
  taskRunnerSha256: 'tools/scenario-bench/src/task-runner.js',
  lockfileSha256: 'tools/scenario-bench/package-lock.json',
};

const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
let changed = false;
for (const [field, relative] of Object.entries(benchmarkToolFiles)) {
  const file = path.join(workspace, ...relative.split('/'));
  if (!fs.existsSync(file)) {
    console.error(`missing tracked tool source: ${relative}`);
    process.exitCode = 1;
    continue;
  }
  const digest = crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
  if (manifest.benchmarkTool?.[field] !== digest) {
    console.log(`${field}: ${manifest.benchmarkTool?.[field] ?? '<missing>'} -> ${digest}`);
    manifest.benchmarkTool[field] = digest;
    changed = true;
  }
}
if (changed) {
  fs.writeFileSync(manifestPath, `${JSON.stringify(manifest, null, 2)}\n`, 'utf8');
  console.log(`Updated ${path.relative(workspace, manifestPath)}`);
} else {
  console.log('benchmarkTool hashes are already current');
}
