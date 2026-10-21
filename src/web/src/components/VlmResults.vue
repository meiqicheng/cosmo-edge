<template>
  <div v-if="results.length" class="vlm-results">
    <div class="vlm-heading">{{ $t('vlmResults.title') }}</div>
    <div v-if="eventId">{{ $t('vlmResults.eventId') }}: {{ eventId }}</div>
    <div v-for="(result, index) in results" :key="`${result.nodeId}:${result.targetId}:${index}`" class="vlm-answer">
      <div>{{ $t('vlmResults.question') }}: {{ result.question || '-' }}</div>
      <div class="answer-text">{{ $t('vlmResults.answer') }}: {{ result.answer || $t('vlmResults.emptyAnswer') }}</div>
      <div>{{ $t('vlmResults.duration') }}: {{ formatDuration(result.durationMs) }}</div>
      <div v-if="result.batchSize > 1">{{ $t('vlmResults.batchSize') }}: {{ result.batchSize }}</div>
      <template v-if="showFrame">
        <div>{{ $t('vlmResults.frameIndex') }}: {{ result.frameIndex || '-' }}</div>
        <div>{{ $t('vlmResults.frameTimestamp') }}: {{ result.timestamp || '-' }}</div>
      </template>
    </div>
  </div>
</template>

<script setup>
defineProps({
  results: { type: Array, default: () => [] },
  showFrame: { type: Boolean, default: false },
  eventId: { type: String, default: '' }
})
const formatDuration = value => typeof value === 'number' && Number.isFinite(value) && value >= 0 ? `${value.toFixed(1)} ms` : '-'
</script>

<style scoped>
.vlm-results { margin: 10px 0; overflow-wrap: anywhere; }
.vlm-heading { font-weight: 600; }
.vlm-answer { padding: 8px 0; line-height: 1.7; font-size: 13px; }
.answer-text { font-weight: 600; white-space: pre-wrap; }
</style>
