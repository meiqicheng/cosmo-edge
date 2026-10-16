import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';

import {
  auditLongRun,
  auditLongRunFile,
  writeLongRunAudit,
} from '../src/longrun-auditor.js';

const START = Date.parse('2026-08-04T00:00:00.000Z');

function accelerator(index) {
  const frames = 1000 + index * 100;
  return {
    blobConvertFrames: frames,
    colorConvertFrames: frames,
    graphForwardFrames: frames,
    graphForwardFailures: 0,
    mppCopyOutFailures: 0,
    mppCopyOutFrames: frames * 2,
    mppRgaCopyOutFailures: 0,
    mppRgaCopyOutFrames: frames * 2,
    mppCpuCopyOutFallbacks: 0,
    mppDecodeFailures: 0,
    mppDecodeFallbacks: 0,
    mppDecodedFrames: frames * 4,
    mppEarlyDroppedFrames: frames * 2,
    mppEncodeFailures: 0,
    mppEncodedFrames: frames * 3,
    mppRgaCopyInFailures: 0,
    mppRgaCopyInFrames: frames * 3,
    mppCpuCopyInFallbacks: 0,
    osdFrames: frames * 3,
    previewStreamFailures: 0,
    publishedFrames: frames * 3,
    resultParseFailures: 0,
    resultParseFrames: frames,
    rgaFailures: 0,
    rgaFrames: frames,
    rknnForwardFailures: 0,
    rknnBoundInputBindFailures: 0,
    rknnBoundInputCopyCalls: 0,
    rknnBoundInputCopyBytes: 0,
    rknnBoundInputCopyFailures: 0,
    rknnBoundInputSyncCalls: frames * 6,
    rknnBoundInputSyncFailures: 0,
    rknnRgaFailures: 0,
    rknnRgaCropResizeCalls: frames,
    rknnRgaCropResizeFailures: 0,
    rknnRgaCropDmaBufFrames: frames,
    rknnRgaCropHostFallbacks: 0,
    rknnCpuResizeFallbackCalls: 0,
    rknnCpuCropResizeFallbackCalls: 0,
    rknnCpuNormalizeFallbackCalls: 0,
    rknnDetectorForwardFailures: 0,
    rknnDetectorInputsSetCalls: 0,
    rknnFloatInputs: 0,
    rknnInputCompatibilityFallbacks: 0,
    rknnInputsSetCalls: 0,
    rknnNativeInputMapCalls: 0,
    rknnOutputCompatibilityFallbacks: 0,
    rknnRgaBoundInputFrames: frames * 3,
    rknnRgaBoundInputBindFailures: 0,
    rknnRgaBoundUint8Frames: frames * 3,
    rknnRgaBoundNativeInt8Frames: 0,
    rknnRgaBoundRequantizeCalls: 0,
    rknnRgaBoundRequantizeMs: 0,
    rknnRgaBoundInputImportFailures: 0,
    rknnRgaBoundRequantizeFailures: 0,
    rknnMppDmaBufFrames: frames,
    rknnMppDmaBufImportCalls: frames * 3,
    rknnMppDmaBufImportFailures: 0,
    rknnMppDmaBufFallbacks: 0,
    rknnUint8ContractInputs: 0,
    rknnYolov8DirectCandidateFailures: 0,
    rknnForwards: frames * 3,
    videoDecoderBackend: 'rockchip-mpp-rga',
    videoEncoderBackend: 'rockchip-mpp-rga',
  };
}

function sample(index, overrides = {}) {
  return {
    ts: START + index * 60_000,
    iso: new Date(START + index * 60_000).toISOString(),
    phase: 'hold',
    stepIndex: 0,
    activeChannels: 4,
    targetChannels: 4,
    channels: Array.from({ length: 4 }, (_, channelIndex) => ({
      taskKey: 'helmet',
      channelId: `ch${channelIndex + 1}`,
      targetFps: 5,
      measuredFps: 5.2,
      discardRate: 0,
      missing: false,
      telemetryMissing: false,
    })),
    hardware: {
      accelerator: accelerator(index),
      cpuUtilization: { usedPercent: 50 },
      generalMemoryUtilization: { usedPercent: 25 },
      eMMCUtilization: { usedPercent: 70 },
      memoryPool: {
        totalAllocatedBytes: 400 * 1024 * 1024 + index * 1024,
        totalInUseBytes: 30 * 1024 * 1024,
        utilizationPercent: 7.5,
      },
    },
    preview: {
      mode: 'algorithm',
      requestedStreams: 4,
      srsStreams: 4,
      srsPublishingStreams: 4,
      srsClients: 4,
      mediaClients: 0,
      errors: [],
    },
    ...overrides,
  };
}

function runResult(count = 5) {
  return {
    scenarioName: 'RK3576 4ch 5fps 12h',
    status: 'running',
    startedAt: new Date(START).toISOString(),
    endedAt: new Date(START + count * 60_000).toISOString(),
    previewProfile: { mode: 'algorithm' },
    thresholds: { pass: { avgDiscardRate: 0.05, maxDiskUsedPercent: 90 } },
    steps: [{ index: 0, channels: 4, holdSec: 259200 }],
    samples: [
      sample(0, { phase: 'ramp' }),
      ...Array.from({ length: count }, (_, index) => sample(index + 1)),
    ],
  };
}

function enableNativeInt8(input) {
  for (const [index, item] of input.samples.entries()) {
    const frames = 1000 + (index + 1) * 100;
    item.hardware.accelerator.rknnRgaBoundUint8Frames = 0;
    item.hardware.accelerator.rknnRgaBoundNativeInt8Frames = frames * 3;
    item.hardware.accelerator.rknnRgaBoundRequantizeCalls = frames * 3;
    item.hardware.accelerator.rknnRgaBoundRequantizeMs = frames * 3 * 0.8;
  }
  return input;
}

const options = {
  gateHours: 4 / 60,
  nowMs: START + 5 * 60_000 + 10_000,
  maxGapSec: 120,
  maxFreshnessSec: 120,
  minFpsRatio: 0.9,
  expectedPreviewStreams: 4,
  expectedDecoderBackend: 'rockchip-mpp-rga',
  expectedEncoderBackend: 'rockchip-mpp-rga',
  minRgaBoundUint8Frames: 1,
};

test('long-run audit passes only after duration and all native gates pass', () => {
  const result = auditLongRun(runResult(), options);
  assert.equal(result.verdict, 'PASS');
  assert.equal(result.gate.reached, true);
  assert.deepEqual(result.failures, []);
  assert.equal(result.nativeMedia.copyAccountingError, 0);
  assert.equal(result.workload.bindings.length, 4);
});

test('long-run audit reports IN_PROGRESS before the wall-clock gate', () => {
  const result = auditLongRun(runResult(), { ...options, gateHours: 1 });
  assert.equal(result.verdict, 'IN_PROGRESS');
  assert.deepEqual(result.pending, ['gate.duration']);
  assert.deepEqual(result.failures, []);
});

test('long-run audit skips preview frame-flow gating when preview is disabled', () => {
  const input = runResult();
  input.previewProfile = { mode: 'none' };
  for (const item of input.samples) {
    item.preview = { mode: 'none', requestedStreams: 0, errors: [] };
    item.hardware.accelerator.mppEarlyDroppedFrames = 0;
    item.hardware.accelerator.mppCopyOutFrames = item.hardware.accelerator.mppDecodedFrames;
    item.hardware.accelerator.mppRgaCopyOutFrames = item.hardware.accelerator.mppDecodedFrames;
    item.hardware.accelerator.mppEncodedFrames = 0;
    item.hardware.accelerator.mppRgaCopyInFrames = 0;
    item.hardware.accelerator.osdFrames = 0;
    item.hardware.accelerator.publishedFrames = 0;
  }
  input.samples.at(-1).preview.srsStreams = 0;
  input.samples.at(-1).preview.srsPublishingStreams = 0;

  const result = auditLongRun(input, {
    ...options,
    expectedPreviewStreams: 0,
  });
  assert.equal(result.verdict, 'PASS');
  assert.equal(result.checks.find((item) => item.id === 'native.previewFrameFlow')?.status, 'N/A');
  assert.equal(result.checks.some((item) => item.id === 'native.earlyDropActive'), false);
});

test('long-run audit accepts fully accounted RGA host staging without accepting CPU fallback', () => {
  const input = runResult();
  for (const item of input.samples) {
    const accelerator = item.hardware.accelerator;
    accelerator.rknnRgaCropDmaBufFrames = 0;
    accelerator.rknnRgaCropHostFallbacks = accelerator.rknnRgaCropResizeCalls;
  }

  const rejected = auditLongRun(input, options);
  assert.equal(rejected.verdict, 'FAIL');
  assert.ok(rejected.failures.includes('native.failures'));

  const accepted = auditLongRun(input, {
    ...options,
    allowRgaCropHostStaging: true,
  });
  assert.equal(accepted.verdict, 'PASS');
  assert.equal(accepted.checks.find((item) => item.id === 'native.rgaCropCoverage')?.status, 'PASS');

  input.samples.at(-1).hardware.accelerator.rknnCpuCropResizeFallbackCalls = 1;
  const cpuFallback = auditLongRun(input, {
    ...options,
    allowRgaCropHostStaging: true,
  });
  assert.equal(cpuFallback.verdict, 'FAIL');
  assert.ok(cpuFallback.failures.includes('native.failures'));
  assert.ok(cpuFallback.failures.includes('native.rgaCropCoverage'));
});

test('long-run audit does not count partial-load ramp time toward the duration gate', () => {
  const input = runResult();
  input.samples[0].activeChannels = 1;
  input.samples[0].channels = input.samples[0].channels.slice(0, 1);

  const running = auditLongRun(input, { ...options, gateHours: 4.5 / 60 });
  assert.equal(running.verdict, 'IN_PROGRESS');
  assert.equal(running.gate.runAgeSec, 300);
  assert.equal(running.gate.fullLoadCoverageSec, 240);
  assert.equal(running.gate.firstFullLoadAt, new Date(START + 60_000).toISOString());

  input.status = 'completed';
  input.endedAt = new Date(START + 5 * 60_000).toISOString();
  const completed = auditLongRun(input, { ...options, gateHours: 4.5 / 60 });
  assert.equal(completed.verdict, 'FAIL');
  assert.ok(completed.failures.includes('gate.duration'));
});

test('completed long-run uses completedAt with a bounded final sampling delay', () => {
  const input = runResult();
  input.status = 'completed';
  input.endedAt = new Date(START + 5.5 * 60_000).toISOString();

  const result = auditLongRun(input, { ...options, gateHours: 5.5 / 60 });
  assert.equal(result.verdict, 'PASS');
  assert.equal(result.gate.finalSampleDelaySec, 30);
  assert.equal(result.checks.find((item) => item.id === 'samples.finalDelay')?.status, 'PASS');
});

test('long-run audit fails sampling gaps, FPS regression, and counter reset', () => {
  const input = runResult();
  input.samples[2].ts += 180_000;
  input.samples[2].iso = new Date(input.samples[2].ts).toISOString();
  input.samples[3].channels[0].measuredFps = 4;
  input.samples[3].hardware.accelerator.rknnForwards = 1;

  const result = auditLongRun(input, options);
  assert.equal(result.verdict, 'FAIL');
  assert.ok(result.failures.includes('samples.timestamps'));
  assert.ok(result.failures.includes('samples.maxGap'));
  assert.ok(result.failures.includes('workload.fps'));
  assert.ok(result.failures.includes('native.counterContinuity'));
});

test('long-run audit fails native failure increments and unhealthy preview', () => {
  const input = runResult();
  input.samples.at(-1).hardware.accelerator.mppEncodeFailures = 1;
  input.samples.at(-1).preview.srsPublishingStreams = 3;

  const result = auditLongRun(input, options);
  assert.equal(result.verdict, 'FAIL');
  assert.ok(result.failures.includes('native.failures'));
  assert.ok(result.failures.includes('preview.health'));
});

test('long-run audit rejects a failure counter that is non-zero before sampling begins', () => {
  const input = runResult();
  for (const item of input.samples) {
    item.hardware.accelerator.mppCpuCopyOutFallbacks = 2;
  }

  const result = auditLongRun(input, options);
  assert.equal(result.verdict, 'FAIL');
  assert.ok(result.failures.includes('native.failures'));
  const nativeFailures = result.checks.find((item) => item.id === 'native.failures');
  assert.equal(nativeFailures.actual.mppCpuCopyOutFallbacks, 2);
});

test('long-run audit discovers new failure counters and requires every sample to expose them', () => {
  const input = runResult();
  input.samples[1].hardware.accelerator.experimentalHardwareFallbacks = 0;

  const result = auditLongRun(input, options);
  assert.equal(result.verdict, 'FAIL');
  assert.ok(result.failures.includes('native.countersPresent'));
  assert.ok(result.nativeMedia.dynamicFailureCounterKeys.includes('experimentalHardwareFallbacks'));
  const countersPresent = result.checks.find((item) => item.id === 'native.countersPresent');
  assert.ok(countersPresent.actual.includes('experimentalHardwareFallbacks'));
});

test('memory-pool warm-up keeps cold growth visible and gates the steady-state window', () => {
  const input = runResult(7);
  const base = 400 * 1024 * 1024;
  for (const [index, item] of input.samples.entries()) {
    item.hardware.memoryPool.totalAllocatedBytes = base + (index < 2 ? 0 : 32 * 1024 * 1024);
  }
  const auditOptions = {
    ...options,
    gateHours: 4 / 60,
    nowMs: START + 7 * 60_000 + 10_000,
    maxPoolGrowthBytes: 0,
  };

  const cold = auditLongRun(input, auditOptions);
  assert.ok(cold.failures.includes('resource.poolGrowth'));
  assert.equal(cold.memoryPool.coldStartAllocatedNetGrowthBytes, 32 * 1024 * 1024);

  const steady = auditLongRun(input, { ...auditOptions, poolGrowthWarmupSec: 120 });
  assert.equal(steady.verdict, 'PASS');
  assert.equal(steady.memoryPool.growthWarmupSec, 120);
  assert.equal(steady.memoryPool.growthSamples, 5);
  assert.equal(steady.memoryPool.allocatedNetGrowthBytes, 0);
  assert.equal(steady.memoryPool.allocatedPeakGrowthBytes, 0);
  assert.equal(steady.memoryPool.coldStartAllocatedNetGrowthBytes, 32 * 1024 * 1024);
});

test('long-run audit fails RGA bound-input import and requantize errors', () => {
  const input = runResult();
  input.samples.at(-1).hardware.accelerator.rknnRgaBoundInputImportFailures = 1;
  input.samples.at(-1).hardware.accelerator.rknnRgaBoundRequantizeFailures = 1;

  const result = auditLongRun(input, options);
  assert.equal(result.verdict, 'FAIL');
  assert.ok(result.failures.includes('native.failures'));
  const nativeFailures = result.checks.find((item) => item.id === 'native.failures');
  assert.equal(nativeFailures.actual.rknnRgaBoundInputImportFailures, 1);
  assert.equal(nativeFailures.actual.rknnRgaBoundRequantizeFailures, 1);
});

test('long-run audit fails when the fused UINT8 bound-input path is inactive', () => {
  const input = runResult();
  for (const item of input.samples) item.hardware.accelerator.rknnRgaBoundUint8Frames = 0;

  const result = auditLongRun(input, options);
  assert.equal(result.verdict, 'FAIL');
  assert.ok(result.failures.includes('native.rgaBoundUint8'));
});

test('long-run audit gates complete native INT8 DMA-BUF accounting and transform latency', () => {
  const input = enableNativeInt8(runResult());
  const nativeOptions = {
    ...options,
    minRgaBoundUint8Frames: undefined,
    minRgaBoundNativeInt8Frames: 1,
    minRknnMppDmaBufFrames: 1,
    minRknnRgaCropDmaBufFrames: 1,
    maxRgaBoundRequantizeAvgMs: 1,
  };

  const result = auditLongRun(input, nativeOptions);
  assert.equal(result.verdict, 'PASS');
  assert.equal(result.checks.find((item) => item.id === 'native.rgaBoundNativeInt8')?.status, 'PASS');
  assert.equal(result.checks.find((item) => item.id === 'native.rknnMppDmaBuf')?.status, 'PASS');
  assert.equal(result.checks.find((item) => item.id === 'native.rknnRgaCropDmaBuf')?.status, 'PASS');
  assert.equal(result.checks.find((item) => item.id === 'native.rgaBoundRequantizeLatency')?.actual, 0.8);
  assert.equal(result.checks.find((item) => item.id === 'native.legacyInputPaths')?.status, 'PASS');
});

test('long-run audit accepts the RK3588 RGA materialization path without MPP DMA-BUF', () => {
  const input = enableNativeInt8(runResult());
  for (const item of input.samples) {
    const accelerator = item.hardware.accelerator;
    accelerator.mppEarlyDroppedFrames = 0;
    accelerator.mppCopyOutFrames = accelerator.mppDecodedFrames;
    accelerator.mppRgaCopyOutFrames = accelerator.mppDecodedFrames;
    accelerator.rknnMppDmaBufFrames = 0;
    accelerator.rknnMppDmaBufImportCalls = 0;
    accelerator.rknnRgaCropDmaBufFrames = 0;
    accelerator.rknnRgaCropHostFallbacks = accelerator.rknnRgaCropResizeCalls;
  }

  const result = auditLongRun(input, {
    ...options,
    minRgaBoundUint8Frames: undefined,
    minRgaBoundNativeInt8Frames: 1,
    minRknnMppDmaBufFrames: 1,
    minRknnRgaCropDmaBufFrames: 1,
    maxRgaBoundRequantizeAvgMs: 1,
    allowRgaCropHostStaging: true,
    allowMppRgaMaterialization: true,
  });
  assert.equal(result.verdict, 'PASS');
  assert.equal(result.checks.some((item) => item.id === 'native.rknnMppDmaBuf'), false);
  assert.equal(result.checks.some((item) => item.id === 'native.rknnRgaCropDmaBuf'), false);
  assert.equal(result.checks.some((item) => item.id === 'native.earlyDropActive'), false);
  assert.equal(result.checks.find((item) => item.id === 'native.rgaCropCoverage')?.status, 'PASS');
});

test('native INT8 audit rejects legacy host-copy and rknn_inputs_set activity', () => {
  const input = enableNativeInt8(runResult());
  input.samples[2].hardware.accelerator.rknnBoundInputCopyCalls = 1;
  input.samples[2].hardware.accelerator.rknnBoundInputCopyBytes = 1_228_800;
  input.samples[2].hardware.accelerator.rknnInputsSetCalls = 1;

  const result = auditLongRun(input, {
    ...options,
    minRgaBoundUint8Frames: undefined,
    minRgaBoundNativeInt8Frames: 1,
  });
  assert.equal(result.verdict, 'FAIL');
  assert.ok(result.failures.includes('native.legacyInputPaths'));
  const legacyPaths = result.checks.find((item) => item.id === 'native.legacyInputPaths');
  assert.equal(legacyPaths.actual.nonZero.rknnBoundInputCopyCalls, 1);
  assert.equal(legacyPaths.actual.nonZero.rknnBoundInputCopyBytes, 1_228_800);
  assert.equal(legacyPaths.actual.nonZero.rknnInputsSetCalls, 1);
});

test('native INT8 audit permits positive DMA-BUF import and cache-sync counters', () => {
  const input = enableNativeInt8(runResult());
  assert.ok(input.samples.at(-1).hardware.accelerator.rknnMppDmaBufImportCalls > 0);
  assert.ok(input.samples.at(-1).hardware.accelerator.rknnBoundInputSyncCalls > 0);

  const result = auditLongRun(input, {
    ...options,
    minRgaBoundUint8Frames: undefined,
    minRgaBoundNativeInt8Frames: 1,
  });
  assert.equal(result.verdict, 'PASS');
  assert.equal(result.checks.find((item) => item.id === 'native.legacyInputPaths')?.status, 'PASS');
});

test('long-run audit rejects uncovered RKNN forwards and slow native transform', () => {
  const input = runResult();
  for (const [index, item] of input.samples.entries()) {
    const frames = 1000 + (index + 1) * 100;
    item.hardware.accelerator.rknnRgaBoundUint8Frames = 0;
    item.hardware.accelerator.rknnRgaBoundNativeInt8Frames = frames * 2;
    item.hardware.accelerator.rknnRgaBoundRequantizeCalls = frames * 2;
    item.hardware.accelerator.rknnRgaBoundRequantizeMs = frames * 2 * 1.5;
  }

  const result = auditLongRun(input, {
    ...options,
    minRgaBoundUint8Frames: undefined,
    minRgaBoundNativeInt8Frames: 1,
    maxRgaBoundRequantizeAvgMs: 1,
  });
  assert.equal(result.verdict, 'FAIL');
  assert.ok(result.failures.includes('native.rgaBoundNativeInt8'));
  assert.ok(result.failures.includes('native.rgaBoundRequantizeLatency'));
});

test('native INT8 audit tolerates bounded in-flight accounting at the sampling edge', () => {
  const input = enableNativeInt8(runResult());
  for (const item of input.samples) {
    item.hardware.accelerator.rknnRgaBoundInputFrames -= 4;
    item.hardware.accelerator.rknnRgaBoundNativeInt8Frames -= 4;
    item.hardware.accelerator.rknnRgaBoundRequantizeCalls -= 4;
  }

  const result = auditLongRun(input, {
    ...options,
    minRgaBoundUint8Frames: undefined,
    minRgaBoundNativeInt8Frames: 1,
    maxRgaBoundRequantizeAvgMs: 1,
  });
  assert.equal(result.verdict, 'PASS');
  assert.equal(result.checks.find((item) => item.id === 'native.rgaBoundNativeInt8')?.status, 'PASS');
});

test('file audit binds source and allowlisted candidate identity by SHA-256', () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'scenario-longrun-audit-'));
  try {
    const input = path.join(dir, 'metrics.partial.json');
    const identity = path.join(dir, 'identity.txt');
    const output = path.join(dir, 'checkpoint.json');
    fs.writeFileSync(input, JSON.stringify(runResult()), 'utf8');
    fs.writeFileSync(identity, [
      'engineSourceCommit=88e556a1',
      'sourceCommit=99f667b2',
      'modelSha256=def456',
      'rknnRuntime=2.3.2-429f97ae6b',
      'engineSha256=abc123',
      'password=must-not-be-copied',
    ].join('\n'), 'utf8');

    const result = auditLongRunFile(input, { ...options, identityPath: identity });
    const written = writeLongRunAudit(output, result);
    const saved = JSON.parse(fs.readFileSync(written, 'utf8'));

    assert.match(saved.source.sha256, /^[a-f0-9]{64}$/);
    assert.match(saved.identity.sha256, /^[a-f0-9]{64}$/);
    assert.equal(saved.identity.properties.engineSourceCommit, '88e556a1');
    assert.equal(saved.identity.properties.sourceCommit, '99f667b2');
    assert.equal(saved.identity.properties.modelSha256, 'def456');
    assert.equal(saved.identity.properties.rknnRuntime, '2.3.2-429f97ae6b');
    assert.equal(saved.identity.properties.password, undefined);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
});
