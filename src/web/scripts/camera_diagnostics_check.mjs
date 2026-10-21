import assert from 'node:assert/strict'
import { reactive } from 'vue'
import { mountComponent } from './helpers/mount_behavior_component.mjs'

const channel = reactive({ channelStatus: 0, statusReason: 'connection_refused', algorithms: [] })
const page = await mountComponent('views/gam/taskManager/components/chanelDetailDialog.vue', {
  props: { visible: true, detailChannel: channel }
})
try {
  const text = () => page.all(n => n.text).map(n => n.text).join(' ')
  assert.ok(text().includes('cameraDiagnostics.connectionRefused'))
  channel.statusReason = 'stream_open_failed'
  await page.settle()
  assert.ok(text().includes('cameraDiagnostics.streamOpenFailed'))
  assert.ok(!text().includes('cameraDiagnostics.connectionRefused'))
  channel.channelStatus = 1
  await page.settle()
  assert.ok(!text().includes('cameraDiagnostics.'), 'online source hides stale failure')
  channel.channelStatus = 0
  delete channel.statusReason
  await page.settle()
  assert.ok(!text().includes('cameraDiagnostics.'), 'legacy response has no invented reason')
  channel.statusReason = 'unrecognized server error with private URL'
  await page.settle()
  assert.ok(!text().includes('private URL'), 'only known diagnostic codes are displayed')
} finally { page.unmount() }
console.log('Camera diagnostics display and recovery checks passed')
