import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

const MIB = 1024 * 1024;

const REQUIRED_FAILURE_COUNTERS = [
  'graphForwardFailures',
  'mppCopyOutFailures',
  'mppRgaCopyOutFailures',
  'mppCpuCopyOutFallbacks',
  'mppRgaCopyInFailures',
  'mppCpuCopyInFallbacks',
  'mppDecodeFailures',
  'mppDecodeFallbacks',
  'mppEncodeFailures',
  'previewStreamFailures',
  'resultParseFailures',
  'rgaFailures',
  'rknnForwardFailures',
  'rknnBoundInputBindFailures',
  'rknnBoundInputCopyFailures',
  'rknnBoundInputSyncFailures',
  'rknnCpuResizeFallbackCalls',
  'rknnCpuCropResizeFallbackCalls',
  'rknnCpuNormalizeFallbackCalls',
  'rknnDetectorForwardFailures',
  'rknnInputCompatibilityFallbacks',
  'rknnMppDmaBufImportFailures',
  'rknnOutputCompatibilityFallbacks',
  'rknnRgaFailures',
  'rknnRgaCropResizeFailures',
  'rknnRgaCropHostFallbacks',
  'rknnRgaBoundInputBindFailures',
  'rknnRgaBoundInputImportFailures',
  'rknnRgaBoundRequantizeFailures',
  'rknnMppDmaBufFallbacks',
  'rknnYolov8DirectCandidateFailures',
];

const FAILURE_COUNTER_SUFFIX = /(Failures|Fallbacks|FallbackCalls)$/;

const NATIVE_BOUND_FORBIDDEN_COUNTERS = [
  'rknnBoundInputCopyCalls',
  'rknnBoundInputCopyBytes',
  'rknnNativeInputMapCalls',
  'rknnFloatInputs',
  'rknnUint8ContractInputs',
  'rknnInputsSetCalls',
  'rknnDetectorInputsSetCalls',
];

const MONOTONIC_COUNTERS = [
  'blobConvertFrames',
  'colorConvertFrames',
  'graphForwardFrames',
  'mppCopyOutFrames',
  'mppRgaCopyOutFrames',
  'mppRgaCopyInFrames',
  'mppDecodedFrames',
  'mppEarlyDroppedFrames',
  'mppEncodedFrames',
  'osdFrames',
  'publishedFrames',
  'resultParseFrames',
  'rgaFrames',
  'rknnForwards',
  'rknnRgaCropResizeCalls',
  'rknnRgaCropDmaBufFrames',
  'rknnRgaCropHostFallbacks',
  'rknnRgaBoundInputFrames',
  'rknnRgaBoundUint8Frames',
  'rknnRgaBoundNativeInt8Frames',
  'rknnRgaBoundRequantizeCalls',
  'rknnRgaBoundRequantizeMs',
  'rknnMppDmaBufFrames',
  'rknnBoundInputSyncCalls',
  'rknnMppDmaBufImportCalls',
];

const IDENTITY_ALLOWLIST = new Set([
  'engineSourceCommit',
  'harnessCommit',
  'sourceCommit',
  'sourceTree',
  'engineSha256',
  'modelSha256',
  'scenarioSha256',
  'videoSha256',
  'templateSha256',
  'startedAt',
  'runnerPid',
  'watcherPid',
  'liveMonitorPid',
  'scenario',
  'previewClients',
  'rknnRuntime',
  'rknnDriver',
  'kernel',
  'rga',
  'mpp',
]);

function round(value, digits = 3) {
  if (!Number.isFinite(value)) return null;
  const factor = 10 ** digits;
  return Math.round(value * factor) / factor;
}

function mean(values) {
  return values.length ? values.reduce((sum, value) => sum + value, 0) / values.length : null;
}

function percentile(values, fraction) {
  if (!values.length) return null;
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil(sorted.length * fraction) - 1))];
}

function numeric(values) {
  return values.filter((value) => typeof value === 'number' && Number.isFinite(value));
}

function counterSeries(samples, key) {
  return numeric(samples.map((sample) => sample.hardware?.accelerator?.[key]));
}

function counterSummary(samples, key) {
  const values = counterSeries(samples, key);
  return {
    samples: values.length,
    first: values[0] ?? null,
    last: values.at(-1) ?? null,
    min: values.length ? Math.min(...values) : null,
    max: values.length ? Math.max(...values) : null,
    nonZeroSamples: values.filter((value) => value !== 0).length,
    delta: values.length ? values.at(-1) - values[0] : null,
    decreases: values.slice(1).reduce(
      (count, value, index) => count + (value < values[index] ? 1 : 0),
      0,
    ),
  };
}

function resourceSummary(samples, key) {
  const values = numeric(samples.map((sample) => sample.hardware?.[key]?.usedPercent));
  return {
    samples: values.length,
    min: values.length ? Math.min(...values) : null,
    avg: round(mean(values), 2),
    p95: percentile(values, 0.95),
    max: values.length ? Math.max(...values) : null,
  };
}

function parseIdentity(raw) {
  const properties = {};
  for (const line of raw.split(/\r?\n/)) {
    const separator = line.indexOf('=');
    if (separator <= 0) continue;
    const key = line.slice(0, separator).trim();
    if (!IDENTITY_ALLOWLIST.has(key)) continue;
    properties[key] = line.slice(separator + 1).trim();
  }
  return properties;
}

function sha256(buffer) {
  return crypto.createHash('sha256').update(buffer).digest('hex');
}

function check(id, status, actual, expected, detail = null) {
  return { id, status, actual, expected, detail };
}

/**
 * Audit a running or completed ScenarioBench long-run result without mutating it.
 * A PASS is intentionally impossible until the requested wall-clock gate has
 * elapsed and every continuity, workload, resource, and native-media check passes.
 */
export function auditLongRun(runResult, options = {}) {
  if (!runResult || typeof runResult !== 'object' || Array.isArray(runResult)) {
    throw new Error('long-run input must be a JSON object');
  }

  const gateHours = Number(options.gateHours ?? 24);
  const maxGapSec = Number(options.maxGapSec ?? 120);
  const maxFreshnessSec = Number(options.maxFreshnessSec ?? maxGapSec);
  const minFpsRatio = Number(options.minFpsRatio ?? 0.9);
  const maxCpuPercent = Number(options.maxCpuPercent ?? 98);
  const maxMemoryPercent = Number(options.maxMemoryPercent ?? 98);
  const maxPoolGrowthBytes = Number(options.maxPoolGrowthBytes ?? 64 * MIB);
  const poolGrowthWarmupSec = Number(options.poolGrowthWarmupSec ?? 0);
  const allowRgaCropHostStaging = options.allowRgaCropHostStaging === true;
  const allowMppRgaMaterialization = options.allowMppRgaMaterialization === true;
  const minRgaBoundUint8Frames = options.minRgaBoundUint8Frames == null
    ? null
    : Number(options.minRgaBoundUint8Frames);
  const minRgaBoundNativeInt8Frames = options.minRgaBoundNativeInt8Frames == null
    ? null
    : Number(options.minRgaBoundNativeInt8Frames);
  const minRknnMppDmaBufFrames = options.minRknnMppDmaBufFrames == null
    ? null
    : Number(options.minRknnMppDmaBufFrames);
  const minRknnRgaCropDmaBufFrames = options.minRknnRgaCropDmaBufFrames == null
    ? null
    : Number(options.minRknnRgaCropDmaBufFrames);
  const maxRgaBoundRequantizeAvgMs = options.maxRgaBoundRequantizeAvgMs == null
    ? null
    : Number(options.maxRgaBoundRequantizeAvgMs);
  const nowMs = Number(options.nowMs ?? Date.now());
  const expectedPreviewStreams = options.expectedPreviewStreams == null
    ? null
    : Number(options.expectedPreviewStreams);

  for (const [name, value] of Object.entries({
    gateHours,
    maxGapSec,
    maxFreshnessSec,
    minFpsRatio,
    maxCpuPercent,
    maxMemoryPercent,
    maxPoolGrowthBytes,
    poolGrowthWarmupSec,
    nowMs,
  })) {
    if (!Number.isFinite(value) || value < 0) throw new Error(`${name} must be a non-negative number`);
  }
  if (gateHours <= 0) throw new Error('gateHours must be greater than zero');
  if (minFpsRatio <= 0) throw new Error('minFpsRatio must be greater than zero');
  if (expectedPreviewStreams != null
      && (!Number.isInteger(expectedPreviewStreams) || expectedPreviewStreams < 0)) {
    throw new Error('expectedPreviewStreams must be a non-negative integer');
  }
  if (minRgaBoundUint8Frames != null
      && (!Number.isInteger(minRgaBoundUint8Frames) || minRgaBoundUint8Frames < 0)) {
    throw new Error('minRgaBoundUint8Frames must be a non-negative integer');
  }
  for (const [name, value] of Object.entries({
    minRgaBoundNativeInt8Frames,
    minRknnMppDmaBufFrames,
    minRknnRgaCropDmaBufFrames,
  })) {
    if (value != null && (!Number.isInteger(value) || value < 0)) {
      throw new Error(`${name} must be a non-negative integer`);
    }
  }
  if (maxRgaBoundRequantizeAvgMs != null
      && (!Number.isFinite(maxRgaBoundRequantizeAvgMs) || maxRgaBoundRequantizeAvgMs < 0)) {
    throw new Error('maxRgaBoundRequantizeAvgMs must be a non-negative number');
  }

  const checks = [];
  const add = (...args) => checks.push(check(...args));
  const samples = Array.isArray(runResult.samples) ? runResult.samples : [];
  const holdSamples = samples.filter((sample) => sample?.phase !== 'ramp');
  const previews = holdSamples.map((sample) => sample.preview ?? {});
  const previewMode = runResult.previewProfile?.mode ?? previews.find((preview) => preview.mode)?.mode ?? 'none';
  const timestamps = holdSamples.map((sample) => Number(sample?.ts));
  const validTimestamps = timestamps.filter(Number.isFinite);
  const startedMs = Date.parse(runResult.startedAt);
  const reportedEndedMs = Date.parse(runResult.endedAt);
  const firstTs = validTimestamps[0] ?? null;
  const lastTs = validTimestamps.at(-1) ?? null;
  const gapsSec = validTimestamps.slice(1).map((value, index) => (value - validTimestamps[index]) / 1000);
  const nonIncreasingTimestamps = gapsSec.filter((gap) => gap <= 0).length;
  const maxObservedGapSec = gapsSec.length ? Math.max(...gapsSec) : null;
  const effectiveEndMs = runResult.status === 'completed' && Number.isFinite(reportedEndedMs)
    ? reportedEndedMs
    : lastTs;
  const runAgeSec = Number.isFinite(startedMs) && effectiveEndMs != null
    ? (effectiveEndMs - startedMs) / 1000
    : null;
  const fullLoadSamples = samples.filter((sample) => {
    const targetChannels = Number(sample?.targetChannels);
    const activeChannels = Number(sample?.activeChannels);
    const uniqueChannels = new Set((sample?.channels ?? []).map((channel) => channel.channelId));
    return Number.isFinite(Number(sample?.ts))
      && Number.isInteger(targetChannels)
      && targetChannels > 0
      && activeChannels === targetChannels
      && uniqueChannels.size === targetChannels;
  });
  const firstFullLoadTs = Number(fullLoadSamples[0]?.ts);
  const fullLoadCoverageSec = Number.isFinite(firstFullLoadTs)
    && effectiveEndMs != null
    && effectiveEndMs >= firstFullLoadTs
    ? (effectiveEndMs - firstFullLoadTs) / 1000
    : null;
  const sampleSpanSec = firstTs != null && lastTs != null ? (lastTs - firstTs) / 1000 : null;
  const firstSampleDelaySec = Number.isFinite(startedMs) && Number.isFinite(firstFullLoadTs)
    ? (firstFullLoadTs - startedMs) / 1000
    : null;
  const finalSampleDelaySec = runResult.status === 'completed'
    && Number.isFinite(reportedEndedMs)
    && lastTs != null
    ? (reportedEndedMs - lastTs) / 1000
    : null;
  const freshnessSec = lastTs == null ? null : (nowMs - lastTs) / 1000;
  const requiredDurationSec = gateHours * 3600;

  add(
    'run.status',
    ['running', 'completed'].includes(runResult.status) ? 'PASS' : 'FAIL',
    runResult.status ?? null,
    'running or completed',
    runResult.error?.message ?? null,
  );
  add(
    'samples.present',
    holdSamples.length > 1 ? 'PASS' : 'FAIL',
    holdSamples.length,
    '> 1 hold sample',
  );
  add(
    'samples.timestamps',
    validTimestamps.length === holdSamples.length && nonIncreasingTimestamps === 0 ? 'PASS' : 'FAIL',
    { valid: validTimestamps.length, hold: holdSamples.length, nonIncreasing: nonIncreasingTimestamps },
    'all hold timestamps valid and strictly increasing',
  );
  add(
    'samples.initialDelay',
    firstSampleDelaySec != null && firstSampleDelaySec >= 0 && firstSampleDelaySec <= maxGapSec ? 'PASS' : 'FAIL',
    round(firstSampleDelaySec),
    `0..${maxGapSec}s`,
  );
  add(
    'samples.maxGap',
    maxObservedGapSec != null && maxObservedGapSec <= maxGapSec ? 'PASS' : 'FAIL',
    round(maxObservedGapSec),
    `<= ${maxGapSec}s`,
  );

  if (runResult.status === 'running') {
    add(
      'samples.freshness',
      freshnessSec != null && freshnessSec >= 0 && freshnessSec <= maxFreshnessSec ? 'PASS' : 'FAIL',
      round(freshnessSec),
      `0..${maxFreshnessSec}s while running`,
    );
  } else {
    add('samples.freshness', 'N/A', round(freshnessSec), 'not required after completion');
    add(
      'samples.finalDelay',
      finalSampleDelaySec != null && finalSampleDelaySec >= 0 && finalSampleDelaySec <= maxGapSec ? 'PASS' : 'FAIL',
      round(finalSampleDelaySec),
      `0..${maxGapSec}s from final sample to completedAt`,
    );
  }

  const durationReached = fullLoadCoverageSec != null
    && fullLoadCoverageSec >= requiredDurationSec;
  add(
    'gate.duration',
    durationReached ? 'PASS' : (runResult.status === 'running' ? 'IN_PROGRESS' : 'FAIL'),
    round(fullLoadCoverageSec),
    `>= ${requiredDurationSec}s (${gateHours}h) of observed full-load coverage`,
  );

  const stepIndexes = [...new Set(holdSamples.map((sample) => sample.stepIndex))];
  const observedTargets = numeric(holdSamples.map((sample) => Number(sample.targetChannels)));
  const step = (runResult.steps ?? []).find((item) => item.index === stepIndexes[0]);
  const expectedChannels = Number(step?.channels ?? observedTargets[0]);
  const shapeFailures = holdSamples.reduce((count, sample) => {
    const uniqueChannels = new Set((sample.channels ?? []).map((channel) => channel.channelId));
    return count + (
      sample.activeChannels !== expectedChannels
      || sample.targetChannels !== expectedChannels
      || uniqueChannels.size !== expectedChannels
        ? 1
        : 0
    );
  }, 0);
  add(
    'workload.singleStep',
    stepIndexes.length === 1 ? 'PASS' : 'FAIL',
    stepIndexes,
    'exactly one hold step',
  );
  add(
    'workload.channelShape',
    Number.isInteger(expectedChannels) && expectedChannels > 0 && shapeFailures === 0 ? 'PASS' : 'FAIL',
    { expectedChannels: Number.isFinite(expectedChannels) ? expectedChannels : null, failedSamples: shapeFailures },
    'active, target, and unique observed channels stay at the configured count',
  );

  const bindings = new Map();
  let missingBindings = 0;
  for (const sample of holdSamples) {
    for (const channel of sample.channels ?? []) {
      const key = `${channel.taskKey ?? 'default'}::${channel.channelId ?? 'unknown'}`;
      if (!bindings.has(key)) {
        bindings.set(key, {
          taskKey: channel.taskKey ?? 'default',
          channelId: channel.channelId ?? null,
          targetFps: null,
          fps: [],
          discard: [],
          observations: 0,
          missing: 0,
        });
      }
      const binding = bindings.get(key);
      binding.observations++;
      if (channel.missing || channel.telemetryMissing) {
        binding.missing++;
        missingBindings++;
      }
      if (Number.isFinite(Number(channel.targetFps)) && Number(channel.targetFps) > 0) {
        binding.targetFps = Number(channel.targetFps);
      }
      if (typeof channel.measuredFps === 'number' && Number.isFinite(channel.measuredFps)) {
        binding.fps.push(channel.measuredFps);
      }
      if (typeof channel.discardRate === 'number' && Number.isFinite(channel.discardRate)) {
        binding.discard.push(channel.discardRate);
      }
    }
  }

  const bindingStats = [...bindings.values()].map((binding) => {
    const ratios = binding.targetFps
      ? binding.fps.map((fps) => fps / binding.targetFps)
      : [];
    return {
      taskKey: binding.taskKey,
      channelId: binding.channelId,
      observations: binding.observations,
      missing: binding.missing,
      targetFps: binding.targetFps,
      fpsMin: binding.fps.length ? round(Math.min(...binding.fps), 3) : null,
      fpsAvg: round(mean(binding.fps), 3),
      fpsP01: round(percentile(binding.fps, 0.01), 3),
      minFpsRatio: ratios.length ? round(Math.min(...ratios), 4) : null,
      discardAvg: round(mean(binding.discard), 6),
      discardMax: binding.discard.length ? round(Math.max(...binding.discard), 6) : null,
    };
  });
  const missingFpsTargets = bindingStats.filter((binding) => binding.minFpsRatio == null).length;
  const worstFpsRatio = bindingStats.length && !missingFpsTargets
    ? Math.min(...bindingStats.map((binding) => binding.minFpsRatio))
    : null;
  const maxMissingRate = bindingStats.length
    ? Math.max(...bindingStats.map((binding) => (
      binding.observations ? binding.missing / binding.observations : 1
    )))
    : null;
  const worstAverageDiscard = bindingStats.length
    ? Math.max(...bindingStats.map((binding) => binding.discardAvg ?? Number.POSITIVE_INFINITY))
    : null;
  const discardLimit = Number(
    runResult.thresholds?.pass?.avgDiscardRate
      ?? runResult.thresholds?.pass?.maxDiscardRate
      ?? 0.05,
  );

  add(
    'workload.telemetry',
    bindingStats.length > 0 && missingBindings === 0 ? 'PASS' : 'FAIL',
    { bindings: bindingStats.length, missingObservations: missingBindings, maxMissingRate: round(maxMissingRate, 6) },
    'no missing task/channel telemetry',
  );
  add(
    'workload.fps',
    worstFpsRatio != null && worstFpsRatio >= minFpsRatio ? 'PASS' : 'FAIL',
    { worstRatio: worstFpsRatio, missingTargets: missingFpsTargets },
    `every task/channel minimum FPS ratio >= ${minFpsRatio}`,
  );
  add(
    'workload.discard',
    Number.isFinite(worstAverageDiscard) && worstAverageDiscard <= discardLimit ? 'PASS' : 'FAIL',
    worstAverageDiscard,
    `worst task/channel average discard <= ${discardLimit}`,
  );

  const resources = {
    cpu: resourceSummary(holdSamples, 'cpuUtilization'),
    memory: resourceSummary(holdSamples, 'generalMemoryUtilization'),
    disk: resourceSummary(holdSamples, 'eMMCUtilization'),
  };
  const diskLimit = Number(runResult.thresholds?.pass?.maxDiskUsedPercent ?? 90);
  add('resource.cpu', resources.cpu.max != null && resources.cpu.max < maxCpuPercent ? 'PASS' : 'FAIL', resources.cpu.max, `< ${maxCpuPercent}%`);
  add('resource.memory', resources.memory.max != null && resources.memory.max < maxMemoryPercent ? 'PASS' : 'FAIL', resources.memory.max, `< ${maxMemoryPercent}%`);
  add('resource.disk', resources.disk.max != null && resources.disk.max <= diskLimit ? 'PASS' : 'FAIL', resources.disk.max, `<= ${diskLimit}%`);

  const poolMeasurementStartMs = firstTs == null
    ? null
    : firstTs + poolGrowthWarmupSec * 1000;
  const poolSamples = poolMeasurementStartMs == null
    ? []
    : holdSamples.filter((sample) => Number(sample?.ts) >= poolMeasurementStartMs);
  const coldPoolAllocated = numeric(holdSamples.map(
    (sample) => sample.hardware?.memoryPool?.totalAllocatedBytes,
  ));
  const poolAllocated = numeric(poolSamples.map(
    (sample) => sample.hardware?.memoryPool?.totalAllocatedBytes,
  ));
  const poolInUse = numeric(poolSamples.map(
    (sample) => sample.hardware?.memoryPool?.totalInUseBytes,
  ));
  const poolUtilization = numeric(poolSamples.map(
    (sample) => sample.hardware?.memoryPool?.utilizationPercent,
  ));
  const pool = {
    growthWarmupSec: poolGrowthWarmupSec,
    growthSamples: poolAllocated.length,
    growthBaselineAt: poolSamples[0]?.iso ?? null,
    coldStartAllocatedFirstBytes: coldPoolAllocated[0] ?? null,
    coldStartAllocatedLastBytes: coldPoolAllocated.at(-1) ?? null,
    coldStartAllocatedMaxBytes: coldPoolAllocated.length ? Math.max(...coldPoolAllocated) : null,
    coldStartAllocatedNetGrowthBytes: coldPoolAllocated.length
      ? coldPoolAllocated.at(-1) - coldPoolAllocated[0]
      : null,
    coldStartAllocatedPeakGrowthBytes: coldPoolAllocated.length
      ? Math.max(...coldPoolAllocated) - coldPoolAllocated[0]
      : null,
    allocatedFirstBytes: poolAllocated[0] ?? null,
    allocatedLastBytes: poolAllocated.at(-1) ?? null,
    allocatedMaxBytes: poolAllocated.length ? Math.max(...poolAllocated) : null,
    allocatedNetGrowthBytes: poolAllocated.length ? poolAllocated.at(-1) - poolAllocated[0] : null,
    allocatedPeakGrowthBytes: poolAllocated.length ? Math.max(...poolAllocated) - poolAllocated[0] : null,
    inUseMaxBytes: poolInUse.length ? Math.max(...poolInUse) : null,
    inUseLastBytes: poolInUse.at(-1) ?? null,
    utilizationMaxPercent: poolUtilization.length ? Math.max(...poolUtilization) : null,
  };
  add(
    'resource.poolGrowth',
    poolAllocated.length > 1
      && pool.allocatedNetGrowthBytes != null
      && pool.allocatedPeakGrowthBytes != null
      && pool.allocatedNetGrowthBytes <= maxPoolGrowthBytes
      && pool.allocatedPeakGrowthBytes <= maxPoolGrowthBytes
      ? 'PASS'
      : 'FAIL',
    {
      samples: poolAllocated.length,
      warmupSec: poolGrowthWarmupSec,
      netBytes: pool.allocatedNetGrowthBytes,
      peakBytes: pool.allocatedPeakGrowthBytes,
    },
    `after ${poolGrowthWarmupSec}s warm-up, net and peak allocated growth <= ${maxPoolGrowthBytes} bytes`,
  );

  const accelerators = holdSamples.map((sample) => sample.hardware?.accelerator ?? null);
  const acceleratorMissing = accelerators.filter((value) => !value).length;
  add(
    'native.telemetry',
    accelerators.length > 0 && acceleratorMissing === 0 ? 'PASS' : 'FAIL',
    { samples: accelerators.length, missing: acceleratorMissing },
    'accelerator telemetry present in every hold sample',
  );

  const dynamicFailureCounterKeys = [...new Set(accelerators.flatMap((accelerator) => (
    accelerator
      ? Object.keys(accelerator).filter((key) => FAILURE_COUNTER_SUFFIX.test(key))
      : []
  )))].sort();
  const allFailureCounterKeys = [...new Set([
    ...REQUIRED_FAILURE_COUNTERS,
    ...dynamicFailureCounterKeys,
  ])];
  const allowedRgaCropHostStagingCounters = allowRgaCropHostStaging
    ? new Set(['rknnRgaCropHostFallbacks'])
    : new Set();
  const failureCounterKeys = allFailureCounterKeys.filter(
    (key) => !allowedRgaCropHostStagingCounters.has(key),
  );
  const gatedForbiddenCounters = minRgaBoundNativeInt8Frames == null
    ? []
    : NATIVE_BOUND_FORBIDDEN_COUNTERS;
  const counterKeys = [...new Set([
    ...MONOTONIC_COUNTERS,
    ...failureCounterKeys,
    ...gatedForbiddenCounters,
  ])];
  const counters = Object.fromEntries(
    counterKeys.map((key) => [key, counterSummary(holdSamples, key)]),
  );
  const missingCounters = counterKeys.filter((key) => counters[key].samples !== holdSamples.length);
  const resetCounters = MONOTONIC_COUNTERS.filter((key) => counters[key].decreases > 0);
  const nonZeroFailures = failureCounterKeys.filter(
    (key) => counters[key].nonZeroSamples > 0,
  );
  add(
    'native.countersPresent',
    missingCounters.length === 0 ? 'PASS' : 'FAIL',
    missingCounters,
    'all explicit, dynamically discovered, and gated native-media counters present in every hold sample',
  );
  add(
    'native.counterContinuity',
    resetCounters.length === 0 ? 'PASS' : 'FAIL',
    resetCounters,
    'no monotonic counter decrease (engine/process restart signal)',
  );
  add(
    'native.failures',
    nonZeroFailures.length === 0 ? 'PASS' : 'FAIL',
    Object.fromEntries(nonZeroFailures.map((key) => [key, counters[key].max])),
    'every failure/fallback counter equals zero in every hold sample',
  );
  if (allowRgaCropHostStaging) {
    const calls = counters.rknnRgaCropResizeCalls?.delta;
    const dmaBufFrames = counters.rknnRgaCropDmaBufFrames?.delta;
    const hostStagingFrames = allFailureCounterKeys.includes('rknnRgaCropHostFallbacks')
      ? counters.rknnRgaCropHostFallbacks?.delta
      : null;
    const coverageError = [calls, dmaBufFrames, hostStagingFrames].every(Number.isFinite)
      ? Math.abs(calls - dmaBufFrames - hostStagingFrames)
      : null;
    const tolerance = Number.isFinite(calls) ? Math.max(2, Math.ceil(calls * 0.001)) : null;
    const cpuFallbacks = counters.rknnCpuCropResizeFallbackCalls?.delta;
    const rgaFailures = counters.rknnRgaCropResizeFailures?.delta;
    add(
      'native.rgaCropCoverage',
      coverageError != null
        && coverageError <= tolerance
        && cpuFallbacks === 0
        && rgaFailures === 0
        ? 'PASS'
        : 'FAIL',
      {
        calls,
        dmaBufFrames,
        hostStagingFrames,
        cpuFallbacks,
        rgaFailures,
        coverageError,
      },
      `RGA crop calls stay fully accounted by DMA-BUF or host staging (tolerance ${tolerance}), with zero CPU fallbacks and RGA failures`,
    );
  }
  if (minRgaBoundUint8Frames != null) {
    const fusedFrames = counters.rknnRgaBoundUint8Frames?.delta;
    add(
      'native.rgaBoundUint8',
      Number.isFinite(fusedFrames) && fusedFrames >= minRgaBoundUint8Frames ? 'PASS' : 'FAIL',
      fusedFrames ?? null,
      `>= ${minRgaBoundUint8Frames} fused UINT8 bound-input frames`,
    );
  }
  if (minRgaBoundNativeInt8Frames != null) {
    const missingForbiddenCounters = NATIVE_BOUND_FORBIDDEN_COUNTERS.filter(
      (key) => counters[key].samples !== holdSamples.length,
    );
    const activeForbiddenCounters = NATIVE_BOUND_FORBIDDEN_COUNTERS.filter(
      (key) => counters[key].nonZeroSamples > 0,
    );
    add(
      'native.legacyInputPaths',
      missingForbiddenCounters.length === 0 && activeForbiddenCounters.length === 0
        ? 'PASS'
        : 'FAIL',
      {
        missing: missingForbiddenCounters,
        nonZero: Object.fromEntries(
          activeForbiddenCounters.map((key) => [key, counters[key].max]),
        ),
      },
      'legacy host-copy, mapped-input, compatibility-input, and rknn_inputs_set paths stay present and zero in every hold sample',
    );
    const forwards = counters.rknnForwards?.delta;
    const boundFrames = counters.rknnRgaBoundInputFrames?.delta;
    const fusedFrames = counters.rknnRgaBoundUint8Frames?.delta;
    const nativeFrames = counters.rknnRgaBoundNativeInt8Frames?.delta;
    const requantizeCalls = counters.rknnRgaBoundRequantizeCalls?.delta;
    // Forward and bound-input counters are sampled from independent paths.
    // Under concurrent load a few frames can be visible in one counter before
    // the other has completed, so allow one bounded in-flight window while
    // still rejecting sustained accounting gaps.
    const accountingTolerance = Number.isFinite(forwards)
      ? Math.max(8, Math.ceil(forwards * 0.0001))
      : null;
    const forwardCoverageError = [forwards, boundFrames].every(Number.isFinite)
      ? Math.abs(forwards - boundFrames)
      : null;
    const nativeCoverageError = [boundFrames, nativeFrames].every(Number.isFinite)
      ? Math.abs(boundFrames - nativeFrames)
      : null;
    const requantizeCoverageError = [nativeFrames, requantizeCalls].every(Number.isFinite)
      ? Math.abs(nativeFrames - requantizeCalls)
      : null;
    add(
      'native.rgaBoundNativeInt8',
      Number.isFinite(nativeFrames)
        && nativeFrames >= minRgaBoundNativeInt8Frames
        && fusedFrames === 0
        && forwardCoverageError != null
        && forwardCoverageError <= accountingTolerance
        && nativeCoverageError != null
        && nativeCoverageError <= accountingTolerance
        && requantizeCoverageError != null
        && requantizeCoverageError <= accountingTolerance
        ? 'PASS'
        : 'FAIL',
      {
        forwards,
        boundFrames,
        fusedFrames,
        nativeFrames,
        requantizeCalls,
        forwardCoverageError,
        nativeCoverageError,
        requantizeCoverageError,
      },
      `>= ${minRgaBoundNativeInt8Frames} native INT8 frames, no fused UINT8 frames, and complete forward/bind/requantize accounting (tolerance ${accountingTolerance})`,
    );
  }
  if (minRknnMppDmaBufFrames != null && !allowMppRgaMaterialization) {
    const frames = counters.rknnMppDmaBufFrames?.delta;
    add(
      'native.rknnMppDmaBuf',
      Number.isFinite(frames) && frames >= minRknnMppDmaBufFrames ? 'PASS' : 'FAIL',
      frames ?? null,
      `>= ${minRknnMppDmaBufFrames} RKNN frames sourced from MPP DMA-BUF`,
    );
  }
  if (minRknnRgaCropDmaBufFrames != null && !allowMppRgaMaterialization) {
    const frames = counters.rknnRgaCropDmaBufFrames?.delta;
    add(
      'native.rknnRgaCropDmaBuf',
      Number.isFinite(frames) && frames >= minRknnRgaCropDmaBufFrames ? 'PASS' : 'FAIL',
      frames ?? null,
      `>= ${minRknnRgaCropDmaBufFrames} classifier crops sourced from DMA-BUF`,
    );
  }
  if (maxRgaBoundRequantizeAvgMs != null) {
    const calls = counters.rknnRgaBoundRequantizeCalls?.delta;
    const elapsedMs = counters.rknnRgaBoundRequantizeMs?.delta;
    const averageMs = Number.isFinite(calls) && calls > 0 && Number.isFinite(elapsedMs)
      ? elapsedMs / calls
      : null;
    add(
      'native.rgaBoundRequantizeLatency',
      averageMs != null && averageMs <= maxRgaBoundRequantizeAvgMs ? 'PASS' : 'FAIL',
      round(averageMs, 6),
      `<= ${maxRgaBoundRequantizeAvgMs} ms average native U8-to-INT8 sign-bit transform`,
    );
  }

  const backendValues = (key) => [...new Set(
    accelerators.map((value) => value?.[key]).filter(Boolean),
  )];
  const decoderBackends = backendValues('videoDecoderBackend');
  const encoderBackends = backendValues('videoEncoderBackend');
  if (options.expectedDecoderBackend) {
    add(
      'native.decoderBackend',
      decoderBackends.length === 1 && decoderBackends[0] === options.expectedDecoderBackend ? 'PASS' : 'FAIL',
      decoderBackends,
      options.expectedDecoderBackend,
    );
  } else {
    add('native.decoderBackend', 'N/A', decoderBackends, 'no expected backend supplied');
  }
  if (options.expectedEncoderBackend) {
    add(
      'native.encoderBackend',
      encoderBackends.length === 1 && encoderBackends[0] === options.expectedEncoderBackend ? 'PASS' : 'FAIL',
      encoderBackends,
      options.expectedEncoderBackend,
    );
  } else {
    add('native.encoderBackend', 'N/A', encoderBackends, 'no expected backend supplied');
  }

  const decoded = counters.mppDecodedFrames?.delta;
  const copied = counters.mppCopyOutFrames?.delta;
  const earlyDropped = counters.mppEarlyDroppedFrames?.delta;
  const copyAccountingError = [decoded, copied, earlyDropped].every(Number.isFinite)
    ? Math.abs(decoded - copied - earlyDropped)
    : null;
  const copyAccountingTolerance = Number.isFinite(decoded)
    ? Math.max(16, Math.ceil(decoded * 0.001))
    : null;
  add(
    'native.copyOutAccounting',
    copyAccountingError != null && copyAccountingError <= copyAccountingTolerance ? 'PASS' : 'FAIL',
    { decoded, copied, earlyDropped, error: copyAccountingError },
    `decoded ~= copied + earlyDropped (tolerance ${copyAccountingTolerance})`,
  );
  const rgaCopiedOut = counters.mppRgaCopyOutFrames?.delta;
  const cpuCopyOutFallbacks = counters.mppCpuCopyOutFallbacks?.delta;
  const rgaCopyOutError = [copied, rgaCopiedOut].every(Number.isFinite)
    ? Math.abs(copied - rgaCopiedOut)
    : null;
  add(
    'native.rgaCopyOut',
    rgaCopyOutError != null && rgaCopyOutError <= copyAccountingTolerance
      && cpuCopyOutFallbacks === 0 ? 'PASS' : 'FAIL',
    { copied, rgaCopiedOut, cpuCopyOutFallbacks, error: rgaCopyOutError },
    `all materialized frames use RGA (tolerance ${copyAccountingTolerance})`,
  );
  if (options.expectedDecoderBackend?.startsWith('rockchip-')
      && !allowMppRgaMaterialization
      && previewMode !== 'none') {
    add(
      'native.earlyDropActive',
      earlyDropped > 0 ? 'PASS' : 'FAIL',
      earlyDropped,
      '> 0 frames avoided copy-out',
    );
  }

  const encoded = counters.mppEncodedFrames?.delta;
  const rgaCopiedIn = counters.mppRgaCopyInFrames?.delta;
  const cpuCopyInFallbacks = counters.mppCpuCopyInFallbacks?.delta;
  const copyInAccountingTolerance = Number.isFinite(encoded)
    ? Math.max(8, Math.ceil(encoded * 0.001))
    : null;
  const copyInAccountingError = [encoded, rgaCopiedIn].every(Number.isFinite)
    ? Math.abs(encoded - rgaCopiedIn)
    : null;
  add(
    'native.rgaCopyIn',
    copyInAccountingError != null && copyInAccountingError <= copyInAccountingTolerance
      && cpuCopyInFallbacks === 0 ? 'PASS' : 'FAIL',
    { encoded, rgaCopiedIn, cpuCopyInFallbacks, error: copyInAccountingError },
    `all encoded frames use RGA copy-in (tolerance ${copyInAccountingTolerance})`,
  );
  const osd = counters.osdFrames?.delta;
  const published = counters.publishedFrames?.delta;
  const publishRatio = Number.isFinite(encoded) && encoded > 0 && Number.isFinite(published)
    ? published / encoded
    : null;
  const osdRatio = Number.isFinite(encoded) && encoded > 0 && Number.isFinite(osd)
    ? osd / encoded
    : null;
  if (previewMode === 'none') {
    add(
      'native.previewFrameFlow',
      'N/A',
      { encoded, osd, published, osdRatio: round(osdRatio, 6), publishRatio: round(publishRatio, 6) },
      'preview disabled',
    );
  } else {
    add(
      'native.previewFrameFlow',
      encoded > 0 && publishRatio >= 0.99 && publishRatio <= 1.01
        && osdRatio >= 0.99 && osdRatio <= 1.01
        ? 'PASS'
        : 'FAIL',
      { encoded, osd, published, osdRatio: round(osdRatio, 6), publishRatio: round(publishRatio, 6) },
      'OSD and published frame deltas stay within 1% of encoded frames',
    );
  }

  const previewErrors = previews.filter((preview) => (
    Array.isArray(preview.errors) && preview.errors.length > 0
  )).length;
  const requestedStreams = numeric(previews.map((preview) => preview.requestedStreams));
  const publishingStreams = numeric(previews.map((preview) => preview.srsPublishingStreams));
  const srsStreams = numeric(previews.map((preview) => preview.srsStreams));
  const inferredPreviewStreams = expectedPreviewStreams
    ?? (requestedStreams.length ? Math.max(...requestedStreams) : 0);
  if (previewMode === 'none' && inferredPreviewStreams === 0) {
    add('preview.health', 'N/A', { mode: previewMode }, 'preview disabled');
  } else {
    const badRequested = requestedStreams.filter((value) => value !== inferredPreviewStreams).length;
    const badPublishing = publishingStreams.filter((value) => value < inferredPreviewStreams).length;
    const badSrs = srsStreams.filter((value) => value < inferredPreviewStreams).length;
    add(
      'preview.health',
      previews.length === holdSamples.length
        && previewErrors === 0
        && requestedStreams.length === holdSamples.length
        && publishingStreams.length === holdSamples.length
        && srsStreams.length === holdSamples.length
        && badRequested === 0
        && badPublishing === 0
        && badSrs === 0
        ? 'PASS'
        : 'FAIL',
      {
        mode: previewMode,
        expectedStreams: inferredPreviewStreams,
        errorSamples: previewErrors,
        badRequestedSamples: badRequested,
        badPublishingSamples: badPublishing,
        badSrsSamples: badSrs,
      },
      'requested and SRS publishing streams remain healthy for every hold sample',
    );
  }

  const failedChecks = checks.filter((item) => item.status === 'FAIL');
  const pendingChecks = checks.filter((item) => item.status === 'IN_PROGRESS');
  const verdict = failedChecks.length ? 'FAIL' : (pendingChecks.length ? 'IN_PROGRESS' : 'PASS');

  return {
    schemaVersion: 1,
    kind: 'scenario-bench-longrun-checkpoint',
    auditedAt: new Date(nowMs).toISOString(),
    verdict,
    gate: {
      gateHours,
      requiredDurationSec,
      reached: durationReached,
      runAgeSec: round(runAgeSec),
      fullLoadCoverageSec: round(fullLoadCoverageSec),
      firstFullLoadAt: Number.isFinite(firstFullLoadTs)
        ? new Date(firstFullLoadTs).toISOString()
        : null,
      sampleSpanSec: round(sampleSpanSec),
      firstSampleDelaySec: round(firstSampleDelaySec),
      finalSampleDelaySec: round(finalSampleDelaySec),
      freshnessSec: round(freshnessSec),
    },
    run: {
      scenarioName: runResult.scenarioName ?? null,
      status: runResult.status ?? null,
      startedAt: runResult.startedAt ?? null,
      reportedEndedAt: runResult.endedAt ?? null,
      allSamples: samples.length,
      holdSamples: holdSamples.length,
      expectedChannels: Number.isFinite(expectedChannels) ? expectedChannels : null,
      stepIndexes,
      firstHoldAt: holdSamples[0]?.iso ?? (firstTs == null ? null : new Date(firstTs).toISOString()),
      lastHoldAt: holdSamples.at(-1)?.iso ?? (lastTs == null ? null : new Date(lastTs).toISOString()),
    },
    continuity: {
      medianGapSec: round(percentile(gapsSec, 0.5)),
      p95GapSec: round(percentile(gapsSec, 0.95)),
      maxGapSec: round(maxObservedGapSec),
      gapsOverLimit: gapsSec.filter((gap) => gap > maxGapSec).length,
      nonIncreasingTimestamps,
    },
    workload: {
      minFpsRatioGate: minFpsRatio,
      worstFpsRatio,
      discardLimit,
      worstAverageDiscard,
      maxMissingRate: round(maxMissingRate, 6),
      bindings: bindingStats,
    },
    resources,
    memoryPool: pool,
    nativeMedia: {
      decoderBackends,
      encoderBackends,
      failureCounterKeys,
      dynamicFailureCounterKeys,
      nativeBoundForbiddenCounters: gatedForbiddenCounters,
      counters,
      copyAccountingError,
      copyAccountingTolerance,
      frameFlow: { encoded, osd, published, osdRatio: round(osdRatio, 6), publishRatio: round(publishRatio, 6) },
    },
    preview: {
      mode: previewMode,
      expectedStreams: inferredPreviewStreams,
      samples: previews.length,
      errorSamples: previewErrors,
      minSrsStreams: srsStreams.length ? Math.min(...srsStreams) : null,
      minSrsPublishingStreams: publishingStreams.length ? Math.min(...publishingStreams) : null,
    },
    checks,
    failures: failedChecks.map((item) => item.id),
    pending: pendingChecks.map((item) => item.id),
  };
}

export function auditLongRunFile(inputPath, options = {}) {
  const absoluteInput = path.resolve(inputPath);
  const raw = fs.readFileSync(absoluteInput);
  const runResult = JSON.parse(raw.toString('utf8'));
  const result = auditLongRun(runResult, options);
  result.source = {
    path: options.sourceLabel ?? absoluteInput,
    localPath: absoluteInput,
    sizeBytes: raw.length,
    sha256: sha256(raw),
  };

  if (options.identityPath) {
    const identityPath = path.resolve(options.identityPath);
    const identityRaw = fs.readFileSync(identityPath);
    result.identity = {
      path: options.identityLabel ?? identityPath,
      localPath: identityPath,
      sizeBytes: identityRaw.length,
      sha256: sha256(identityRaw),
      properties: parseIdentity(identityRaw.toString('utf8')),
    };
  }
  return result;
}

export function writeLongRunAudit(outputPath, result) {
  const absoluteOutput = path.resolve(outputPath);
  fs.mkdirSync(path.dirname(absoluteOutput), { recursive: true });
  const temporaryPath = `${absoluteOutput}.tmp-${process.pid}`;
  fs.writeFileSync(temporaryPath, `${JSON.stringify(result, null, 2)}\n`, 'utf8');
  fs.renameSync(temporaryPath, absoluteOutput);
  return absoluteOutput;
}
