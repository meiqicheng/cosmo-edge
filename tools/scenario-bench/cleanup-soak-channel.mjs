#!/usr/bin/env node
// Remove leftover soak-test channel(s) so ladder runs start from a clean slate.
// Usage: node cleanup-soak-channel.mjs --device http://192.168.112.196:8000 [--delete-all-bench]
// Read-only unless --apply is passed.

import { CosmoClient } from './src/cosmo-client.js';

const args = process.argv.slice(2);
function argOf(name) {
  const i = args.indexOf(name);
  return i >= 0 ? args[i + 1] : undefined;
}
const base = argOf('--device') || 'http://192.168.112.196:8000';
const apply = args.includes('--apply');

const client = new CosmoClient({ base, user: 'admin', password: 'admin123' });
await client.login();

const page = await client.cameraPage({ pageNum: 1, pageSize: 100 });
const rows = page?.rows ?? page?.list ?? [];
console.log(`channels on device: ${rows.length}`);
for (const r of rows) {
  const tasks = (r.tasks ?? []).map((t) => ({
    id: t.id,
    algorithmId: t.algorithmId,
    name: t.algorithmName ?? t.name,
    enableStatus: t.enableStatus,
    status: t.status,
    scheduleId: t.scheduleId,
  }));
  console.log(
    JSON.stringify({
      videoChannelId: r.videoChannelId ?? r.id,
      name: r.channelName ?? r.name,
      url: r.videoUrl ?? r.url,
      channelStatus: r.channelStatus,
      tasks,
    }),
  );
}

if (!apply) {
  console.log('\n[dry-run] pass --apply to switch off enabled tasks and delete these channels.');
  process.exit(0);
}

for (const r of rows) {
  const chId = r.videoChannelId ?? r.id;
  const enabledTasks = (r.tasks ?? []).filter((t) => t.enableStatus === 1 || t.enable === 1);
  if (enabledTasks.length > 0) {
    const switches = enabledTasks.map((t) => ({
      id: t.id,
      channelId: chId,
      algorithmId: t.algorithmId,
      enable: false,
    }));
    const sw = await client.taskBatchSwitch(switches);
    console.log(`switched off ${switches.length} task(s) on ${chId}: failedList=${JSON.stringify(sw.failedList)}`);
  }
  const del = await client.cameraBatchDelete([chId]);
  console.log(`deleted channel ${chId}: ${JSON.stringify(del?.resData ?? del ?? {})}`);
}

const after = await client.cameraPage({ pageNum: 1, pageSize: 100 });
const afterRows = after?.rows ?? after?.list ?? [];
console.log(`\nchannels remaining: ${afterRows.length}`);
const hw = await client.queryHardwareResource();
console.log('hardware snapshot:', JSON.stringify(hw).slice(0, 2000));
