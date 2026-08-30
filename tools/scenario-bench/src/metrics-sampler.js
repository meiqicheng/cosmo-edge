// metrics-sampler.js — Collect RunningDetail + HardwareResource each tick.
import {
  isPrimaryThroughputAction,
  isThroughputBearingAction,
  normalizeTaskType,
} from './task-strategies.js';
//
// Response shapes (verified against source DTOs):
//   RunningDetail: resData.status[] where each item has
//     { taskId, channelId, actionStatus[], nodeDurationInfos[] }
//     actionStatus[]: { actionId, name, statusCode, holdCount, alarmCount,
//       insertCount, processCount, discardCount, periodMs,
//       insertCountPeriod, processCountPeriod, discardCountPeriod }
//   HardwareResource: resData.itemList[] where each item is
//     { key, name, usedPercent, usedSize, unusedSize, available }
//     key in { cpuUtilization, generalMemoryUtilization, npuUtilization,
//       modelMemoryUtilization, pictureMemoryUtilization, TPPMemoryUtilization,
//       specialMemoryUtilization, eMMCUtilization, packetDiscardUtilization }
//
// NOTE: RunningDetail silently drops tasks whose action count <= 2. We detect
// such "sampling-missing" channels by diffing expected taskIds vs returned ones.

const HW_KEYS = [
  'cpuUtilization',
  'generalMemoryUtilization',
  'npuUtilization',
  'modelMemoryUtilization',
  'pictureMemoryUtilization',
  'TPPMemoryUtilization',
  'specialMemoryUtilization',
  'eMMCUtilization',
  'packetDiscardUtilization',
];

export class MetricsSampler {
  /**
   * @param {import('./cosmo-client.js').CosmoClient} client
   * @param {import('./logger.js').Logger} [logger]
   */
  constructor(client, logger) {
    this.client = client;
    this.log = logger;
    this.counterSnapshots = new Map();
  }

  /**
   * Take one sample for the currently active task/channel bindings.
   * @param {Array<object>|Map<string,string>} expectedBindings active task bindings.
   *   The array form is [{ taskKey, taskId, channelId, targetFps, ... }].
   *   A legacy Map channelId -> taskId is still accepted.
   * @param {number} [legacyTargetFps] orchestration fps baseline for legacy callers
   * @returns {Promise<object>} a sample record
   */
  async sample(expectedBindings, legacyTargetFps = null) {
    const ts = Date.now();
    const expected = normalizeExpectedBindings(expectedBindings, legacyTargetFps);
    const activeChannelIds = [...new Set(expected.map((entry) => entry.channelId))];
    const activeTaskIds = [...new Set(expected.map((entry) => entry.taskId))];

    // Sample all endpoints in parallel; each may fail independently.
    const [taskDetail, hwRes, memoryPoolRes] = await Promise.allSettled([
      activeTaskIds.length ? this.client.taskRunningDetail(activeTaskIds) : Promise.resolve({ status: [] }),
      this.client.queryHardwareResource(),
      typeof this.client.queryDeviceMemoryPool === 'function'
        ? this.client.queryDeviceMemoryPool()
        : Promise.resolve(null),
    ]);

    const perBinding = this._parseRunningDetail(taskDetail, expected, ts);
    const hw = this._parseHardware(hwRes);
    const memoryPool = this._parseMemoryPool(memoryPoolRes);
    if (memoryPool) hw.memoryPool = memoryPool;

    return {
      ts,
      iso: new Date(ts).toISOString(),
      activeChannels: activeChannelIds.length,
      activeTaskBindings: expected.length,
      channels: perBinding,
      hardware: hw,
    };
  }

  // ── RunningDetail parsing ──────────────────────────────────────────────

  _parseRunningDetail(detailResult, expectedBindings, ts) {
    const out = [];
    if (detailResult.status !== 'fulfilled') {
      // Whole RunningDetail call failed - mark every expected binding as errored.
      for (const entry of expectedBindings) {
        out.push({ ...entry, error: String(detailResult.reason?.message ?? detailResult.reason), missing: true });
      }
      return out;
    }

    const statusList = detailResult.value?.status ?? [];
    const returnedByTaskId = new Map(statusList.map((s) => [s.taskId, s]));

    for (const entry of expectedBindings) {
      const st = returnedByTaskId.get(entry.taskId);
      if (!st) {
        // Not returned -> either action count <= 2 (silent filter) or task not started yet.
        out.push({ ...entry, missing: true });
        continue;
      }
      out.push({
        ...entry,
        ...this._summarizeTask(
          st,
          entry.targetFps,
          entry.taskType,
          ts,
          entry.vlmCompletionActionId,
        ),
        taskKey: entry.taskKey,
        taskDisplayName: entry.taskDisplayName,
        taskType: entry.taskType,
        algorithmId: entry.algorithmId,
        algorithmCode: entry.algorithmCode,
      });
    }
    return out;
  }

  /**
   * Aggregate a task's actions into per-channel throughput.
   * FPS uses the slowest effective action rate instead of summing pipeline nodes,
   * otherwise multi-node graphs overcount the channel throughput.
   */
  _summarizeTask(st, targetFps, taskType, ts, expectedVlmCompletionActionId = null) {
    const actions = Array.isArray(st.actionStatus) ? st.actionStatus : [];
    const isVlm = normalizeTaskType(taskType) === 'vlm';
    let insertPeriod = 0, processPeriod = 0, discardPeriod = 0;
    let periodMs = 0, holdCount = 0, alarmCount = 0;
    let insertTotal = 0, processTotal = 0, discardTotal = 0;
    let pipelineMinFps = Infinity;
    let primaryFps = null;
    let primaryAction = null;
    let primaryProcessTotal = null;
    let maxDiscardRate = 0;
    const actionSummaries = [];

    for (const a of actions) {
      const actionInsertPeriod = num(a.insertCountPeriod);
      const actionProcessPeriod = num(a.processCountPeriod);
      const actionDiscardPeriod = num(a.discardCountPeriod);
      const actionPeriodMs = num(a.periodMs);

      insertPeriod += actionInsertPeriod;
      processPeriod += actionProcessPeriod;
      discardPeriod += actionDiscardPeriod;
      periodMs = Math.max(periodMs, actionPeriodMs);  // pick the longest window
      holdCount += num(a.holdCount);
      alarmCount += num(a.alarmCount);
      insertTotal += num(a.insertCount);
      processTotal += num(a.processCount);
      discardTotal += num(a.discardCount);

      const actionFps = actionPeriodMs > 0 ? (actionProcessPeriod * 1000) / actionPeriodMs : null;
      const actionName = String(a.name ?? '');
      const actionId = String(a.actionId ?? '');
      const primaryThroughputAction = isPrimaryThroughputAction(actionName, actionId, taskType);
      const throughputBearingAction = isThroughputBearingAction(actionName, actionId, taskType);
      if (primaryAction == null && primaryThroughputAction) {
        primaryAction = a;
      }
      actionSummaries.push({
        actionId,
        name: actionName,
        fps: actionFps != null ? round(actionFps, 2) : null,
        processTotal: num(a.processCount),
        insertPeriod: actionInsertPeriod,
        processPeriod: actionProcessPeriod,
        discardPeriod: actionDiscardPeriod,
        periodMs: actionPeriodMs,
        statusCode: a.statusCode != null ? String(a.statusCode) : null,
        statusDesc: a.statusDesc ?? null,
        statusDescKey: a.statusDescKey ?? null,
      });

      if (actionFps != null && actionProcessPeriod > 0 && throughputBearingAction) {
        pipelineMinFps = Math.min(pipelineMinFps, actionFps);
        if (primaryFps == null && primaryThroughputAction) {
          primaryFps = actionFps;
        }
      }
      const actionDiscardDen = actionInsertPeriod + actionDiscardPeriod;
      if (actionDiscardDen > 0) {
        maxDiscardRate = Math.max(maxDiscardRate, actionDiscardPeriod / actionDiscardDen);
      }
    }

    // Per-action error/exception state (e.g. "帧数据异常" = api.error.FrameDataInvalid).
    // statusCode '0' is ErrorEnum::Success; any other value is an error condition.
    const actionErrors = actionSummaries
      .filter((s) => s.statusCode != null && s.statusCode !== '0')
      .map((s) => ({ actionId: s.actionId, name: s.name, statusCode: s.statusCode, statusDesc: s.statusDesc, statusDescKey: s.statusDescKey }));

    // Qwen workers may batch and share up to several task bindings, so their
    // DA_00003 processCount is a worker-batch counter rather than a per-task
    // completion counter. In a direct VLM graph every completed AlgData is
    // dispatched once to the task-local BA_00004 queue. Prefer that counter
    // when present, while keeping DA_00003 as the inference/latency signal.
    const expectedCompletionActionId = isVlm
      ? normalizeActionId(expectedVlmCompletionActionId)
      : null;
    const completionAction = isVlm
      ? (expectedCompletionActionId
        ? actions.find((a) => normalizeActionId(a.actionId) === expectedCompletionActionId)
        : (actions.find((a) => normalizeActionId(a.actionId) === 'BA_00004') ?? primaryAction))
      : primaryAction;
    const strictCompletionCounterMissing = isVlm && expectedCompletionActionId != null
      && (!completionAction || finiteNumber(completionAction.processCount) == null);
    if (isVlm && completionAction && !strictCompletionCounterMissing) {
      primaryProcessTotal = expectedCompletionActionId
        ? finiteNumber(completionAction.processCount)
        : num(completionAction.processCount);
      const deltaFps = this._counterFps(
        `${st.taskId}:${normalizeActionId(completionAction.actionId)
          ?? completionAction.name ?? 'vlm'}`,
        primaryProcessTotal,
        ts,
      );
      const completionPeriodMs = num(completionAction.periodMs);
      const completionPeriodFps = completionPeriodMs > 0
        ? (num(completionAction.processCountPeriod) * 1000) / completionPeriodMs
        : null;
      primaryFps = deltaFps ?? completionPeriodFps ?? primaryFps;
    } else if (isVlm && expectedCompletionActionId) {
      // Never cross counter domains. In particular, a DA+BA graph must not
      // silently substitute the shared DA worker counter when BA disappears.
      primaryFps = null;
      primaryProcessTotal = null;
    }
    if (primaryFps == null && !isVlm) {
      const firstEffective = actionSummaries.find((a) => a.fps != null && a.processPeriod > 0);
      primaryFps = firstEffective?.fps ?? null;
    }
    const minPipelineFps = pipelineMinFps !== Infinity ? pipelineMinFps : 0;
    // A detector instance may be shared by several channels. Its AA_00001 counter is then the
    // aggregate rate and is repeated in every task detail, while downstream per-channel actions
    // retain the actual channel rate. CV throughput is therefore the slowest effective pipeline
    // frame-throughput action. Terminal event/report actions run only when a
    // business event fires and are intentionally excluded. Direct VLM keeps
    // its dedicated completion-counter semantics.
    const measuredFps = isVlm ? primaryFps : minPipelineFps;
    const discardRate = maxDiscardRate;
    const fpsRatio = targetFps && targetFps > 0 && measuredFps != null ? measuredFps / targetFps : null;

    return {
      channelId: st.channelId,
      taskId: st.taskId,
      algorithmName: st.algorithmName,
      algorithmVersion: st.algorithmVersion,
      actionCount: actions.length,
      measuredFps: measuredFps != null ? round(measuredFps, 2) : null,
      throughputFps: measuredFps != null ? round(measuredFps, 2) : null,
      telemetryMissing: isVlm && (primaryAction == null || strictCompletionCounterMissing),
      primaryProcessTotal,
      completionActionId: completionAction && !strictCompletionCounterMissing
        ? normalizeActionId(completionAction.actionId)
        : null,
      expectedCompletionActionId,
      pipelineMinFps: round(minPipelineFps, 2),
      targetFps,
      fpsRatio: fpsRatio != null ? round(fpsRatio, 3) : null,
      discardRate: round(discardRate, 4),
      insertPeriod, processPeriod, discardPeriod, periodMs,
      insertTotal, processTotal, discardTotal,
      holdCount, alarmCount,
      actionSummaries,
      hasError: actionErrors.length > 0,
      errorActions: actionErrors,
      nodeDurationInfos: Array.isArray(st.nodeDurationInfos) ? st.nodeDurationInfos : [],
    };
  }

  _counterFps(key, count, ts) {
    const previous = this.counterSnapshots.get(key);
    this.counterSnapshots.set(key, { count, ts });
    if (!previous || ts <= previous.ts || count < previous.count) return null;
    return (count - previous.count) * 1000 / (ts - previous.ts);
  }

  // ── HardwareResource parsing ───────────────────────────────────────────

  _parseHardware(hwResult) {
    const hw = {};
    if (hwResult.status !== 'fulfilled') {
      hw._error = String(hwResult.reason?.message ?? hwResult.reason);
      return hw;
    }
    const itemList = hwResult.value?.itemList ?? [];
    const byKey = new Map(itemList.map((it) => [it.key, it]));
    for (const key of HW_KEYS) {
      const it = byKey.get(key);
      if (it && it.available !== 0) {
        hw[key] = { usedPercent: num(it.usedPercent), usedSize: it.usedSize, unusedSize: it.unusedSize };
      }
    }
    hw.customScore = hwResult.value?.customScore ?? null;
    hw.accelerator = normalizeAccelerator(hwResult.value?.accelerator);
    return hw;
  }

  _parseMemoryPool(poolResult) {
    if (poolResult.status !== 'fulfilled') {
      return { _error: String(poolResult.reason?.message ?? poolResult.reason) };
    }
    if (!poolResult.value || typeof poolResult.value !== 'object') return null;

    const totalAllocatedBytes = num(poolResult.value.totalMalloc);
    const totalInUseBytes = num(poolResult.value.totalInUsing);
    return {
      totalAllocatedBytes,
      totalInUseBytes,
      utilizationPercent: totalAllocatedBytes > 0
        ? round((totalInUseBytes / totalAllocatedBytes) * 100, 2)
        : 0,
      pools: Array.isArray(poolResult.value.status)
        ? poolResult.value.status.map((pool) => ({
            blockSize: num(pool.poolSize),
            usedBlocks: num(pool.mallocCnt),
            freeBlocks: num(pool.freeCnt),
          }))
        : [],
    };
  }
}

function normalizeAccelerator(value) {
  if (!value || typeof value !== 'object') return null;
  return Object.fromEntries(Object.entries(value).map(([key, item]) => {
    if (typeof item === 'number') return [key, Number.isFinite(item) ? item : null];
    return [key, item];
  }));
}

function num(v) {
  const n = Number(v);
  return Number.isFinite(n) ? n : 0;
}

function finiteNumber(value) {
  if (value === null || value === undefined || value === '') return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function normalizeActionId(value) {
  const normalized = String(value ?? '').trim().toUpperCase();
  return normalized || null;
}

function round(v, digits) {
  const f = 10 ** digits;
  return Math.round(v * f) / f;
}

function normalizeExpectedBindings(expectedBindings, legacyTargetFps) {
  if (expectedBindings instanceof Map) {
    return [...expectedBindings.entries()].map(([channelId, taskId]) => ({
      taskKey: 'default',
      taskDisplayName: 'default',
      taskType: 'cv',
      algorithmId: null,
      algorithmCode: null,
      targetFps: legacyTargetFps,
      channelId,
      taskId,
    }));
  }

  if (!Array.isArray(expectedBindings)) return [];
  return expectedBindings.map((entry) => ({
    taskKey: entry.taskKey ?? entry.taskId,
    taskDisplayName: entry.taskDisplayName ?? entry.taskKey ?? entry.taskId,
    taskType: entry.taskType ?? 'cv',
    vlmCompletionActionId: normalizeActionId(entry.vlmCompletionActionId),
    algorithmId: entry.algorithmId ?? null,
    algorithmCode: entry.algorithmCode ?? null,
    targetFps: entry.targetFps ?? null,
    channelId: entry.channelId,
    taskId: entry.taskId,
  }));
}
