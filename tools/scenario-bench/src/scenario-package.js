// scenario-package.js - Parse a scenario directory into a normalized workload.
//
// Canonical scenario.yml shape:
//   channels: { mode, repeatCount, sources: [...] }
//   tasks: [{ id, type, algorithmId, scheduleId, template }]
//   bindings: [{ task, channels }]        // optional, defaults to all tasks/all channels
//   thresholds: { pass, taskTypes, tasks } // optional
//
// A single-task benchmark is just tasks[] with one entry.

import fs from 'node:fs';
import path from 'node:path';
import { parse as parseYaml } from 'yaml';
import { normalizeTaskType } from './task-strategies.js';

const FPS_ACTION_ID = 'AA_00001';
const CLASSIFIER_FPS_ACTION_ID = 'AA_00002';
const VLM_ACTION_IDS = new Set(['DA_00003', 'PDA_00003']);
const FPS_ACTION_IDS = new Set([FPS_ACTION_ID, CLASSIFIER_FPS_ACTION_ID, ...VLM_ACTION_IDS]);
const SUPPORTED_VIDEO_MODES = new Set(['local', 'rtsp-fidelity', 'rtsp-deterministic']);
export const DEFAULT_HOLD_SEC = 30;
export const DEFAULT_VLM_HOLD_SEC = 60;

export class ScenarioPackage {
  /**
   * @param {string} dir absolute or relative path to the scenario directory.
   */
  constructor(dir) {
    this.dir = path.resolve(dir);
    this.scenario = null;
    this.thresholds = null;
    this.videos = null;
    this.channelConfig = null;
    this.tasks = [];
    this.bindings = [];

    // Primary-task accessors keep report metadata compact for single-task runs.
    this.template = null;
    this.targetFps = null;
  }

  /** Load and validate the package. Returns this for chaining. */
  load() {
    this.scenario = parseYaml(this._readText('scenario.yml')) ?? {};
    this.thresholds = this.scenario.thresholds ?? {};
    this._loadWorkload();

    this._resolveVideoPaths();
    this._validate();

    this.template = this.primaryTask?.template ?? null;
    this.targetFps = this.primaryTask?.targetFps ?? null;
    return this;
  }

  // -- normalized accessors -------------------------------------------------

  get primaryTask() {
    return this.tasks[0] ?? null;
  }

  /** algorithmId as a string for the primary task. */
  get algorithmId() {
    return this.primaryTask?.algorithmId ?? '';
  }

  get scheduleId() {
    return this.primaryTask?.scheduleId ?? '';
  }

  get loadProfile() {
    return this.scenario.loadProfile ?? [];
  }

  get sampleIntervalSec() {
    return Number(this.scenario.sampleIntervalSec ?? 5);
  }

  get videoMode() {
    const m = this.videos?.mode;
    if (!SUPPORTED_VIDEO_MODES.has(m)) {
      throw new Error(`scenario.yml channels: unsupported mode "${m}"`);
    }
    return m;
  }

  /**
   * Local video repeat count. 0 = loop forever (AlgChannelDemux treats
   * video_repeat_count_ <= 0 as unlimited), 1 = play once then stop.
   */
  get videoRepeatCount() {
    const v = this.channelConfig?.repeatCount;
    if (v == null) return this.videoMode === 'local' ? 0 : 1;
    return Number(v);
  }

  /** Primary-task taskConfig accessor. */
  get taskConfig() {
    return this.primaryTask?.taskConfig ?? { params: [], areas: [] };
  }

  /** Primary-task layout save payload accessor. */
  get layoutSavePayload() {
    if (!this.primaryTask) throw new Error('workload: no tasks defined');
    return this.primaryTask.layoutSavePayload;
  }

  /** Payloads for all task templates that need layout/save. */
  get layoutSavePayloads() {
    return this.tasks.map((task) => ({
      taskId: task.id,
      displayName: task.displayName,
      algorithmId: task.algorithmId,
      payload: task.layoutSavePayload,
    }));
  }

  // -- loaders --------------------------------------------------------------

  _loadWorkload() {
    if (!this.scenario.channels) {
      throw new Error('scenario.yml: missing channels');
    }
    this.channelConfig = this.scenario.channels;
    this.videos = this._videosFromChannels(this.scenario.channels);
    if (!Array.isArray(this.scenario.tasks) || !this.scenario.tasks.length) {
      throw new Error('scenario.yml: workload must define tasks[]');
    }

    this.tasks = this.scenario.tasks.map((taskSpec, index) => {
      const templateFile = taskSpec.template ?? (this.scenario.tasks.length === 1 ? 'algorithm-template.json' : null);
      if (!templateFile) throw new Error(`scenario.yml: tasks[${index}].template is required`);
      const template = this._readJson(templateFile);
      return this._normalizeTask({ ...taskSpec, template: templateFile }, template, index);
    });

    this.bindings = this._normalizeBindings(this.scenario.bindings);
    this.scenario.loadProfile = this._normalizeLoadProfile(this.scenario.loadProfile);
  }

  _videosFromChannels(channels) {
    const mode = channels.mode;
    const sources = channels.sources ?? channels.local ?? channels.rtsp ?? [];
    const videos = { mode };
    if (mode === 'local') {
      videos.local = sources;
    } else {
      videos.rtsp = sources;
    }
    return videos;
  }

  _normalizeTask(spec, template, index) {
    const algorithmId = String(spec.algorithmId ?? template.algorithmId ?? template.id ?? template.algorithmCode ?? '');
    if (!algorithmId) {
      throw new Error(`scenario.yml: tasks[${index}].algorithmId cannot be derived`);
    }

    const id = String(spec.id ?? spec.name ?? `task-${index + 1}`);
    const algorithmCode = String(spec.algorithmCode ?? template.algorithmCode ?? template.algorithmId ?? algorithmId);
    const scheduleId = spec.scheduleId ?? '';
    const vlm = detectVlmMode(template);
    const vlmCompletionActionId = vlm.direct ? detectVlmCompletionActionId(template) : null;
    const type = vlm.direct ? 'vlm' : (spec.type ?? 'cv');
    const explicitTargetFps = spec.targetFps != null ? Number(spec.targetFps) : null;
    const targetFps = explicitTargetFps ?? extractTargetFpsFromTemplate(template);
    const normalizedType = normalizeTaskType(type);
    const videoReadFps = this.videoMode === 'local' && normalizedType === 'vlm' ? targetFps : null;
    const taskConfig = buildTaskConfig(
      template,
      this.videoRepeatCount,
      videoReadFps,
      spec.taskConfig,
      `tasks[${index}]`,
    );

    return {
      id,
      displayName: spec.displayName ?? template.algorithmName ?? id,
      type,
      hasNestedVlmReview: vlm.review,
      vlmCompletionActionId,
      algorithmId,
      algorithmCode,
      scheduleId,
      templateFile: spec.template ?? (index === 0 ? 'algorithm-template.json' : null),
      template,
      targetFps: Number.isFinite(targetFps) && targetFps > 0 ? targetFps : null,
      taskConfig,
      layoutSavePayload: buildLayoutSavePayload(template, explicitTargetFps),
    };
  }

  _normalizeBindings(bindings) {
    if (!bindings?.length) {
      return this.tasks.map((task) => ({ taskId: task.id, channels: 'all' }));
    }

    return bindings.map((binding, index) => {
      const taskId = String(binding.task ?? binding.taskId ?? '');
      if (!taskId) throw new Error(`scenario.yml: bindings[${index}].task is required`);
      return {
        taskId,
        channels: binding.channels ?? 'all',
      };
    });
  }

  _normalizeLoadProfile(loadProfile) {
    if (!Array.isArray(loadProfile)) return loadProfile;

    const defaultHoldSec = defaultHoldSecForTasks(this.tasks);
    return loadProfile.map((step) => ({
      ...step,
      channels: Number(step.channels),
      holdSec: step.holdSec == null ? defaultHoldSec : Number(step.holdSec),
    }));
  }

  // -- file helpers ---------------------------------------------------------

  _readText(name) {
    const p = path.isAbsolute(name) ? name : path.join(this.dir, name);
    if (!fs.existsSync(p)) {
      throw new Error(`Missing scenario file: ${p}`);
    }
    return fs.readFileSync(p, 'utf8');
  }

  _readJson(name) {
    return JSON.parse(this._readText(name));
  }

  /**
   * Resolve relative file entries against the scenario directory, so the
   * channel manager receives absolute paths.
   */
  _resolveVideoPaths() {
    if (this.videos?.mode !== 'local' || !Array.isArray(this.videos.local)) return;
    for (const src of this.videos.local) {
      if (src.file && !path.isAbsolute(src.file)) {
        src.file = path.resolve(this.dir, src.file);
      }
    }
  }

  _validate() {
    if (!this.scenario?.name) throw new Error('scenario.yml: missing "name"');
    if (!Array.isArray(this.scenario?.loadProfile) || !this.scenario.loadProfile.length) {
      throw new Error('scenario.yml: loadProfile must have at least one step');
    }
    for (const [i, step] of this.scenario.loadProfile.entries()) {
      if (!Number.isInteger(step.channels) || step.channels < 1) {
        throw new Error(`scenario.yml: loadProfile[${i}].channels must be a positive integer`);
      }
      if (!Number.isFinite(step.holdSec) || step.holdSec <= 0) {
        throw new Error(`scenario.yml: loadProfile[${i}].holdSec must be > 0`);
      }
    }
    if (!this.videos?.mode) throw new Error('scenario.yml channels: missing "mode"');
    this.videoMode; // re-trigger mode validation
    if (this.videoMode === 'local' && !this.videos.local?.length) {
      throw new Error('scenario.yml channels: local mode requires at least one source');
    }
    if (this.videoMode !== 'local' && !this.videos.rtsp?.length) {
      throw new Error('scenario.yml channels: rtsp mode requires at least one source');
    }

    if (!this.tasks.length) throw new Error('scenario.yml: at least one task is required');
    const seen = new Set();
    for (const task of this.tasks) {
      if (seen.has(task.id)) throw new Error(`scenario.yml: duplicate task id "${task.id}"`);
      seen.add(task.id);
      if (!task.scheduleId) throw new Error(`scenario.yml: task "${task.id}" missing scheduleId`);
      if (task.hasNestedVlmReview) {
        throw new Error(
          `scenario.yml: task "${task.id}" enables BA_00004 LLM review, but RunningDetail does not `
          + 'expose that nested inference; ScenarioBench can only measure direct DA_00003/PDA_00003 VLM nodes',
        );
      }
      if (normalizeTaskType(task.type) === 'vlm' && task.targetFps == null) {
        throw new Error(`scenario.yml: VLM task "${task.id}" must define targetFps, for example targetFps: 0.2`);
      }
    }

    const taskIds = new Set(this.tasks.map((t) => t.id));
    for (const binding of this.bindings) {
      if (!taskIds.has(binding.taskId)) {
        throw new Error(`scenario.yml: binding references unknown task "${binding.taskId}"`);
      }
    }
  }
}

function buildTaskConfig(
  template,
  videoRepeatCount,
  videoReadFps = null,
  taskConfigOverride = null,
  taskPath = 'task',
) {
  const base = template.taskConfig ?? { params: [], areas: [] };
  if (taskConfigOverride != null
      && (typeof taskConfigOverride !== 'object' || Array.isArray(taskConfigOverride))) {
    throw new Error(`scenario.yml: ${taskPath}.taskConfig must be an object`);
  }

  const override = taskConfigOverride ?? {};
  if (override.params != null && !Array.isArray(override.params)) {
    throw new Error(`scenario.yml: ${taskPath}.taskConfig.params must be an array`);
  }
  if (override.areas != null && !Array.isArray(override.areas)) {
    throw new Error(`scenario.yml: ${taskPath}.taskConfig.areas must be an array`);
  }

  const params = mergeTaskConfigParams(
    Array.isArray(base.params) ? base.params : [],
    override.params ?? [],
    taskPath,
  );
  const hasRepeat = params.some((p) => p?.key === 'param.videoRepeatCount');
  if (!hasRepeat) {
    params.push({ key: 'param.videoRepeatCount', value: String(videoRepeatCount) });
  }
  const hasReadFps = params.some((p) => p?.key === 'param.videoReadFps');
  if (!hasReadFps && Number.isFinite(videoReadFps) && videoReadFps > 0) {
    params.push({ key: 'param.videoReadFps', value: String(videoReadFps) });
  }
  return {
    ...base,
    ...override,
    params,
    areas: override.areas ?? base.areas ?? [],
  };
}

function mergeTaskConfigParams(baseParams, overrideParams, taskPath) {
  const params = baseParams.map((param) => ({ ...param }));
  const indexByKey = new Map();
  for (const [index, param] of params.entries()) {
    if (param?.key != null && !indexByKey.has(String(param.key))) {
      indexByKey.set(String(param.key), index);
    }
  }

  const overrideKeys = new Set();
  for (const [index, param] of overrideParams.entries()) {
    const key = String(param?.key ?? '').trim();
    if (!key) {
      throw new Error(
        `scenario.yml: ${taskPath}.taskConfig.params[${index}].key must be a non-empty string`,
      );
    }
    if (param.value == null) {
      throw new Error(
        `scenario.yml: ${taskPath}.taskConfig.params[${index}].value is required`,
      );
    }
    if (overrideKeys.has(key)) {
      throw new Error(`scenario.yml: ${taskPath}.taskConfig.params has duplicate key "${key}"`);
    }
    overrideKeys.add(key);

    const normalized = { ...param, key, value: String(param.value) };
    const existingIndex = indexByKey.get(key);
    if (existingIndex == null) {
      indexByKey.set(key, params.length);
      params.push(normalized);
    } else {
      params[existingIndex] = normalized;
    }
  }
  return params;
}

export function defaultHoldSecForTasks(tasks) {
  const hasVlm = (tasks ?? []).some((task) => {
    const type = task && typeof task === 'object' ? task.type : task;
    return normalizeTaskType(type) === 'vlm';
  });
  return hasVlm ? DEFAULT_VLM_HOLD_SEC : DEFAULT_HOLD_SEC;
}

function buildLayoutSavePayload(template, targetFpsOverride = null) {
  const algorithmId = String(template.algorithmId ?? template.id ?? template.algorithmCode ?? '');
  if (!algorithmId) throw new Error('template: cannot derive algorithmId for layout save');
  const str = (v) => (v == null ? undefined : String(v));
  return {
    confVersionId: template.confVersionId,
    algorithmId,
    configVersionName: str(template.configVersionName ?? template.confVersionName),
    algorithmCategory: str(template.algorithmCategory),
    algorithmUsage: str(template.algorithmUsage),
    remark: str(template.remark),
    atomicList: str(template.atomicList),
    algorithmProcessdata: str(overrideProcessTargetFps(template.algorithmProcessdata, targetFpsOverride)),
    algorithmMetadata: str(template.algorithmMetadata),
    filePath: str(template.filePath),
  };
}

function overrideProcessTargetFps(raw, targetFps) {
  if (!Number.isFinite(targetFps) || targetFps <= 0 || raw == null) return raw;

  let nodes;
  try {
    nodes = typeof raw === 'string' ? JSON.parse(raw) : raw;
  } catch {
    return raw;
  }
  if (!Array.isArray(nodes)) return raw;

  let updated = false;
  const nextNodes = nodes.map((node) => {
    if (!FPS_ACTION_IDS.has(String(node?.actionId ?? ''))) return node;

    const configWasString = typeof node.configObject === 'string';
    let configObject = node.configObject;
    if (configWasString) {
      try { configObject = JSON.parse(configObject); } catch { return node; }
    }
    if (!Array.isArray(configObject?.params)) return node;

    let nodeUpdated = false;
    const params = configObject.params.map((param) => {
      if (param?.key !== 'fps') return param;
      nodeUpdated = true;
      return { ...param, value: String(targetFps) };
    });
    if (!nodeUpdated) return node;

    updated = true;
    const nextConfig = { ...configObject, params };
    return {
      ...node,
      configObject: configWasString ? JSON.stringify(nextConfig) : nextConfig,
    };
  });

  return updated ? JSON.stringify(nextNodes) : raw;
}

function parseProcessData(template) {
  const raw = template.algorithmProcessdata;
  if (raw == null) return [];
  if (typeof raw === 'string') {
    try {
      const parsed = JSON.parse(raw);
      return Array.isArray(parsed) ? parsed : [];
    } catch {
      return [];
    }
  }
  return Array.isArray(raw) ? raw : [];
}

function extractTargetFpsFromTemplate(template) {
  const nodes = parseProcessData(template);
  const fpsNode = nodes.find((n) => n && VLM_ACTION_IDS.has(String(n.actionId ?? '')))
    ?? nodes.find((n) => n && n.actionId === FPS_ACTION_ID);
  if (!fpsNode) return null;
  let configObj = fpsNode.configObject;
  if (typeof configObj === 'string') {
    try { configObj = JSON.parse(configObj); } catch { return null; }
  }
  const params = configObj?.params;
  if (!Array.isArray(params)) return null;
  const fpsParam = params.find((p) => p?.key === 'fps');
  if (!fpsParam) return null;
  const val = Number(fpsParam.value);
  return Number.isFinite(val) && val > 0 ? val : null;
}

export function detectVlmMode(template) {
  const nodes = parseProcessData(template);
  const direct = nodes.some((node) => VLM_ACTION_IDS.has(String(node?.actionId ?? '')));
  const review = nodes.some((node) => {
    let configObj = node?.configObject;
    if (typeof configObj === 'string') {
      try { configObj = JSON.parse(configObj); } catch { return false; }
    }
    return Array.isArray(configObj?.params)
      && configObj.params.some((param) =>
        param?.key === 'enableLlmReview' && String(param.value) === '1');
  });
  return { direct, review };
}

/**
 * Select the counter that represents one completed result for this direct VLM
 * graph. Video VLM graphs dispatch completed results through BA_00004, while
 * the picture-only PDA graph owns its PDA_00003 counter directly. DA-only is
 * retained as the legacy fallback for external templates.
 */
export function detectVlmCompletionActionId(template) {
  const actionIds = new Set(parseProcessData(template).map(
    (node) => String(node?.actionId ?? '').trim().toUpperCase(),
  ));
  const hasDirectVlm = [...VLM_ACTION_IDS].some((actionId) => actionIds.has(actionId));
  if (!hasDirectVlm) return null;
  if (actionIds.has('BA_00004')) return 'BA_00004';
  if (actionIds.has('PDA_00003')) return 'PDA_00003';
  return actionIds.has('DA_00003') ? 'DA_00003' : null;
}

// Re-export for unit testing of the fps extractor on arbitrary template objects.
export function extractTargetFps(template) {
  return extractTargetFpsFromTemplate(template);
}
