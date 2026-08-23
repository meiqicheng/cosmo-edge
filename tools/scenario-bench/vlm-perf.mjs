// vlm-perf.mjs — VLM (Qwen3.5 RKLLM) picture-inference performance probe.
//
// Measures end-to-end /gtw/cwai/aihost/PTaskDetectPic latency against one device:
//   1. warmup calls (model load / first-token path)
//   2. sequential back-to-back series (latency distribution + sustained throughput)
//   3. concurrency phases: same-task 2-way, cross-task 2-way, cross-task 4-way
// A background sampler polls /System/QueryHardwareResource throughout so NPU/CPU
// utilization under load is captured with timestamps.
//
// Image delivery uses the imageBase64 field (same path as the on-device acceptance
// evidence), not the chunked uploadTemp staging flow.
//
// Usage:
//   node tools/scenario-bench/vlm-perf.mjs \
//     --device http://192.168.112.196:8000 --user admin --password admin123 \
//     --image <jpg> --out <dir> [--algorithm-code 7463005] [--tasks 4]
//     [--warmup 2] [--sequential 10] [--rounds 5]

import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { CosmoClient } from './src/cosmo-client.js';

function parseArgs(argv) {
  const args = {
    device: 'http://192.168.112.196:8000',
    user: 'admin',
    password: 'admin123',
    image: '',
    out: '',
    algorithmCode: '7463005',
    tasks: 4,
    warmup: 2,
    sequential: 10,
    rounds: 5,
  };
  for (let i = 2; i < argv.length; i += 2) {
    const key = String(argv[i] ?? '').replace(/^--/, '');
    const value = argv[i + 1];
    if (!(key in args)) throw new Error(`Unknown option --${key}`);
    args[key] = value;
  }
  args.tasks = Number(args.tasks);
  args.warmup = Number(args.warmup);
  args.sequential = Number(args.sequential);
  args.rounds = Number(args.rounds);
  if (!args.image) throw new Error('--image is required');
  if (!args.out) throw new Error('--out is required');
  if (!Number.isInteger(args.tasks) || args.tasks < 1 || args.tasks > 8) {
    throw new Error('--tasks must be an integer 1..8');
  }
  return args;
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/** Trim a hardware-resource payload to the fields worth persisting. */
function slimHw(raw) {
  if (!raw || typeof raw !== 'object') return raw;
  const slim = {};
  for (const [key, value] of Object.entries(raw)) {
    if (Array.isArray(value)) {
      slim[key] = value.length <= 16 ? value : `${value.length} items`;
    } else if (value && typeof value === 'object') {
      slim[key] = '[object]';
    } else {
      slim[key] = value;
    }
  }
  return slim;
}

/** Extract the decision-relevant summary from a DetectPic resData. */
function summarizeResult(resData) {
  if (!resData || typeof resData !== 'object') return {};
  const areas = Array.isArray(resData.areaList) ? resData.areaList : [];
  const detectedAreas = areas.filter((a) => a?.bDetected === true).length;
  let targetCount = 0;
  const labels = [];
  for (const area of areas) {
    const targets = Array.isArray(area?.targetList) ? area.targetList : [];
    targetCount += targets.length;
    for (const target of targets) {
      // Observed wire shapes: classifyRst[] on some builds, confidence[] here.
      const rst = Array.isArray(target?.classifyRst) ? target.classifyRst
        : Array.isArray(target?.confidence) ? target.confidence : [];
      for (const c of rst) {
        if (c?.label) labels.push(`${c.label}@${c.confidence ?? '?'}`);
      }
    }
  }
  // Surface any engine-provided timing/token fields if present.
  const timingKeys = {};
  const scan = (obj, prefix, depth) => {
    if (!obj || typeof obj !== 'object' || depth > 3) return;
    for (const [k, v] of Object.entries(obj)) {
      if (/ms|time|duration|token|cost/i.test(k) && typeof v !== 'object') {
        timingKeys[`${prefix}${k}`] = v;
      } else if (v && typeof v === 'object') {
        scan(v, `${prefix}${k}.`, depth + 1);
      }
    }
  };
  scan(resData, '', 0);
  return {
    areas: areas.length,
    detectedAreas,
    targetCount,
    labels: labels.slice(0, 6),
    timingKeys,
  };
}

async function detectOnce(client, taskId, algorithmCode, imageBase64) {
  const startedAt = Date.now();
  try {
    const res = await client.pictureDetect({
      mvDebug: 'Cosmo-Debug',
      taskId,
      algorithmCode,
      algorithmUpdateTime: String(Date.now()),
      imageBase64,
      needRetImg: false,
    });
    return {
      ok: true,
      elapsedMs: Date.now() - startedAt,
      summary: summarizeResult(res.resData),
      rawFirstCall: null,
    };
  } catch (err) {
    return {
      ok: false,
      elapsedMs: Date.now() - startedAt,
      error: String(err?.message ?? err),
    };
  }
}

function stats(values) {
  const sorted = [...values].sort((a, b) => a - b);
  const pick = (q) => sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))];
  return {
    n: values.length,
    min: sorted[0],
    p50: pick(0.5),
    p90: pick(0.9),
    max: sorted[sorted.length - 1],
    mean: Math.round(values.reduce((s, v) => s + v, 0) / values.length),
  };
}

async function main() {
  const args = parseArgs(process.argv);
  const imageBuf = fs.readFileSync(args.image);
  const imageBase64 = imageBuf.toString('base64');
  const imageSizeBytes = imageBuf.length;

  fs.mkdirSync(args.out, { recursive: true });

  const client = new CosmoClient({ base: args.device, user: args.user, password: args.password });
  await client.login();

  // --- Background hardware sampler -------------------------------------
  const hwSamples = [];
  let phaseLabel = 'init';
  let sampling = true;
  const sampler = (async () => {
    while (sampling) {
      try {
        const res = await client.queryHardwareResource();
        hwSamples.push({ t: Date.now(), phase: phaseLabel, res: slimHw(res) });
      } catch {
        hwSamples.push({ t: Date.now(), phase: phaseLabel, error: 'hw-query-failed' });
      }
      await sleep(2000);
    }
  })();

  const result = {
    meta: {
      device: args.device,
      algorithmCode: args.algorithmCode,
      imageSizeBytes,
      imagePath: path.resolve(args.image),
      startedAt: new Date().toISOString(),
      deliveryMode: 'imageBase64',
      firmwareNote: 'device firmware cosmo-V1.1.0-79785ee2b00b71a34b3259c0f488de9b',
    },
    tasksCreated: [],
    warmup: [],
    sequential: [],
    concurrency: {},
    hwFinal: null,
    firstRawResData: null,
    errors: [],
  };

  const createdTasks = [];
  try {
    // --- Create picture-analysis tasks ---------------------------------
    for (let k = 0; k < args.tasks; k += 1) {
      const taskId = `vlmperf-${Date.now()}-${k}`;
      await client.pictureTaskCreate({
        mvDebug: 'Cosmo-Debug',
        taskId,
        algorithmCode: args.algorithmCode,
        algorithmUpdateTime: String(Date.now()),
      });
      createdTasks.push(taskId);
      result.tasksCreated.push(taskId);
      console.log(`[task] created ${taskId}`);
    }

    const captureRawOnce = { done: false };

    async function timedCall(taskId, keepRaw = false) {
      const startedAt = Date.now();
      const res = await client.pictureDetect({
        mvDebug: 'Cosmo-Debug',
        taskId,
        algorithmCode: args.algorithmCode,
        algorithmUpdateTime: String(Date.now()),
        imageBase64,
        needRetImg: false,
      });
      const elapsedMs = Date.now() - startedAt;
      if (keepRaw && !captureRawOnce.done) {
        result.firstRawResData = res.resData;
        captureRawOnce.done = true;
      }
      return { ok: true, elapsedMs, summary: summarizeResult(res.resData) };
    }

    // --- Warmup ---------------------------------------------------------
    phaseLabel = 'warmup';
    for (let i = 0; i < args.warmup; i += 1) {
      const r = await detectOnce(client, createdTasks[0], args.algorithmCode, imageBase64);
      result.warmup.push(r);
      console.log(`[warmup ${i + 1}/${args.warmup}] ${r.ok ? `${r.elapsedMs}ms ${JSON.stringify(r.summary.labels)}` : `ERROR ${r.error}`}`);
    }

    // --- Sequential series ----------------------------------------------
    phaseLabel = 'sequential';
    for (let i = 0; i < args.sequential; i += 1) {
      let r;
      try {
        r = await timedCall(createdTasks[0], i === 0);
      } catch (err) {
        r = { ok: false, elapsedMs: null, error: String(err?.message ?? err) };
      }
      result.sequential.push(r);
      console.log(`[seq ${i + 1}/${args.sequential}] ${r.ok ? `${r.elapsedMs}ms` : `ERROR ${r.error}`}`);
    }
    const okSeq = result.sequential.filter((r) => r.ok);
    if (okSeq.length) {
      const st = stats(okSeq.map((r) => r.elapsedMs));
      st.throughputPerSec = +(1000 / st.mean).toFixed(3);
      result.sequentialStats = st;
      console.log(`[seq-stats] ${JSON.stringify(st)}`);
    }

    // --- Concurrency phases ----------------------------------------------
    async function concurrencyPhase(name, workerSpecs) {
      phaseLabel = name;
      const rounds = [];
      for (let r = 0; r < args.rounds; r += 1) {
        const roundStart = Date.now();
        const settled = await Promise.all(
          workerSpecs.map((spec) =>
            detectOnce(client, spec.taskId, args.algorithmCode, imageBase64)
              .catch((err) => ({ ok: false, elapsedMs: null, error: String(err?.message ?? err) })),
          ),
        );
        const wallMs = Date.now() - roundStart;
        const okCount = settled.filter((x) => x.ok).length;
        rounds.push({ round: r + 1, wallMs, okCount, total: settled.length, results: settled });
        console.log(`[${name} r${r + 1}/${args.rounds}] wall=${wallMs}ms ok=${okCount}/${settled.length}`);
        await sleep(500);
      }
      const allOk = rounds.flatMap((x) => x.results).filter((x) => x.ok);
      const summary = {
        rounds: rounds.length,
        parallelism: workerSpecs.length,
        wallStats: stats(rounds.map((x) => x.wallMs)),
        perRequestStats: allOk.length ? stats(allOk.map((x) => x.elapsedMs)) : null,
        successRate: +(allOk.length / rounds.reduce((s, x) => s + x.total, 0)).toFixed(3),
        throughputPerSec: null,
      };
      if (allOk.length) {
        summary.throughputPerSec = +(
          (allOk.length / rounds.reduce((s, x) => s + x.wallMs, 0)) * 1000
        ).toFixed(3);
      }
      result.concurrency[name] = { summary, rounds };
      console.log(`[${name}-stats] ${JSON.stringify(summary)}`);
    }

    await concurrencyPhase('same-task-2way', [
      { taskId: createdTasks[0] },
      { taskId: createdTasks[0] },
    ]);
    if (createdTasks.length >= 2) {
      await concurrencyPhase('multi-task-2way', [
        { taskId: createdTasks[0] },
        { taskId: createdTasks[1] },
      ]);
    }
    if (createdTasks.length >= 4) {
      await concurrencyPhase('multi-task-4way', createdTasks.slice(0, 4).map((taskId) => ({ taskId })));
    }
  } catch (err) {
    result.errors.push(String(err?.message ?? err));
    console.error(`[fatal] ${err?.message ?? err}`);
  } finally {
    phaseLabel = 'final';
    result.hwFinal = slimHw(await client.queryHardwareResource().catch(() => null));
    sampling = false;
    await sampler.catch(() => {});
    result.hwSamples = hwSamples;

    for (const taskId of createdTasks) {
      try {
        await client.pictureTaskCancel({
          mvDebug: 'Cosmo-Debug',
          taskId,
          algorithmCode: args.algorithmCode,
        });
        console.log(`[cleanup] cancelled ${taskId}`);
      } catch (err) {
        result.errors.push(`cancel ${taskId}: ${String(err?.message ?? err)}`);
      }
    }

    result.meta.finishedAt = new Date().toISOString();
    const outFile = path.join(args.out, 'vlm-perf-results.json');
    fs.writeFileSync(outFile, JSON.stringify(result, null, 2));
    console.log(`\n[out] ${outFile}`);
  }
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
