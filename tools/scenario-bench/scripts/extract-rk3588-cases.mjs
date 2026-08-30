// Extract rk3588 capacity results into cases.json format.
// Reuses the same summarizeStep evaluator that produced report.html,
// so per-step values match the generated reports exactly.
import { readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { join } from 'node:path';
import { summarizeStep } from '../src/step-evaluator.js';

const RESULTS_ROOT = 'results/rk3588-full-retest';
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

// Case-level metadata (lastPass/firstBlocked/outcome/boundaryKind/blockedReason)
// is derived from each scenario's summary.json, which is the authoritative
// conclusion of the retest run. Only the directory/workload/fps identity is
// listed here.
const SCENARIOS = [
  { dir: 'person-detector-24fps', workload: 'person-detector', targetFps: 24 },
  { dir: 'person-detector-15fps', workload: 'person-detector', targetFps: 15 },
  { dir: 'person-detector-10fps', workload: 'person-detector', targetFps: 10 },
  { dir: 'person-detector-7fps', workload: 'person-detector', targetFps: 7 },
  { dir: 'person-detector-5fps', workload: 'person-detector', targetFps: 5 },
  { dir: 'no-safety-helmet-24fps', workload: 'no-safety-helmet-analysis', targetFps: 24 },
  { dir: 'no-safety-helmet-15fps', workload: 'no-safety-helmet-analysis', targetFps: 15 },
  { dir: 'no-safety-helmet-10fps', workload: 'no-safety-helmet-analysis', targetFps: 10 },
  { dir: 'no-safety-helmet-7fps', workload: 'no-safety-helmet-analysis', targetFps: 7 },
  { dir: 'no-safety-helmet-5fps', workload: 'no-safety-helmet-analysis', targetFps: 5 },
  { dir: 'concurrent-mixed-24fps-32ch-fixed', workload: 'concurrent-mixed', targetFps: 24 },
  { dir: 'concurrent-mixed-15fps-32ch-fixed', workload: 'concurrent-mixed', targetFps: 15 },
  { dir: 'concurrent-mixed-10fps-32ch-fixed', workload: 'concurrent-mixed', targetFps: 10 },
  { dir: 'concurrent-mixed-7fps-32ch-fixed', workload: 'concurrent-mixed', targetFps: 7 },
  { dir: 'concurrent-mixed-5fps-32ch-fixed', workload: 'concurrent-mixed', targetFps: 5 },
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

function deriveCaseMeta(summary) {
  const lastPass = summary.maxStableChannels ?? summary.maxVerifiedPassedChannels;
  const firstBlocked = summary.firstFailedStep?.channels ?? summary.bottleneck?.channels ?? null;
  let outcome;
  let boundaryKind;
  let blockedReason;
  if (summary.capacityMeasured) {
    outcome = 'stopped';
    boundaryKind = 'performance-stop';
    blockedReason = summary.bottleneck?.reason ?? null;
  } else if (summary.capacityExecutionBlocked) {
    // The run was stopped by the runtime protection fuse before a capacity
    // boundary was measured. The retest report does not count this as a
    // capacity failure and no capacity limit is formed.
    outcome = 'stopped';
    boundaryKind = 'performance-stop';
    blockedReason = `execution blocked: ${summary.bottleneck?.reason ?? 'run stopped before capacity measured'} (not counted as capacity failure)`;
  } else {
    outcome = 'stopped';
    boundaryKind = 'performance-stop';
    blockedReason = summary.bottleneck?.reason ?? null;
  }
  return { lastPass, firstBlocked, outcome, boundaryKind, blockedReason };
}

function buildCase(scenario) {
  const dir = join(RESULTS_ROOT, scenario.dir);
  const metricsBuf = readFileSync(join(dir, 'metrics.json'));
  const metrics = JSON.parse(metricsBuf.toString('utf8'));
  const summary = JSON.parse(readFileSync(join(dir, 'summary.json'), 'utf8'));
  const sourceSummarySha256 = sha256(metricsBuf);
  const meta = deriveCaseMeta(summary);

  const steps = [];
  for (const step of metrics.steps) {
    const channels = step.channels;
    const isBlockedStep = meta.boundaryKind === 'performance-stop' && channels === meta.firstBlocked;
    if (channels > meta.lastPass && !isBlockedStep) continue; // never executed / not part of a binding-blocked case
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
      failureReason: result === 'PASS' ? null : meta.blockedReason,
    });

    if (result === 'STOP') {
      console.log(`[${scenario.dir}] step ${channels} STOP: pass=${summary.pass} reasons=${JSON.stringify(summary.reasons)}`);
    }
  }

  const caseId = `${WORKLOAD_PREFIX[scenario.workload]}-${scenario.targetFps}fps-32ch`;
  const sourceCaseId = `${SOURCE_PREFIX[scenario.workload]}-${scenario.targetFps}fps-32ch`;

  return {
    caseId,
    sourceCaseId,
    workload: scenario.workload,
    targetFps: scenario.targetFps,
    configuredChannels: 32,
    outcome: meta.outcome,
    boundaryKind: meta.boundaryKind,
    lastPassingChannels: meta.lastPass,
    firstBlockedChannels: meta.firstBlocked,
    blockedReason: meta.blockedReason,
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
  evidenceDate: '2026-08-30',
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