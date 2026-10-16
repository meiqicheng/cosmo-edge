// derive-vlm.mjs — Derive video-chain VLM (DA_00003) capacity templates from a
// board-exported algorithm template.
//
// Background: the picture-chain VLM action (PDA_00003, e.g. algorithm 7463005)
// cannot be bound to a video channel — the engine answers
// "Action: PDA_00003-Qwen3.5 VLM Analysis Not Support" (resCode 7).
// The video chain instead implements DA_00003, which is what board template
// 34707 (视觉语言大模型分析, algorithmUsage=1) uses:
//   BA_00001 视频解码 -> DA_00003 语言视觉大模型 -> BA_00004 事件上报
//   atomicCode 8888999 (Qwen3VL)  -- this is also the model lookup key
//
// The fps knob lives in the DA_00003 configObject.params entry (key === 'fps').
//
// Usage:
//   node scripts/derive-vlm.mjs
//   node scripts/derive-vlm.mjs --src ../templates/vlm-video-34707.json --outdir ../../../output/bench-templates
import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const here = path.dirname(url.fileURLToPath(import.meta.url));

function parseArgs(argv) {
  const args = {
    src: path.join(here, '..', 'templates', 'vlm-video-34707.json'),
    outdir: path.join(here, '..', '..', '..', 'output', 'bench-templates'),
    variants: '746601:0.1,746602:0.3,746603:1',
  };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === '--src') args.src = argv[++i];
    else if (argv[i] === '--outdir') args.outdir = argv[++i];
    else if (argv[i] === '--variants') args.variants = argv[++i];
  }
  return args;
}

// fps lives in the DA_00003 configObject.params (key === 'fps').
// Preserve the original configObject JSON type: MvActionConfigObject::from_json
// only understands the object form; a string form is silently dropped
// (params empty -> initFps stays -1 -> full-fps).
function setNodeFps(nodesArr, actionId, fps) {
  return nodesArr.map((n) => {
    if (n.actionId !== actionId) return n;
    const wasString = typeof n.configObject === 'string';
    const cfg = wasString ? JSON.parse(n.configObject) : n.configObject;
    if (!cfg || !Array.isArray(cfg.params)) return n;
    let hit = false;
    const params = cfg.params.map((p) => {
      if (p && p.key === 'fps') { hit = true; return { ...p, value: String(fps) }; }
      return p;
    });
    if (!hit) { console.warn('  WARN: no fps param in ' + actionId); return n; }
    const nextCfg = { ...cfg, params };
    return { ...n, configObject: wasString ? JSON.stringify(nextCfg) : nextCfg };
  });
}

function renameAll(t, id, name, desc) {
  t.algorithmId = id;
  t.algorithmCode = Number(id);
  t.algorithmName = name;
  t.description = desc;
  t.remark = desc;
  t.confVersionId = id + '-v1';
  t.configVersionList = [];
  delete t.createTime;
  delete t.updateTime;
  return t;
}

const args = parseArgs(process.argv);
const src = JSON.parse(fs.readFileSync(path.resolve(process.cwd(), args.src), 'utf8'));
fs.mkdirSync(path.resolve(process.cwd(), args.outdir), { recursive: true });

const nodes = JSON.parse(src.algorithmProcessdata);
console.log('nodes:', nodes.map((n) => n.actionId + '(' + n.flowActionId + ')pre=' + n.preFlowActionId).join(' '));
console.log('atomics:', JSON.parse(src.atomicList).map((a) => a.atomicCode + '@' + a.position).join(' '));
console.log('usage:', src.algorithmUsage, 'code:', src.algorithmId);
if (src.algorithmUsage !== 1) console.warn('WARN: template usage != 1 (not a video-chain algorithm)');

for (const spec of args.variants.split(',')) {
  const [id, fps] = spec.split(':');
  const t = JSON.parse(JSON.stringify(src));
  let pd = setNodeFps(JSON.parse(t.algorithmProcessdata), 'DA_00003', fps);
  t.algorithmProcessdata = JSON.stringify(pd);
  renameAll(t, id, 'Qwen3.5 VLM Analysis fps' + fps,
    'Capacity-test VLM variant of template 34707 (DA_00003, atomic 8888999), fps=' + fps);
  const file = path.join(path.resolve(process.cwd(), args.outdir), 'vlm-video-' + id + '.json');
  fs.writeFileSync(file, JSON.stringify(t, null, 2));
  const node = pd.find((n) => n.actionId === 'DA_00003');
  const cfg = typeof node.configObject === 'string' ? JSON.parse(node.configObject) : node.configObject;
  console.log('wrote', file, '| fps=', cfg.params.find((p) => p.key === 'fps').value);
}
console.log('done');
