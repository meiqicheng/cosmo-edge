// gen-vlm-scenarios.mjs — Generate video-chain VLM capacity scenario packages.
//
// Depends on the templates produced by derive-vlm.mjs (vlm-video-<id>.json).
//
// Usage:
//   node scripts/gen-vlm-scenarios.mjs
//   node scripts/gen-vlm-scenarios.mjs --tpldir ../../../output/bench-templates ^
//        --outdir ../../../output/bench-scenarios --max-channels 12 --hold-sec 60
import fs from 'node:fs';
import path from 'node:path';
import url from 'node:url';

const here = path.dirname(url.fileURLToPath(import.meta.url));
const repoRoot = path.join(here, '..', '..', '..');

function parseArgs(argv) {
  const args = {
    tpldir: path.join(repoRoot, 'output', 'bench-templates'),
    outdir: path.join(repoRoot, 'output', 'bench-scenarios'),
    video: '../../bench-inputs/1080p24.mp4', // relative to each generated package dir
    maxChannels: 12,
    holdSec: 60,
    variants: '746601:0.1,746602:0.3,746603:1',
  };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === '--tpldir') args.tpldir = argv[++i];
    else if (argv[i] === '--outdir') args.outdir = argv[++i];
    else if (argv[i] === '--video') args.video = argv[++i];
    else if (argv[i] === '--max-channels') args.maxChannels = Number(argv[++i]);
    else if (argv[i] === '--hold-sec') args.holdSec = Number(argv[++i]);
    else if (argv[i] === '--variants') args.variants = argv[++i];
  }
  return args;
}

const args = parseArgs(process.argv);
const tplDir = path.resolve(process.cwd(), args.tpldir);
const outRoot = path.resolve(process.cwd(), args.outdir);
fs.mkdirSync(outRoot, { recursive: true });

const LOAD = [];
for (let c = 1; c <= args.maxChannels; c++) LOAD.push('  - { channels: ' + c + ', holdSec: ' + args.holdSec + ' }');

const THRESHOLDS = `thresholds:
  pass:
    maxCriticalPathLatencyMs: 200
    maxDetectorLatencyMs: 150
    avgDiscardRate: 0.05
    maxPacketDiscardRate: 0.01
`;

const CHANNELS = `channels:
  mode: local
  repeatCount: 0
  sources:
    - name: input-1080p24
      file: ${args.video}
`;

for (const spec of args.variants.split(',')) {
  const [id, fps] = spec.split(':');
  const name = 'vlm-qwen35-08b-fps' + fps;
  const dir = path.join(outRoot, name);
  fs.mkdirSync(dir, { recursive: true });
  const yml = `name: v1.1-rk3588-${name}
displayName: VLM容量-Qwen3.5-0.8B(视频链 DA_00003)@${fps}fps-RK3588
sampleIntervalSec: 3

${CHANNELS}
tasks:
  - id: vlm
    displayName: Qwen3.5-0.8B视频分析@${fps}fps
    type: vlm
    algorithmId: "${id}"
    scheduleId: "e89c6c6385e5454b35cde0d1653vg"
    template: algorithm-template.json
    targetFps: ${fps}

bindings:
  - task: vlm
    channels: all

loadProfile:
${LOAD.join('\n')}

${THRESHOLDS}`;
  fs.writeFileSync(path.join(dir, 'scenario.yml'), yml);
  fs.copyFileSync(path.join(tplDir, 'vlm-video-' + id + '.json'), path.join(dir, 'algorithm-template.json'));
  console.log('pkg:', name, '| template vlm-video-' + id + '.json');
}
console.log('done ->', outRoot);
