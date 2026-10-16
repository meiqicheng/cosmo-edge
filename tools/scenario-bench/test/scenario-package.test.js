import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { ScenarioPackage } from '../src/scenario-package.js';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

test('loads a single-task scenario through the canonical workload schema', () => {
  const pkg = new ScenarioPackage(
    path.join(root, 'scenarios/no-helmet-99898-fps5-20260630'),
  ).load();

  assert.equal(pkg.scenario.name, 'no-helmet-99898-fps5-20260630');
  assert.equal(pkg.videoMode, 'local');
  assert.equal(pkg.videoRepeatCount, 0);
  assert.equal(pkg.tasks.length, 1);
  assert.equal(pkg.tasks[0].id, 'no-helmet');
  assert.equal(pkg.tasks[0].algorithmId, '99898');
  assert.deepEqual(pkg.bindings, [{ taskId: 'no-helmet', channels: 'all' }]);
  assert.equal(pkg.thresholds.pass.avgDiscardRate, 0.05);
  assert.match(pkg.videos.local[0].file, /LX0000000007\.mp4$/);

  const repeatParam = pkg.taskConfig.params.find((param) => param.key === 'param.videoRepeatCount');
  assert.equal(repeatParam?.value, '0');
});

test('loads a multi-task scenario without a package version flag', () => {
  const pkg = new ScenarioPackage(path.join(root, 'scenarios/multi-task-example')).load();

  assert.equal(pkg.scenario.version, undefined);
  assert.equal(pkg.tasks.length, 2);
  assert.deepEqual(
    pkg.tasks.map((task) => task.id),
    ['no-helmet', 'play-phone'],
  );
  assert.deepEqual(pkg.bindings, [
    { taskId: 'no-helmet', channels: 'all' },
    { taskId: 'play-phone', channels: 'all' },
  ]);
  assert.equal(pkg.thresholds.pass.avgDiscardRate, 0.05);
  assert.match(pkg.videos.local[0].file, /LX0000000007\.mp4$/);

  const readFpsParam = pkg.taskConfig.params.find((param) => param.key === 'param.videoReadFps');
  assert.equal(readFpsParam, undefined);
});

test('injects local video read FPS for direct VLM tasks', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-bench-vlm-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));

  fs.writeFileSync(path.join(dir, 'scenario.yml'), `name: vlm-smoke
sampleIntervalSec: 5
channels:
  mode: local
  repeatCount: 0
  sources:
    - name: vlm
      file: /device/vlm.mp4
tasks:
  - id: vlm
    displayName: VLM
    algorithmId: "55009"
    scheduleId: schedule
    template: algorithm-template.json
    targetFps: 0.1
loadProfile:
  - channels: 1
    holdSec: 120
`, 'utf8');
  fs.writeFileSync(path.join(dir, 'algorithm-template.json'), JSON.stringify({
    algorithmId: '55009',
    algorithmCode: '55009',
    algorithmName: 'VLM',
    taskConfig: { params: [], areas: [] },
    algorithmProcessdata: JSON.stringify([{
      actionId: 'DA_00003',
      name: 'Qwen3VLWorker',
      configObject: { params: [{ key: 'fps', value: '0.1' }] },
    }]),
  }), 'utf8');

  const pkg = new ScenarioPackage(dir).load();
  const params = pkg.taskConfig.params;
  assert.equal(params.find((param) => param.key === 'param.videoRepeatCount')?.value, '0');
  assert.equal(params.find((param) => param.key === 'param.videoReadFps')?.value, '0.1');
  assert.equal(pkg.loadProfile[0].holdSec, 120);
  assert.equal(pkg.tasks[0].vlmCompletionActionId, 'DA_00003');
});

test('defaults omitted VLM hold seconds to 60', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-bench-vlm-default-hold-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));

  fs.writeFileSync(path.join(dir, 'scenario.yml'), `name: vlm-default-hold
sampleIntervalSec: 5
channels:
  mode: local
  repeatCount: 0
  sources:
    - name: vlm
      file: /device/vlm.mp4
tasks:
  - id: vlm
    displayName: VLM
    algorithmId: "55009"
    scheduleId: schedule
    template: algorithm-template.json
    targetFps: 0.1
loadProfile:
  - channels: 1
  - channels: 2
`, 'utf8');
  writeTemplate(path.join(dir, 'algorithm-template.json'), {
    actionId: 'DA_00003',
    name: 'Qwen3VLWorker',
    configObject: { params: [{ key: 'fps', value: '0.1' }] },
  });

  const pkg = new ScenarioPackage(dir).load();
  assert.deepEqual(pkg.loadProfile.map((step) => step.holdSec), [60, 60]);
});

test('defaults omitted CV hold seconds to 30', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-bench-cv-default-hold-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));

  fs.writeFileSync(path.join(dir, 'scenario.yml'), `name: cv-default-hold
sampleIntervalSec: 5
channels:
  mode: local
  repeatCount: 0
  sources:
    - name: cv
      file: /device/cv.mp4
tasks:
  - id: cv
    displayName: CV
    type: cv
    algorithmId: "55009"
    scheduleId: schedule
    template: algorithm-template.json
loadProfile:
  - channels: 1
  - channels: 4
`, 'utf8');
  writeTemplate(path.join(dir, 'algorithm-template.json'), {
    actionId: 'AA_00001',
    name: 'AIDetector',
    configObject: { params: [{ key: 'fps', value: '5' }] },
  });

  const pkg = new ScenarioPackage(dir).load();
  assert.deepEqual(pkg.loadProfile.map((step) => step.holdSec), [30, 30]);
});

test('explicit target FPS overrides every detector node in the saved layout', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-bench-cv-fps-override-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));

  fs.writeFileSync(path.join(dir, 'scenario.yml'), `name: cv-fps-override
sampleIntervalSec: 5
channels:
  mode: local
  repeatCount: 0
  sources:
    - name: cv
      file: /device/cv.mp4
tasks:
  - id: cv
    displayName: CV
    type: cv
    algorithmId: "7463"
    scheduleId: schedule
    template: algorithm-template.json
    targetFps: 5
loadProfile:
  - channels: 1
    holdSec: 30
`, 'utf8');
  fs.writeFileSync(path.join(dir, 'algorithm-template.json'), JSON.stringify({
    algorithmId: '7463',
    algorithmCode: '7463',
    algorithmName: 'CV',
    taskConfig: { params: [], areas: [] },
    algorithmProcessdata: JSON.stringify([
      {
        actionId: 'AA_00001',
        configObject: { params: [{ key: 'fps', value: '7' }] },
      },
      {
        actionId: 'AA_00001',
        configObject: JSON.stringify({ params: [{ key: 'fps', value: '7' }] }),
      },
      {
        actionId: 'AA_00002',
        configObject: { params: [{ key: 'fps', value: '7' }] },
      },
    ]),
  }), 'utf8');

  const pkg = new ScenarioPackage(dir).load();
  const nodes = JSON.parse(pkg.layoutSavePayload.algorithmProcessdata);
  const fpsValues = nodes.map((node) => {
    const config = typeof node.configObject === 'string'
      ? JSON.parse(node.configObject)
      : node.configObject;
    return config.params.find((param) => param.key === 'fps')?.value;
  });

  assert.equal(pkg.targetFps, 5);
  assert.deepEqual(fpsValues, ['5', '5', '5']);
});

test('merges explicit per-task config params for soak-safe workloads', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-bench-task-config-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));

  fs.writeFileSync(path.join(dir, 'scenario.yml'), `name: cv-task-config
sampleIntervalSec: 5
channels:
  mode: local
  repeatCount: 0
  sources:
    - name: cv
      file: /device/cv.mp4
tasks:
  - id: cv
    displayName: CV
    type: cv
    algorithmId: "7463"
    scheduleId: schedule
    template: algorithm-template.json
    taskConfig:
      params:
        - key: param.sensitivity
          value: 1
        - key: param.detectionDuration
          value: 3600
      areas:
        - name: validation-area
loadProfile:
  - channels: 1
    holdSec: 30
`, 'utf8');
  fs.writeFileSync(path.join(dir, 'algorithm-template.json'), JSON.stringify({
    algorithmId: '7463',
    algorithmCode: '7463',
    algorithmName: 'CV',
    taskConfig: {
      params: [{ key: 'param.sensitivity', value: '5' }],
      areas: [{ name: 'template-area' }],
    },
    algorithmProcessdata: '[]',
  }), 'utf8');

  const pkg = new ScenarioPackage(dir).load();
  assert.deepEqual(pkg.taskConfig.params, [
    { key: 'param.sensitivity', value: '1' },
    { key: 'param.detectionDuration', value: '3600' },
    { key: 'param.videoRepeatCount', value: '0' },
  ]);
  assert.deepEqual(pkg.taskConfig.areas, [{ name: 'validation-area' }]);
});

test('rejects duplicate per-task config override keys', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-bench-task-config-duplicate-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));

  fs.writeFileSync(path.join(dir, 'scenario.yml'), `name: cv-task-config-duplicate
channels:
  mode: local
  sources:
    - name: cv
      file: /device/cv.mp4
tasks:
  - id: cv
    algorithmId: "7463"
    scheduleId: schedule
    template: algorithm-template.json
    taskConfig:
      params:
        - key: param.sensitivity
          value: 1
        - key: param.sensitivity
          value: 2
loadProfile:
  - channels: 1
    holdSec: 30
`, 'utf8');
  writeTemplate(path.join(dir, 'algorithm-template.json'), {
    actionId: 'AA_00001',
    configObject: { params: [{ key: 'fps', value: '5' }] },
  });

  assert.throws(
    () => new ScenarioPackage(dir).load(),
    /taskConfig\.params has duplicate key "param\.sensitivity"/,
  );
});

test('rk3588 v29 templates keep algorithm ids, names and flow ids consistent', () => {
  const dirs = [
    'person-detector-24fps',
    'no-safety-helmet-24fps',
    'concurrent-mixed-24fps',
    'longrun_dualcv_24h_8ch',
    'vlm-qwen35-0.1fps',
  ];
  for (const dir of dirs) {
    const pkg = new ScenarioPackage(path.join(root, 'scenarios/rk3588-v29-run', dir)).load();
    for (const task of pkg.tasks) {
      assert.ok(task.template.algorithmName, `${dir}:${task.id} must carry algorithmName`);
      assert.equal(String(task.template.algorithmId), String(task.algorithmId));
      assert.equal(String(task.template.algorithmCode), String(task.algorithmId));
    }
  }
});

test('no-safety-helmet template no longer keeps the person-detector version id', () => {
  const pkg = new ScenarioPackage(
    path.join(root, 'scenarios/rk3588-v29-run/no-safety-helmet-24fps'),
  ).load();
  const template = pkg.tasks[0].template;
  assert.equal(template.algorithmId, '7463002');

  const versionIds = template.configVersionList.map((version) => version.id);
  assert.ok(
    !versionIds.includes('default-7463'),
    `unexpected person-detector version id in ${versionIds.join(', ')}`,
  );
  assert.ok(versionIds.includes('default-7463002'));
  assert.ok(versionIds.includes(template.confVersionId));
});

test('layout save payload carries the algorithm name and config version name', () => {
  const pkg = new ScenarioPackage(
    path.join(root, 'scenarios/rk3588-v29-run/no-safety-helmet-24fps'),
  ).load();
  assert.equal(pkg.layoutSavePayload.algorithmName, 'No Safety Helmet');
  assert.equal(pkg.layoutSavePayload.configVersionName, '默认');
});

test('rejects a template that keeps another algorithm config version id', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-bench-template-version-id-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  writeScenario(dir, { algorithmId: '7463002' });
  writeJson(path.join(dir, 'algorithm-template.json'), {
    algorithmId: '7463002',
    algorithmCode: '7463002',
    algorithmName: 'No Safety Helmet',
    confVersionId: '79c9d9bdf07445048b57602e1dce6b6e',
    configVersionList: [
      { id: 'default-7463', name: '默认' },
      { id: '79c9d9bdf07445048b57602e1dce6b6e', name: '默认' },
    ],
    algorithmProcessdata: '[]',
  });

  assert.throws(() => new ScenarioPackage(dir).load(), /default-7463/);
});

test('rejects a template whose algorithmId disagrees with scenario.yml', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-bench-template-id-mismatch-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  writeScenario(dir, { algorithmId: '7463002' });
  writeJson(path.join(dir, 'algorithm-template.json'), {
    algorithmId: '7463',
    algorithmCode: '7463',
    algorithmName: 'Pedestrian Detector',
    algorithmProcessdata: '[]',
  });

  assert.throws(
    () => new ScenarioPackage(dir).load(),
    /does not match scenario\.yml algorithmId "7463002"/,
  );
});

test('rejects a template whose flow chain references an unknown step', (t) => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-bench-template-flow-'));
  t.after(() => fs.rmSync(dir, { recursive: true, force: true }));
  writeScenario(dir, { algorithmId: '7463' });
  writeJson(path.join(dir, 'algorithm-template.json'), {
    algorithmId: '7463',
    algorithmCode: '7463',
    algorithmName: 'Pedestrian Detector',
    algorithmProcessdata: JSON.stringify([
      { actionId: 'BA_00001', flowActionId: 'decode', preFlowActionId: '-1' },
      { actionId: 'AA_00001', flowActionId: 'detect', preFlowActionId: 'missing-step' },
    ]),
  });

  assert.throws(() => new ScenarioPackage(dir).load(), /preFlowActionId "missing-step"/);
});

function writeScenario(dir, { name = 'fixture', algorithmId, template = 'algorithm-template.json' }) {
  fs.writeFileSync(path.join(dir, 'scenario.yml'), `name: ${name}
sampleIntervalSec: 5
channels:
  mode: local
  repeatCount: 0
  sources:
    - name: cv
      file: /device/cv.mp4
tasks:
  - id: cv
    displayName: CV
    algorithmId: "${algorithmId}"
    scheduleId: schedule
    template: ${template}
loadProfile:
  - channels: 1
    holdSec: 30
`, 'utf8');
}

function writeJson(file, value) {
  fs.writeFileSync(file, JSON.stringify(value, null, 2), 'utf8');
}

function writeTemplate(file, processNode) {
  fs.writeFileSync(file, JSON.stringify({
    algorithmId: '55009',
    algorithmCode: '55009',
    algorithmName: 'Template',
    taskConfig: { params: [], areas: [] },
    algorithmProcessdata: JSON.stringify([processNode]),
  }), 'utf8');
}
