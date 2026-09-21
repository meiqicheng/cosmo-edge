// export-layouts.mjs — Export board layout templates (7463 no-helmet, 7463005 VLM)
// Usage: node scripts/export-layouts.mjs [--device http://192.168.111.6] [--user admin --password admin] [--outdir ../../output/bench-templates]
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';

function parseArgs(argv) {
  const args = { device: 'http://192.168.111.6', user: 'admin', password: 'admin', outdir: '../../output/bench-templates' };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === '--device') args.device = argv[++i];
    else if (argv[i] === '--user') args.user = argv[++i];
    else if (argv[i] === '--password') args.password = argv[++i];
    else if (argv[i] === '--outdir') args.outdir = argv[++i];
  }
  return args;
}

async function post(base, route, body, token) {
  const headers = { 'Content-Type': 'application/json' };
  if (token) { headers.mtk = token; headers.token = token; }
  const resp = await fetch(base + '/gtw/cwai' + route, { method: 'POST', headers, body: JSON.stringify(body) });
  const text = await resp.text();
  let data = null;
  try { data = JSON.parse(text); } catch { /* binary */ }
  if (resp.status !== 200) throw new Error('HTTP ' + resp.status + ' on ' + route + ': ' + text.slice(0, 200));
  if (data && data.resCode !== 1) {
    const msg = Array.isArray(data.resMsg) && data.resMsg[0] ? (data.resMsg[0].msgText || data.resMsg[0].msgCode) : 'unknown';
    throw new Error('API error on ' + route + ': ' + msg);
  }
  return { resp, data, text };
}

// Raw post that returns the body as a Buffer without pre-reading (for file-download routes).
async function postRaw(base, route, body, token) {
  const headers = { 'Content-Type': 'application/json' };
  if (token) { headers.mtk = token; headers.token = token; }
  const resp = await fetch(base + '/gtw/cwai' + route, { method: 'POST', headers, body: JSON.stringify(body) });
  const buf = Buffer.from(await resp.arrayBuffer());
  if (resp.status !== 200) throw new Error('HTTP ' + resp.status + ' on ' + route + ': ' + buf.toString('utf8').slice(0, 200));
  return { resp, buf };
}

const args = parseArgs(process.argv);
const base = args.device.replace(/\/+$/, '');
const outdir = path.resolve(process.cwd(), args.outdir);
fs.mkdirSync(outdir, { recursive: true });

const pwdHash = crypto.createHash('md5').update(args.password, 'utf8').digest('hex').toUpperCase();
console.log('login', base, 'as', args.user);
const loginResp = await post(base, '/login/dologin', { account: args.user, pwd: pwdHash });
const token = loginResp.data.resData.mtk;
if (!token) throw new Error('no mtk in login response');
console.log('login OK, mtk', token.slice(0, 8) + '...');

// 1) Algorithm inventory page
const pageResp = await post(base, '/Algorithm/Page', { pageNum: 1, pageSize: 100 }, token);
const rd = pageResp.data.resData || {};
let algs = Array.isArray(rd) ? rd : (Array.isArray(rd.list) ? rd.list : []);
if (!algs.length) {
  console.log('Algorithm/Page resData keys:', Object.keys(rd), JSON.stringify(rd).slice(0, 400));
  // try common alternates
  for (const k of ['records', 'rows', 'data', 'algorithms', 'totalList']) {
    if (Array.isArray(rd[k])) { algs = rd[k]; break; }
  }
}
console.log('algorithms on board (' + algs.length + '):');
for (const a of algs) {
  console.log('  id=%s name=%s usage=%s category=%s', a.algorithmId ?? a.algorithmCode, a.algorithmName, a.algorithmUsage ?? '', a.algorithmCategory ?? '');
}
fs.writeFileSync(path.join(outdir, 'algorithm-page.json'), JSON.stringify(pageResp.data, null, 2));

// 2) Export single-alg layouts
for (const code of ['7463', '7463005', '7463001']) {
  try {
    const r = await postRaw(base, '/algorithm/layout/exportSingleAlg', { algorithmCode: code }, token);
    const buf = r.buf;
    const file = path.join(outdir, 'layout-' + code + '.bin');
    fs.writeFileSync(file, buf);
    const isGzip = buf[0] === 0x1f && buf[1] === 0x8b;
    const isJson = buf[0] === 0x7b || buf[0] === 0x5b;
    console.log('exported %s -> %s (%d bytes, %s)', code, file, buf.length, isGzip ? 'gzip' : (isJson ? 'json' : 'other'));
  } catch (e) {
    console.log('export %s FAILED: %s', code, e.message);
  }
}
console.log('done ->', outdir);
