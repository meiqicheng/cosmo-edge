// Extract rk3588 capacity results into cases.json format.
// Reuses the same summarizeStep evaluator that produced report.html,
// so per-step values match the generated reports exactly.
import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { summarizeStep } from '../src/step-evaluator.js';

const RESULTS_ROOT = 'results/rk3588-capacity';
const OUT = '../../docs/benchmarks/scenario-bench/v1.1/results/rk3588/cases.json';

const THRESHOLDS = {
  pass: {
    maxCriticalPathLatencyMs: 200,
    maxDetectorLatencyMs: 150,
    avgDiscardRate: 0.05,
    maxPacketDiscardRate: 0.01,
    maxDiskUsedPercent: 90,
  },
  taskTypes: {
    cv: { minFpsRatio: 0.8, maxMissingRate: 0 },
  },
};

const TASK_NAME_MAP = {
  'person-detector': 'person-detector',
  'no-safety-helmet': 'no-safety-helmet-analysis',
};

const SCENARIOS = [
  { dir: 'person-detector-24fps', workload: 'person-detector', targetFps: 24, outcome: 'setup-blocked', boundaryKind: 'binding-blocked', lastPass: 5, firstBlocked: 6, blockedReason: 'task binding failed (device concurrent authorization limit reached)' },
  { dir: 'person-detector-10fps', workload: 'person-detector', targetFps: 10, outcome: 'stopped', boundaryKind: 'performance-stop', lastPass: 6, firstBlocked: 7, blockedReason: 'average discard-rate protection threshold reached' },
  { dir: 'person-detector-7fps', workload: 'person-detector', targetFps: 7, outcome: 'stopped', boundaryKind: 'performance-stop', lastPass: 7, firstBlocked: 8, blockedReason: 'average discard-rate protection threshold reached' },
  { dir: 'person-detector-5fps', workload: 'person-detector', targetFps: 5, outcome: 'stopped', boundaryKind: 'performance-stop', lastPass: 7, firstBlocked: 8, blockedReason: 'average discard-rate protection threshold reached' },
  { dir: 'no-safety-helmet-24fps', workload: 'no-safety-helmet-analysis', targetFps: 24, outcome: 'stopped', boundaryKind: 'performance-stop', lastPass: 3, firstBlocked: 4, blockedReason: 'processing-FPS gate failed (79.2%)' },
  { dir: 'no-safety-helmet-10fps', workload: 'no-safety-helmet-analysis', targetFps: 10, outcome: 'stopped', boundaryKind: 'performance-stop', lastPass: 5, firstBlocked: 6, blockedReason: 'average discard-rate protection threshold reached' },
  { dir: 'no-safety-helmet-7fps', workload: 'no-safety-helmet-analysis', targetFps: 7, outcome: 'stopped', boundaryKind: 'performance-stop', lastPass: 6, firstBlocked: 7, blockedReason: 'average discard-rate protection threshold reached' },
  { dir: 'no-safety-helmet-5fps', workload: 'no-safety-helmet-analysis', targetFps: 5, outcome: 'stopped', boundaryKind: 'performance-stop', lastPass: 6, firstBlocked: 7, blockedReason: 'average discard-rate protection threshold reached' },
  { dir: 'concurrent-mixed-5fps', workload: 'concurrent-mixed', targetFps: 5, outcome: 'stopped', boundaryKind: 'performance-stop', lastPass: 6, firstBlocked: 7, blockedReason: 'average discard-rate protection threshold reached' },
];

const WORKLOAD_PREFIX = {
  'person-detector': 'person',
  'no-safety-helmet-analysis': 'nohelmet',
  'concurrent-mixed': 'mixed-cv',
};
const SOURCE_PREFIX = {
  'person-detector': 'person',
  'no-safety-helmet-analysis': 'nohelmet',
  'concurrent-mixed': 'dual-cv',
};

function sha256(buf) {
  return createHash('sha256').update(buf).digest('hex');
}

function round(v, digits) {
  if (v == null) return null;
  const f = 10 ** digits;
  return Math.round(v * f) / f;
}

function buildCase(scenario) {
  const dir = join(RESULTS_ROOT, scenario.dir);
  const metricsBuf = readFileSync(join(dir, 'metrics.json'));
  const metrics = JSON.parse(metricsBuf.toString('utf8'));
  const sourceSummarySha256 = sha256(metricsBuf);

  const steps = [];
  for (const step of metrics.steps) {
    const channels = step.channels;
    const isBlockedStep = scenario.boundaryKind === 'performance-stop' && channels === scenario.firstBlocked;
    if (channels > scenario.lastPass && !isBlockedStep) continue; // never executed / not part of a binding-blocked case
    const summary = summarizeStep(step, metrics.samples, THRESHOLDS, 'local', 0);

    const result = isBlockedStep ? 'STOP' : 'PASS';

    const tasks = summary.taskStats.map((t) => ({
      name: TASK_NAME_MAP[t.taskKey] ?? t.taskKey,
      targetFps: t.targetFps,
      minimumProcessingFps: t.minThroughputFps,
      minimumFpsRatio: t.minFpsRatio,
      missingRate: t.maxMissingRate,
      averageDiscardRate: t.avgDiscardRate,
      maximumDetectorLatencyMs: t.maxDetectorLatencyMs,
      maximumCriticalPathLatencyMs: t.maxCriticalPathLatencyMs,
    }));

    const minFps = Math.min(...tasks.map((t) => t.minimumProcessingFps ?? Infinity));
    const maxDet = Math.max(...tasks.map((t) => t.maximumDetectorLatencyMs ?? 0));
    const maxCrit = Math.max(...tasks.map((t) => t.maximumCriticalPathLatencyMs ?? 0));

    steps.push({
      channels,
      holdSeconds: step.holdSec,
      result,
      tasks,
      minimumProcessingFps: Number.isFinite(minFps) ? round(minFps, 2) : null,
      maximumDetectorLatencyMs: maxDet > 0 ? round(maxDet, 1) : null,
      maximumCriticalPathLatencyMs: maxCrit > 0 ? round(maxCrit, 1) : null,
      averageDiscardRate: summary.avgDiscard != null ? round(summary.avgDiscard, 4) : null,
      maximumChannelDiscardRate: summary.maxDiscard != null ? round(summary.maxDiscard, 4) : null,
      acceleratorPeakPercent: summary.maxNpu != null ? round(summary.maxNpu, 1) : null,
      acceleratorMemoryPeakPercent: summary.maxAcceleratorMem != null ? round(summary.maxAcceleratorMem, 1) : null,
      cpuPeakPercent: summary.maxCpu != null ? round(summary.maxCpu, 1) : null,
      memoryPeakPercent: summary.maxMem != null ? round(summary.maxMem, 1) : null,
      failureReason: result === 'PASS' ? null : scenario.blockedReason,
    });

    if (result === 'STOP') {
      console.log(`[${scenario.dir}] step ${channels} STOP: pass=${summary.pass} reasons=${JSON.stringify(summary.reasons)}`);
    }
  }

  const caseId = `${WORKLOAD_PREFIX[scenario.workload]}-${scenario.targetFps}fps-16ch`;
  const sourceCaseId = `${SOURCE_PREFIX[scenario.workload]}-${scenario.targetFps}fps-16ch`;

  return {
    caseId,
    sourceCaseId,
    workload: scenario.workload,
    targetFps: scenario.targetFps,
    configuredChannels: 16,
    outcome: scenario.outcome,
    boundaryKind: scenario.boundaryKind,
    lastPassingChannels: scenario.lastPass,
    firstBlockedChannels: scenario.firstBlocked,
    blockedReason: scenario.blockedReason,
    sourceSummarySha256,
    steps,
  };
}

const cases = SCENARIOS.map(buildCase);

const casesJson = {
  $schema: '../cases.schema.json',
  schemaVersion: 3,
  benchmark: 'CosmoEdge 1.1 small-model capacity benchmark',
  platformId: 'rk3588',
  platform: 'RK3588',
  scope: 'additional-experimental-platform',
  evidenceDate: '2026-08-29',
  caseCount: cases.length,
  gates: {
    minFpsRatio: 0.8,
    maxMissingRate: 0,
    maxAverageDiscardRate: 0.05,
    maxCriticalPathLatencyMs: 200,
    maxDetectorLatencyMs: 150,
  },
  cases,
};

writeFileSync(OUT, JSON.stringify(casesJson, null, 2) + '\n', 'utf8');
console.log(`Wrote ${OUT} with ${cases.length} cases`);
for (const c of cases) {
  const passSteps = c.steps.filter((s) => s.result === 'PASS').length;
  const stopSteps = c.steps.filter((s) => s.result === 'STOP').length;
  console.log(`  ${c.caseId}: ${c.outcome}/${c.boundaryKind} lastPass=${c.lastPassingChannels} firstBlocked=${c.firstBlockedChannels} steps=${passSteps}P+${stopSteps}S sha=${c.sourceSummarySha256.slice(0, 12)}`);
}