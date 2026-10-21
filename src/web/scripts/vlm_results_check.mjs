import assert from 'node:assert/strict'
import { h, reactive } from 'vue'
import { mountComponent } from './helpers/mount_behavior_component.mjs'

for (const answer of ['是', '否', 'uncertain', '<script>example</script>']) {
  const panel = await mountComponent('components/VlmResults.vue', {
    props: { results: [{ nodeId: 'vlm', targetId: 'vlm:0', question: 'Is a person visible?', answer, durationMs: 12.5 }] }
  })
  try {
    const text = panel.all(node => node.text).map(node => node.text).join(' ')
    assert.ok(text.includes('Is a person visible?'))
    assert.ok(text.includes(answer), 'answers must be visible regardless of business selection')
    assert.ok(text.includes('12.5 ms'))
    assert.equal(panel.all(node => node.type === 'script').length, 0, 'model text must never be rendered as HTML')
  } finally { panel.unmount() }
}
console.log('VLM answer display checks passed')

const event = reactive({ visible: false, detailData: {} })
const details = await mountComponent('views/box/eventQuery/components/detailDialog.vue', {
  mocks: {
    moment: { default: () => ({ format: () => 'event-time' }) },
    '@/components/ImageLoader.vue': { default: { render: () => null } }
  },
  wrap: component => ({ render: () => h(component, event) })
})
try {
  const text = () => details.all(n => n.text).map(n => n.text).join(' ')
  for (const property of ['', 'invalid-json', '{}']) {
    event.detailData = { property }
    event.visible = true
    await details.settle()
    assert.ok(!text().includes('vlmResults.title'), 'old events must remain readable without a VLM panel')
  }
  event.detailData = { id: 'event-123', property: JSON.stringify({ vlmResults: [{
    question: 'Is a person visible?', answer: 'yes', durationMs: 42.5, batchSize: 2,
    frameIndex: 'frame-456', timestamp: '1792259129459'
  }] }) }
  await details.settle()
  for (const value of ['event-123', 'frame-456', '1792259129459', 'Is a person visible?', 'yes', '42.5 ms', 'vlmResults.batchSize']) {
    assert.ok(text().includes(value), `event details must display ${value}`)
  }
} finally { details.unmount() }
console.log('VLM event detail checks passed')
