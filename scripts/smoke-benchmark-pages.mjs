import fs from 'node:fs';
import { spawnSync } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const repositoryRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const distRoot = path.resolve(repositoryRoot, process.argv[2] ?? 'docs/.vitepress/dist');
const benchmarkRoot = path.join(distRoot, 'benchmarks', 'scenario-bench', 'v1.1');
const failures = [];

const validation = spawnSync(process.execPath, [
  path.join(repositoryRoot, 'scripts', 'validate-public-v1.1-multistream-benchmark.mjs'),
  '--output-root', benchmarkRoot,
], { stdio: 'inherit' });
if (validation.error) throw validation.error;
if (validation.status !== 0) process.exit(validation.status ?? 1);

const canonicalPublicReportLinks = validateCanonicalPublicReportLinks();
if (failures.length) {
  console.error(`Benchmark site smoke test failed with ${failures.length} issue(s):`);
  for (const failure of failures) console.error(`- ${failure}`);
  process.exit(1);
}
console.log(`Benchmark site smoke test passed: ${canonicalPublicReportLinks} canonical website report links.`);

function walk(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const full = path.join(directory, entry.name);
    return entry.isDirectory() ? walk(full) : [full];
  });
}

function validateCanonicalPublicReportLinks() {
  const docsRoot = path.join(repositoryRoot, 'docs');
  const benchmarkSourceRoot = path.join(docsRoot, 'benchmarks', 'scenario-bench');
  const markdownFiles = [
    path.join(repositoryRoot, 'README.md'),
    path.join(repositoryRoot, 'README.zh-CN.md'),
    ...walk(docsRoot).filter((file) => file.endsWith('.md') && !file.includes(`${path.sep}.vitepress${path.sep}`)),
  ];
  let checked = 0;

  for (const file of markdownFiles) {
    const source = fs.readFileSync(file, 'utf8');
    const targets = new Set([
      ...[...source.matchAll(/!?\[[^\]]*\]\(([^)\s]+)(?:\s+["'][^)]*)?\)/g)]
        .map((match) => match[1].replace(/^<|>$/g, '')),
      ...[...source.matchAll(/<a\b[^>]*href=["']([^"']+)["']/gi)].map((match) => match[1]),
    ]);
    const benchmarkReadme = file.startsWith(`${benchmarkSourceRoot}${path.sep}`)
      && /^README(?:\.zh-CN)?\.md$/u.test(path.basename(file));

    for (const target of targets) {
      if (!/\.html(?:[?#].*)?$/iu.test(target)) continue;
      if (!benchmarkReadme && !target.includes('benchmarks/scenario-bench')) continue;
      checked += 1;

      let url;
      try {
        url = new URL(target);
      } catch {
        failures.push(`${path.relative(repositoryRoot, file)}: benchmark report must use the canonical website URL: ${target}`);
        continue;
      }
      if (url.origin !== 'https://www.cosmowander.ai') {
        failures.push(`${path.relative(repositoryRoot, file)}: benchmark report uses a non-canonical origin: ${target}`);
        continue;
      }

      const match = url.pathname.match(/^\/(?:zh\/)?docs\/(benchmarks\/scenario-bench\/.+\.html)$/u);
      if (!match) {
        failures.push(`${path.relative(repositoryRoot, file)}: benchmark report has a non-canonical website path: ${target}`);
        continue;
      }
      const chineseReport = url.pathname.endsWith('.zh-CN.html');
      const chineseRoute = url.pathname.startsWith('/zh/docs/');
      if (chineseReport !== chineseRoute) {
        failures.push(`${path.relative(repositoryRoot, file)}: report language and website locale differ: ${target}`);
      }

      const builtTarget = path.resolve(distRoot, ...decodeURIComponent(match[1]).split('/'));
      if (!builtTarget.startsWith(`${distRoot}${path.sep}`) || !fs.existsSync(builtTarget)) {
        failures.push(`${path.relative(repositoryRoot, file)}: canonical website report is missing from the build: ${target}`);
      }
    }
  }
  return checked;
}
