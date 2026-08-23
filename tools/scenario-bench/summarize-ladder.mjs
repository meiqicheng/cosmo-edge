// summarize-ladder.mjs — Compact per-step digest from a scenario-bench metrics.json
// Usage: node summarize-ladder.mjs <metrics.json> [out.json]
import fs from 'node:fs';

const [, , inputPath, outPath] = process.argv;
const m = JSON.parse(fs.readFileSync(inputPath, 'utf8'));

const holdSamples = m.samples.filter((s) => s.phase === 'hold');
const steps = [...new Set(holdSamples.map((s) => s.targetChannels))].sort((a, b) => a - b);

function detectorAvgMs(sample) {
  const values = [];
  for (const c of sample.channels ?? []) {
    for (const n of c.nodeDurationInfos ?? []) {
      if (/AiDetector/i.test(n.name) && Number.isFinite(n.durationAvgUs)) {
        values.push(n.durationAvgUs / 1000);
      }
    }
  }
  return values.length ? values.reduce((a, b) => a + b, 0) / values.length : null;
}

const rows = steps.map((ch) => {
  const stepSamples = holdSamples.filter((s) => s.targetChannels === ch);
  const fps = [];
  const discard = [];
  const detMs = [];
  let npuMax = null;
  let cpuMax = null;
  let memMax = null;
  for (const s of stepSamples) {
    for (const c of s.channels ?? []) {
      if (!c.missing && Number.isFinite(c.measuredFps)) fps.push(c.measuredFps);
      if (!c.missing && typeof c.discardRate === 'number') discard.push(c.discardRate);
    }
    const d = detectorAvgMs(s);
    if (d != null) detMs.push(d);
    const npu = s.hardware?.npuUtilization?.usedPercent;
    const cpu = s.hardware?.cpuUtilization?.usedPercent;
    const mem = s.hardware?.generalMemoryUtilization?.usedPercent;
    if (Number.isFinite(npu)) npuMax = Math.max(npuMax ?? -Infinity, npu);
    if (Number.isFinite(cpu)) cpuMax = Math.max(cpuMax ?? -Infinity, cpu);
    if (Number.isFinite(mem)) memMax = Math.max(memMax ?? -Infinity, mem);
  }
  const r = (v, digits = 3) => (Number.isFinite(v) ? Math.round(v * 10 ** digits) / 10 ** digits : null);
  return {
    channels: ch,
    samples: stepSamples.length,
    fpsMin: fps.length ? r(Math.min(...fps)) : null,
    discardAvg: discard.length ? r(discard.reduce((a, b) => a + b, 0) / discard.length, 4) : null,
    discardMax: discard.length ? r(Math.max(...discard), 4) : null,
    detectorAvgMs: detMs.length ? r(detMs.reduce((a, b) => a + b, 0) / detMs.length, 1) : null,
    detectorAvgMsMax: detMs.length ? r(Math.max(...detMs), 1) : null,
    npuMax: npuMax === null ? null : r(npuMax, 1),
    cpuMax: cpuMax === null ? null : r(cpuMax, 1),
    memMax: memMax === null ? null : r(memMax, 1),
  };
});

const digest = {
  scenarioName: m.scenarioName,
  algorithmId: m.algorithmId,
  targetFps: m.targetFps,
  baselineFps: m.baselineFps,
  status: m.status,
  startedAt: m.startedAt,
  endedAt: m.endedAt,
  bottleneck: m.bottleneck ?? null,
  steps: rows,
};

if (outPath) fs.writeFileSync(outPath, `${JSON.stringify(digest, null, 2)}\n`);
console.log(JSON.stringify(digest, null, 1));
