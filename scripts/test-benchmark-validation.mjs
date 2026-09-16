import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { generateBenchmarkPages } from './generate-v1.1-benchmark-pages.mjs';

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'benchmark validation '));
let outputRoot = path.join(temporary, 'reports');
const validator = path.join(repositoryRoot, 'scripts', 'validate-public-v1.1-multistream-benchmark.mjs');

function validate(args = ['--output-root', outputRoot]) {
  const result = spawnSync(process.execPath, [validator, ...args], { encoding: 'utf8' });
  assert.ifError(result.error);
  return { status: result.status, output: result.stdout + result.stderr };
}

function walk(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const file = path.join(directory, entry.name);
    return entry.isDirectory() ? walk(file) : [file];
  });
}

function snapshot() {
  return Object.fromEntries(walk(outputRoot).sort().map((file) => [
    path.relative(outputRoot, file).replaceAll('\\', '/'),
    fs.readFileSync(file),
  ]));
}

function writeChecksums() {
  const entries = Object.entries(snapshot()).filter(([name]) => name !== 'SHA256SUMS');
  fs.writeFileSync(path.join(outputRoot, 'SHA256SUMS'), entries.map(([name, bytes]) =>
    `${createHash('sha256').update(bytes).digest('hex')}  ${name}\n`).join(''));
}

function mutate(file, update, { reason = null, rehash = true } = {}) {
  const target = path.join(outputRoot, file);
  const original = fs.readFileSync(target);
  const checksums = fs.readFileSync(path.join(outputRoot, 'SHA256SUMS'));
  try {
    const changed = update(original.toString('utf8'));
    assert.notEqual(changed, original.toString('utf8'), 'fixture mutation must apply');
    if (changed === null) fs.rmSync(target);
    else fs.writeFileSync(target, changed);
    if (rehash) writeChecksums();
    const before = snapshot();
    const result = validate();
    assert.equal(result.status === 0, reason === null, result.output);
    if (reason) assert.match(result.output, reason);
    assert.deepEqual(snapshot(), before, 'output validation must not repair or regenerate the caller-owned pack');
  } finally {
    fs.writeFileSync(target, original);
    fs.writeFileSync(path.join(outputRoot, 'SHA256SUMS'), checksums);
  }
}

try {
  const source = validate(['--source-only']);
  assert.equal(source.status, 0, source.output);
  assert.match(source.output, /generated report links remain for the output check/);

  const generation = generateBenchmarkPages({ outputRoot });
  // These release expectations are independent of the generator's return value.
  const reports = Object.keys(snapshot()).filter((name) => name.endsWith('.html'));
  assert.equal(reports.length, 148);
  for (const required of ['report.html', 'report.zh-CN.html',
    'results/bm1688/cases/person-24fps-8ch/report.html',
    'results/dual-cv-72h/report.zh-CN.html']) {
    assert.ok(reports.includes(required), `missing independently expected report: ${required}`);
  }
  assert.equal(generation.reportCount, reports.length);
  const before = snapshot();
  const existing = validate();
  assert.equal(existing.status, 0, existing.output);
  assert.deepEqual(snapshot(), before, 'successful validation does not rewrite the existing output');

  mutate('report.zh-CN.html', () => null, { reason: /report inventory.*missing/ });
  mutate('results/bm1688/cases.json', (text) => text + '\n', {
    reason: /generated SHA256SUMS mismatch/, rehash: false,
  });
  mutate('report.html', (text) => text.replace('</body>', '<a href="missing-report.html">Broken</a></body>'), {
    reason: /broken HTML link/,
  });
  mutate('report.html', (text) => text.replace(/\bscope-note\b/u, 'ordinary-note'), {
    reason: /expected 1 scope-note/,
  });
  mutate('report.html', (text) => text
    .replaceAll('overflow-wrap:anywhere', 'overflow-wrap: anywhere')
    .replaceAll('<div class="table"', '<section class="table"')
    .replaceAll('</table></div>', '</table></section>')
    .replaceAll('class="report-nav"', "class = 'extra report-nav'")
    .replace(/href="([^"]+)"/gu, "href = '$1'"));

  // Exercise an actual VitePress output mixed with the generator's portable
  // pack. The site renderer owns only paths backed by canonical Markdown;
  // arbitrary HTML additions must not disappear behind that distinction.
  const siteRoot = path.join(temporary, 'site');
  const siteSources = path.join(siteRoot, 'benchmarks', 'scenario-bench', 'v1.1');
  fs.mkdirSync(path.join(siteRoot, '.vitepress'), { recursive: true });
  // Resolve Vue and VitePress runtime imports from the same installed
  // dependencies as the real documentation build.
  fs.symlinkSync(path.join(repositoryRoot, 'node_modules'), path.join(siteRoot, 'node_modules'), 'dir');
  fs.mkdirSync(path.join(siteSources, 'dataset'), { recursive: true });
  fs.writeFileSync(path.join(siteRoot, '.vitepress', 'config.mjs'),
    "export default { base: '/cosmo-edge/', cleanUrls: true }\n");
  fs.writeFileSync(path.join(siteSources, 'README.md'), '# Benchmark site fixture\n');
  fs.writeFileSync(path.join(siteSources, 'dataset', 'dataset-card.md'), '# Dataset site fixture\n');
  const build = spawnSync(process.execPath, [
    path.join(repositoryRoot, 'node_modules', 'vitepress', 'bin', 'vitepress.js'),
    'build', siteRoot,
  ], { encoding: 'utf8' });
  assert.ifError(build.error);
  assert.equal(build.status, 0, build.stdout + build.stderr);
  outputRoot = path.join(siteRoot, '.vitepress', 'dist', 'benchmarks', 'scenario-bench', 'v1.1');
  assert.match(fs.readFileSync(path.join(outputRoot, 'README.html'), 'utf8'), /\/cosmo-edge\/assets\//u);
  generateBenchmarkPages({ outputRoot });
  const mixedBefore = snapshot();
  assert.equal(Object.keys(mixedBefore).filter((name) => name.endsWith('.html')).length, 150);
  const mixed = validate();
  assert.equal(mixed.status, 0, mixed.output);
  assert.deepEqual(snapshot(), mixedBefore);
  mutate('README.html', (text) => text + '\n', { reason: /generated SHA256SUMS mismatch/, rehash: false });
  mutate('report.zh-CN.html', () => null, { reason: /report inventory.*missing/ });
  mutate('report.html', (text) => text.replace('</body>', '<a href="/cosmo-edge/missing">Wrong scope</a></body>'), {
    reason: /HTML link is not relative/, // Portable reports still reject site-root links.
  });
  const unexpected = path.join(outputRoot, 'unexpected.html');
  fs.writeFileSync(unexpected, '<!doctype html><html><title>Unexpected</title></html>');
  writeChecksums();
  const unexpectedSnapshot = snapshot();
  const extra = validate();
  assert.notEqual(extra.status, 0, extra.output);
  assert.match(extra.output, /report inventory contains unexpected entry: unexpected\.html/u);
  assert.deepEqual(snapshot(), unexpectedSnapshot);

  const standalone = validate([]);
  assert.equal(standalone.status, 0, standalone.output);
  console.log('Benchmark validation passed: source-only, caller-owned output, independent report inventory, missing report, checksum, link and disclosure failures, equivalent markup, real VitePress mixed output, unexpected HTML, and standalone generation.');
} finally {
  fs.rmSync(temporary, { recursive: true, force: true });
}
