import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const sourceRoot = path.join(repositoryRoot, 'docs', 'benchmarks', 'scenario-bench');
const outputRoot = path.join(repositoryRoot, 'docs', '.vitepress', 'dist', 'benchmarks', 'scenario-bench');
const copied = [];

// v1.1 has a canonical-data-only source layout and its static/public artifacts
// are emitted by generate-v1.1-benchmark-pages.mjs after VitePress completes.
for (const version of ['v1.0', 'current']) {
  const source = path.join(sourceRoot, version);
  if (!fs.existsSync(source)) continue;
  for (const file of walk(source)) {
    if (!staticBenchmarkAsset(file)) continue;
    const relative = path.relative(sourceRoot, file);
    const target = path.join(outputRoot, relative);
    fs.mkdirSync(path.dirname(target), { recursive: true });
    fs.copyFileSync(file, target);
    copied.push(relative.replaceAll('\\', '/'));
  }
}

console.log(`Copied ${copied.length} static benchmark assets into the VitePress output.`);

function staticBenchmarkAsset(file) {
  return !/\.md$/i.test(file);
}

function walk(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const full = path.join(directory, entry.name);
    return entry.isDirectory() ? walk(full) : [full];
  });
}
