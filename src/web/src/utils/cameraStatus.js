import { t } from '@/i18n'

const reasonKeys = {
  connection_failed: 'cameraDiagnostics.connectionFailed',
  connection_refused: 'cameraDiagnostics.connectionRefused',
  connection_timeout: 'cameraDiagnostics.connectionTimeout',
  network_unreachable: 'cameraDiagnostics.networkUnreachable',
  invalid_url: 'cameraDiagnostics.invalidUrl',
  unsupported_host: 'cameraDiagnostics.unsupportedHost',
  device_unavailable: 'cameraDiagnostics.deviceUnavailable',
  file_not_found: 'cameraDiagnostics.fileNotFound',
  authentication_failed: 'cameraDiagnostics.authenticationFailed',
  stream_open_failed: 'cameraDiagnostics.streamOpenFailed',
  stream_read_failed: 'cameraDiagnostics.streamReadFailed',
  resolution_unsupported: 'status.unsupportedResolution'
}

export const cameraStatusReason = channel => {
  if (Number(channel?.channelStatus) === 1) return ''
  const key = reasonKeys[channel?.statusReason]
  return key ? t(key) : ''
}
