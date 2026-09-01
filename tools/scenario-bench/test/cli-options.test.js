import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

test('rejects an invalid VLM readiness timeout before scenario or device work', (t) => {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-bench-cli-options-'));
  t.after(() => fs.rmSync(temp, { recursive: true, force: true }));
  const output = path.join(temp, 'report-output');
  const result = spawnSync(process.execPath, [
    path.join(root, 'src/cli.js'),
    'run',
    '--device', 'http://127.0.0.1:1',
    '--scenario', path.join(temp, 'missing-scenario'),
    '--output', output,
    '--vlm-ready-timeout-sec', 'invalid',
  ], { encoding: 'utf8' });

  assert.equal(result.status, 1);
  assert.match(result.stderr, /--vlm-ready-timeout-sec must be a positive number/);
  assert.doesNotMatch(result.stderr, /scenario\.yml|connect|ECONNREFUSED/i);
  assert.equal(fs.existsSync(output), false);
});
