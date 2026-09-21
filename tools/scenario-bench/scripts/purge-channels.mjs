// purge-channels.mjs — Delete camera channels on the board (bench leftovers).
//
// Why this exists: the engine keeps its own channel accounting. After a killed
// benchmark run the board can still refuse new channels with
//   POST /Camera/AddVideo -> code 10026 "相机数量达到上限"
// even though the DB only holds a handful of rows. Purging the rows is the first
// step; a full reset additionally needs an engine restart.
//
// Usage:
//   node scripts/purge-channels.mjs                       # delete every channel
//   node scripts/purge-channels.mjs --dry-run             # only report the count
//   node scripts/purge-channels.mjs --device http://192.168.111.6:8000
import crypto from 'node:crypto';

function parseArgs(argv) {
  const args = { device: 'http://192.168.111.6:8000', user: 'admin', password: 'admin', dryRun: false };
  for (let i = 2; i < argv.length; i++) {
    if (argv[i] === '--device') args.device = argv[++i];
    else if (argv[i] === '--user') args.user = argv[++i];
    else if (argv[i] === '--password') args.password = argv[++i];
    else if (argv[i] === '--dry-run') args.dryRun = true;
  }
  return args;
}

async function post(base, route, body, token) {
  const headers = { 'Content-Type': 'application/json' };
  if (token) { headers.mtk = token; headers.token = token; }
  const resp = await fetch(base + '/gtw/cwai' + route, { method: 'POST', headers, body: JSON.stringify(body) });
  const text = await resp.text();
  let data = null;
  try { data = JSON.parse(text); } catch { /* non-json */ }
  if (resp.status !== 200) throw new Error('HTTP ' + resp.status + ' on ' + route + ': ' + text.slice(0, 200));
  if (data && data.resCode !== 1) {
    const msg = Array.isArray(data.resMsg) && data.resMsg[0] ? (data.resMsg[0].msgText || data.resMsg[0].msgCode) : 'unknown';
    throw new Error('API error on ' + route + ': ' + msg);
  }
  return { resp, data };
}

const args = parseArgs(process.argv);
const base = args.device.replace(/\/+$/, '');

const pwdHash = crypto.createHash('md5').update(args.password, 'utf8').digest('hex').toUpperCase();
const login = await post(base, '/login/dologin', { account: args.user, pwd: pwdHash });
const token = login.data.resData.mtk;
if (!token) throw new Error('no mtk in login response');
console.log('login ok');

const ids = [];
for (let pageNum = 1; pageNum <= 5; pageNum++) {
  const p = await post(base, '/Camera/Page', { pageNum, pageSize: 100 }, token);
  const rd = p.data.resData || {};
  const rows = Array.isArray(rd) ? rd : (rd.rows ?? rd.list ?? []);
  for (const r of rows) {
    const id = r.videoChannelId ?? r.id;
    if (id) ids.push(id);
  }
  if (rows.length < 100) break;
}
console.log('channels found:', ids.length);

if (args.dryRun) {
  console.log('dry-run, nothing deleted');
  process.exit(0);
}

const CHUNK = 20;
let deleted = 0;
for (let i = 0; i < ids.length; i += CHUNK) {
  const part = ids.slice(i, i + CHUNK);
  const res = await post(base, '/Camera/BatchDelete', { videoChannelIds: part }, token);
  const failed = res.data.resData?.failedList ?? [];
  deleted += part.length - failed.length;
  console.log('deleted %d/%d (failed: %d)', part.length - failed.length, part.length, failed.length);
}
console.log('done, deleted', deleted);
