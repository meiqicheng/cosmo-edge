#!/usr/bin/env node
// Probe current device channel/task state (read-only) for the RKLLM run.
import { CosmoClient } from './src/cosmo-client.js';

const base = process.env.DEV_BASE ?? 'http://192.168.112.196:8000';
const client = new CosmoClient({ base, user: 'admin', password: 'admin123' });
await client.login();

const res = await client.cameraPage({ pageNum: 1, pageSize: 200 });
const list = res?.rows ?? res?.list ?? res?.data ?? [];
console.log('channelCount=' + list.length);
for (const ch of list) {
  console.log('=== CHANNEL RAW ===');
  console.log(JSON.stringify(ch));
}

const hw = await client.queryHardwareResource();
console.log('=== HARDWARE ===');
console.log(JSON.stringify(hw, null, 1).slice(0, 3000));

