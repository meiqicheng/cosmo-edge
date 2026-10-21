import assert from 'node:assert/strict'
import { readFileSync } from 'node:fs'
import { resolve, join } from 'node:path'
import { spawnSync } from 'node:child_process'
import { pathToFileURL } from 'node:url'
import { createMarkdownRenderer } from 'vitepress'

export const root = resolve(import.meta.dirname, '..')
const markdown = await createMarkdownRenderer(root)
export function yaml(source) {
  const env = {}
  markdown.render(`---\n${source}\n---\n`, env)
  assert.ok(env.frontmatter && typeof env.frontmatter === 'object', 'YAML frontmatter must parse')
  return env.frontmatter
}
export function compose(file, env = {}) {
  const result = spawnSync('docker', ['compose', '-f', file, 'config', '--format', 'json'], {
    cwd: root, env: { ...process.env, ...env }, encoding: 'utf8'
  })
  assert.equal(result.status, 0, `BLOCKED: Compose V2 config required: ${result.stderr || result.error}`)
  return JSON.parse(result.stdout)
}
const pinned = value => assert.match(value, /^[^\s@]+@sha256:[0-9a-f]{64}$/, 'image must use a valid digest')
export function checkMac(config, jobs, port = '8080') {
  const service = config.services['cosmo-x86-macos']
  assert.equal(service.platform, 'linux/amd64', 'Mac architecture')
  pinned(service.build.args.BUILD_ENV_IMAGE)
  pinned(service.build.args.RUNTIME_BASE_IMAGE)
  if (jobs !== undefined) assert.equal(String(service.build.args.COSMO_BUILD_JOBS), jobs, 'build jobs')
  assert.equal(service.build.args.RESOURCE_DIR, 'data/resource/aiboxresource_x86')
  assert.equal(service.environment.COSMO_STREAM_PLAY_MODE, 'httpflv-srs')
  assert.equal(String(service.environment.COSMO_STREAM_HTTP_PORT), '18088')
  assert.deepEqual(service.healthcheck.test, ['CMD', '/usr/local/bin/cosmo-x86-healthcheck'])
  assert.ok(!service.devices?.length && !service.cap_add?.length && !Object.keys(service.sysctls || {}).length, 'no devices or network privileges')
  assert.ok(!service.privileged && service.network_mode !== 'host', 'no privileged or host network')
  const ports = service.ports
  for (const binding of ports) {
    assert.ok(['127.0.0.1', '::1'].includes(binding.host_ip), 'loopback binding')
    assert.equal(binding.protocol || 'tcp', 'tcp', 'TCP only')
  }
  for (const [target, published] of [[80, port], [1936, '1936'], [1985, '1985'], [18088, '18088']]) {
    assert.ok(ports.some(p => p.target === target && String(p.published) === published), `missing port ${target}`)
  }
  const data = service.volumes.find(v => v.target === '/data/cwaiuserdata')
  const resource = service.volumes.find(v => v.target === '/appfs/cosmo_wander/cwai_data/resource')
  assert.equal(data?.type, 'volume'); assert.equal(resource?.type, 'volume')
  assert.notEqual(data.source, resource.source, 'independent persistent volumes')
  const output = service.volumes.find(v => v.target === '/build_output')
  assert.equal(output?.type, 'bind'); assert.ok(output.source.endsWith('/build_output/macos-x86'))
}
export function checkPackage(config, kind, env = {}) {
  const service = config.services[`cosmo-${kind}-package`]
  assert.equal(service.working_dir, '/workspace')
  if (kind === 'rockchip' && !env.COSMO_ROCKCHIP_BUILDER_IMAGE) pinned(service.image)
  assert.deepEqual(service.entrypoint, kind === 'sophon' ? ['/bin/bash', '/workspace/scripts/build_sophon_package.sh'] : ['/workspace/scripts/build_rockchip_package.sh'])
  assert.deepEqual(service.command, kind === 'sophon' ? [] : ['--chip', env.COSMO_TARGET_CHIP || 'rk3576', '--models', env.COSMO_PACKAGE_MODELS || 'include'])
  for (const [target, type] of [['/workspace', 'bind'], ['/build_output', 'bind']])
    assert.equal(service.volumes.find(v => v.target === target)?.type, type, `mount ${target}`)
  assert.equal(service.volumes.find(v => v.target === '/workspace').source, root, 'workspace bind source')
  assert.equal(service.volumes.find(v => v.target === '/build_output').source, join(root, 'build_output'), 'output bind source')
  const expected = { COSMO_MODEL_GUARD_BUILD_PROFILE: 'public-runtime', COSMO_MODEL_GUARD_SDK_ROOT: '' }
  if (kind === 'sophon') expected.COSMO_PACKAGE_MODELS = 'include'
  else {
    Object.assign(expected, { COSMO_RKNN_ARTIFACT_MANIFEST: '', COSMO_RKNN_MODELS_DIR: '', COSMO_RKNN_RESOURCE_OVERLAY_DIR: '' })
    if (env.COSMO_BUILD_JOBS !== undefined) expected.COSMO_BUILD_JOBS = env.COSMO_BUILD_JOBS
  }
  for (const [key, fallback] of Object.entries(expected)) assert.equal(String(service.environment[key]), env[key] ?? fallback, key)
}
export function readWorkflow(kind) {
  return yaml(readFileSync(join(root, '.github/workflows', kind === 'rockchip' ? 'ci-build-rockchip.yml' : 'nightly-build-test-sophon.yml'), 'utf8'))
}
export function checkComposeContracts() {
  const mac = join(root, 'docker-compose.x86.macos.yml')
  checkMac(compose(mac))
  checkMac(compose(mac, { COSMO_X86_WEB_PORT: '8090', COSMO_X86_BUILD_JOBS: '3' }), '3', '8090')
  const compatibility = compose(join(root, 'docker-compose.rk3576.yml'))
  compatibility.services['cosmo-rockchip-package'] = compatibility.services['cosmo-rk3576-package']
  checkPackage(compatibility, 'rockchip')
  for (const kind of ['sophon', 'rockchip']) {
    const file = join(root, `docker-compose.${kind}.yml`)
    checkPackage(compose(file), kind)
    const env = { COSMO_MODEL_GUARD_BUILD_PROFILE: 'production-release', COSMO_PACKAGE_MODELS: 'preserve', COSMO_TARGET_CHIP: 'rk3576', COSMO_BUILD_JOBS: '7', COSMO_MODEL_GUARD_SDK_ROOT: '/sdk with space', COSMO_RKNN_ARTIFACT_MANIFEST: '/model manifest.json', COSMO_RKNN_MODELS_DIR: '/models', COSMO_RKNN_RESOURCE_OVERLAY_DIR: '/resources' }
    checkPackage(compose(file, env), kind, env)
  }
}
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  checkComposeContracts()
  console.log('Effective Compose contracts passed')
}
