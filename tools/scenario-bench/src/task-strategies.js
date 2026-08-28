const DEFAULT_TYPE = 'cv';

// These actions emit business events after frame processing has completed.
// Their process rate follows the alarm/event rate rather than the video-frame
// rate, so they must not be used as a CV throughput floor.
const CV_TERMINAL_EVENT_ACTION_IDS = new Set([
  'BA_00004', // event report / task alarm
  'BA_10004', // face-feature report / face alarm
  'BA_90001', // target judgment: event-driven, runs at alarm/event rate not frame rate
]);

const CV_TERMINAL_EVENT_NAMES = [
  'eventreport',
  'event report',
  'taskalarm',
  'facealarm',
  '事件上报',
  '人脸特征上报',
  '目标判断',
  '灵敏度计算',
];

const TASK_TYPE_ALIASES = new Map([
  ['cv', 'cv'],
  ['detect', 'cv'],
  ['detection', 'cv'],
  ['classifier', 'cv'],
  ['classification', 'cv'],
  ['vlm', 'vlm'],
  ['vision-language', 'vlm'],
  ['vision_language', 'vlm'],
  ['multimodal', 'vlm'],
  ['llm-vlm', 'vlm'],
]);

const STRATEGIES = {
  cv: {
    id: 'cv',
    displayName: 'CV',
    throughputLabel: '检测FPS',
    primaryLatencyLabel: '检测节点延时ms',
    criticalLatencyLabel: '关键链路延时ms',
    defaultThresholds: {},
    primaryThroughputNames: ['aidetector', 'detector', '检测'],
    primaryLatencyNames: ['aidetector', 'detector', '检测'],
    criticalLatencyNames: [
      'decode',
      'aidetector',
      'detector',
      '检测',
      '追踪',
      'track',
      '分类',
      'classif',
      '目标判断',
      'judge',
    ],
    primaryActionIds: [/^1001/],
    useAllNodesForCriticalFallback: false,
    useBaselineFpsFuse: true,
  },
  vlm: {
    id: 'vlm',
    displayName: 'VLM',
    throughputLabel: '分析FPS',
    primaryLatencyLabel: '分析节点延时ms',
    criticalLatencyLabel: '端到端链路延时ms',
    defaultThresholds: {
      minFpsRatio: 0.8,
      maxMissingRate: 0,
    },
    primaryThroughputNames: [
      'vlm',
      'llm',
      'qwen',
      'vision',
      'language',
      'multimodal',
      'analysis',
      'analyze',
      'infer',
      'inference',
      'model',
      '大模型',
      '视觉语言',
      '分析',
      '推理',
    ],
    primaryLatencyNames: [
      'vlm',
      'llm',
      'qwen',
      'vision',
      'language',
      'multimodal',
      'analysis',
      'analyze',
      'infer',
      'inference',
      'model',
      '大模型',
      '视觉语言',
      '分析',
      '推理',
    ],
    criticalLatencyNames: [
      'decode',
      'vlm',
      'llm',
      'qwen',
      'vision',
      'language',
      'multimodal',
      'analysis',
      'analyze',
      'infer',
      'inference',
      'model',
      '大模型',
      '视觉语言',
      '分析',
      '推理',
    ],
    primaryActionIds: [/^(?:P?DA_00003)$/],
    useAllNodesForCriticalFallback: true,
    useBaselineFpsFuse: false,
  },
};

export function normalizeTaskType(type) {
  const raw = String(type ?? DEFAULT_TYPE).trim().toLowerCase();
  return TASK_TYPE_ALIASES.get(raw) ?? raw;
}

export function strategyForTaskType(type) {
  return STRATEGIES[normalizeTaskType(type)] ?? STRATEGIES[DEFAULT_TYPE];
}

export function strategyForTask(task) {
  return strategyForTaskType(task?.taskType ?? task?.type);
}

export function resolveTaskThresholds(thresholds = {}, task = {}) {
  const strategy = strategyForTask(task);
  const taskType = normalizeTaskType(task.taskType ?? task.type);
  const pass = basePassThresholds(thresholds.pass ?? {}, strategy);
  const typeRules = thresholds.taskTypes?.[taskType]
    ?? thresholds.strategies?.[taskType]
    ?? thresholds[taskType]
    ?? {};
  const taskKey = task.taskKey ?? task.id;
  const taskRules = taskKey ? (thresholds.tasks?.[taskKey] ?? {}) : {};
  return {
    ...strategy.defaultThresholds,
    ...pass,
    ...typeRules,
    ...taskRules,
  };
}

function basePassThresholds(pass, strategy) {
  if (strategy.id !== 'vlm') return pass;
  const portable = { ...pass };
  delete portable.maxDetectorLatencyMs;
  delete portable.maxCriticalPathLatencyMs;
  delete portable.maxAvgNodeLatencyMs;
  return portable;
}

export function isPrimaryThroughputAction(name, actionId, taskType) {
  const strategy = strategyForTaskType(taskType);
  const normalizedName = String(name ?? '').toLowerCase();
  const normalizedActionId = String(actionId ?? '');
  return strategy.primaryThroughputNames.some((pattern) => normalizedName.includes(pattern.toLowerCase()))
    || strategy.primaryActionIds.some((pattern) => pattern.test(normalizedActionId));
}

export function isThroughputBearingAction(name, actionId, taskType) {
  if (normalizeTaskType(taskType) !== 'cv') return true;

  const normalizedActionId = String(actionId ?? '').trim().toUpperCase();
  if (CV_TERMINAL_EVENT_ACTION_IDS.has(normalizedActionId)) return false;

  const normalizedName = String(name ?? '').trim().toLowerCase();
  return !CV_TERMINAL_EVENT_NAMES.some((pattern) => normalizedName.includes(pattern));
}

export function latencyMetricsForNodes(nodes, taskType) {
  const strategy = strategyForTaskType(taskType);
  const normalizedNodes = (Array.isArray(nodes) ? nodes : [])
    .map((node) => ({
      name: nodeName(node),
      latencyMs: nodeLatencyMs(node),
    }))
    .filter((node) => node.latencyMs != null);

  const primary = normalizedNodes
    .filter((node) => matchesAny(node.name, strategy.primaryLatencyNames))
    .map((node) => node.latencyMs);
  const critical = normalizedNodes
    .filter((node) => matchesAny(node.name, strategy.criticalLatencyNames))
    .map((node) => node.latencyMs);
  const criticalFallback = strategy.useAllNodesForCriticalFallback && !critical.length
    ? normalizedNodes.map((node) => node.latencyMs)
    : [];

  return {
    primaryLatencyMs: primary.length ? Math.max(...primary) : null,
    criticalPathLatencyMs: critical.length || criticalFallback.length
      ? [...critical, ...criticalFallback].reduce((sum, value) => sum + value, 0)
      : null,
  };
}

export function evaluateTaskStat(taskStat, thresholds = {}) {
  const strategy = strategyForTask(taskStat);
  const rules = resolveTaskThresholds(thresholds, taskStat);
  const checks = [];
  const failures = [];

  const check = (name, actual, op, limit) => {
    if (actual == null || limit == null) {
      checks.push(checkRecord(taskStat, name, limit, null, 'N/A'));
      return;
    }
    const ok = op === '>=' ? actual >= limit : actual <= limit;
    checks.push(checkRecord(taskStat, name, limit, actual, ok ? 'PASS' : 'FAIL'));
    if (!ok) failures.push(formatThresholdFailure(name, actual, limit, strategy));
  };

  if (rules.minThroughputFps != null) {
    check('minThroughputFps', taskStat.minThroughputFps, '>=', rules.minThroughputFps);
  }
  if (rules.minFpsRatio != null) {
    check('minFpsRatio', taskStat.minFpsRatio, '>=', rules.minFpsRatio);
  }
  if (rules.maxMissingRate != null) {
    check('maxMissingRate', taskStat.maxMissingRate, '<=', rules.maxMissingRate);
  }
  if (rules.avgDiscardRate != null || rules.maxDiscardRate != null) {
    check('avgDiscardRate', taskStat.avgDiscardRate, '<=', rules.avgDiscardRate ?? rules.maxDiscardRate);
  }
  if (rules.maxPrimaryLatencyMs != null || rules.maxAnalysisLatencyMs != null) {
    check(
      'maxPrimaryLatencyMs',
      taskStat.maxPrimaryLatencyMs,
      '<=',
      rules.maxPrimaryLatencyMs ?? rules.maxAnalysisLatencyMs,
    );
  }

  if (strategy.id === 'cv') {
    check('maxCriticalPathLatencyMs', taskStat.maxCriticalPathLatencyMs, '<=', rules.maxCriticalPathLatencyMs ?? rules.maxAvgNodeLatencyMs);
    check('maxDetectorLatencyMs', taskStat.maxPrimaryLatencyMs, '<=', rules.maxDetectorLatencyMs);
  } else {
    check('maxEndToEndLatencyMs', taskStat.maxCriticalPathLatencyMs, '<=', rules.maxEndToEndLatencyMs ?? rules.maxCriticalPathLatencyMs);
  }

  return {
    checks,
    pass: failures.length === 0,
    reasons: failures.map((reason) => `${taskStat.taskDisplayName ?? taskStat.taskKey}: ${reason}`),
  };
}

export function formatThresholdFailure(name, actual, limit, strategy = null) {
  const label = thresholdLabel(name, strategy);
  if (name.endsWith('Rate') || name === 'minFpsRatio') {
    return `${label} ${formatPercent(actual)}，阈值 ${formatPercent(limit)}`;
  }
  if (name.endsWith('LatencyMs')) {
    return `${label} ${actual}ms，阈值 ${limit}ms`;
  }
  return `${label} ${actual}，阈值 ${limit}`;
}

export function thresholdLabel(name, strategy = null) {
  const s = strategy ?? strategyForTaskType(DEFAULT_TYPE);
  return {
    minThroughputFps: `最低${s.throughputLabel}`,
    minFpsRatio: `${s.throughputLabel}达标率`,
    maxMissingRate: '采样缺失率',
    avgDiscardRate: '平均丢弃率',
    maxDiscardRate: '丢弃率',
    maxPacketDiscardRate: '网络丢包率',
    maxDiskUsedPercent: '设备磁盘使用率',
    maxPrimaryLatencyMs: s.primaryLatencyLabel,
    maxDetectorLatencyMs: '检测节点延时',
    maxAnalysisLatencyMs: '分析节点延时',
    maxCriticalPathLatencyMs: s.criticalLatencyLabel,
    maxEndToEndLatencyMs: '端到端链路延时',
  }[name] ?? name;
}

function checkRecord(taskStat, name, threshold, actual, result) {
  return {
    taskKey: taskStat.taskKey,
    taskDisplayName: taskStat.taskDisplayName,
    taskType: taskStat.taskType,
    strategy: strategyForTask(taskStat).id,
    name,
    threshold,
    actual,
    result,
  };
}

function matchesAny(name, patterns) {
  return patterns.some((pattern) => name.includes(String(pattern).toLowerCase()));
}

function formatPercent(v) {
  return `${round(Number(v) * 100, 2)}%`;
}

function round(v, digits) {
  const f = 10 ** digits;
  return Math.round(v * f) / f;
}

function nodeAvgUs(n) {
  if (n?.durationAvgUs != null) return Number(n.durationAvgUs);
  if (n?.durationMs != null) return Number(n.durationMs) * 1000;
  return Number(n?.duration ?? 0);
}

function nodeName(n) {
  return String(n?.name ?? '').toLowerCase();
}

function nodeLatencyMs(n) {
  const us = nodeAvgUs(n);
  return Number.isFinite(us) && us > 0 ? us / 1000 : null;
}
