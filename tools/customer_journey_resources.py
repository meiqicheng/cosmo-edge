"""Portable, private customer journey resources (standard library only).

Only local `path` fields are rooted. Remote workspaces, URLs and device IDs are
kept verbatim. Availability checks never imply business/algorithm acceptance.
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
from pathlib import Path, PureWindowsPath
import socket
from concurrent.futures import ThreadPoolExecutor


PROFILE = 'resources.private.json'
MANIFEST = 'manifest.json'
POINTER = 'output/agent-runs/cosmoedge-test-kit/resource-root.local.json'


def native_path(path):
    path = Path(path)
    if os.name == 'nt':
        text = str(path.resolve())
        if not text.startswith('\\\\?\\'):
            text = '\\\\?\\UNC\\' + text[2:] if text.startswith('\\\\') else '\\\\?\\' + text
        return Path(text)
    return path


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def save(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    os.chmod(tmp, 0o600)
    tmp.replace(path)


def within(root, relative):
    text = str(relative)
    if not text or Path(text).is_absolute() or PureWindowsPath(text).is_absolute() or PureWindowsPath(text).drive:
        raise ValueError('Portable resource path must be relative to its root')
    root = Path(root).resolve()
    result = (root / text.replace('\\', '/')).resolve()
    if result == root or root not in result.parents:
        raise ValueError('Portable resource path escapes its root')
    return result


def configured_root(repo):
    value = os.environ.get('COSMO_JOURNEY_RESOURCE_ROOT')
    if not value and (Path(repo) / POINTER).is_file():
        value = read(Path(repo) / POINTER)['root']
    return Path(value).expanduser().resolve() if value else None


def configure(repo, root):
    root = Path(root).resolve()
    profile = read(root / PROFILE)
    manifest = read(root / MANIFEST)
    if profile.get('portableVersion') != 1 or not profile.get('kitId'):
        raise ValueError('Not a portable resource kit')
    if manifest.get('kitId') != profile['kitId']:
        raise ValueError('Resource manifest belongs to a different kit')
    resolve_profile(profile, root)
    save(Path(repo) / POINTER, {'schemaVersion': 1, 'root': str(root), 'kitId': profile['kitId']})
    return {'configured': True, 'root': str(root), 'kitId': profile['kitId']}


def default_profile(repo):
    root = configured_root(repo)
    return root / PROFILE if root else Path(repo) / 'output/agent-runs/cosmoedge-test-kit' / PROFILE


def resolve_profile(profile, root=None):
    profile = copy.deepcopy(profile)
    if profile.get('portableVersion') != 1:
        return profile
    if root is None:
        raise ValueError('Portable resource root is not configured')
    for item in profile.get('resources', {}).values():
        if item.get('path'):
            item['path'] = str(within(root, item['path']))
    for item in profile.get('supplementaryAssets', []):
        item['path'] = str(within(root, item['path']))
    return profile


def root_for_run(repo, meta):
    root = configured_root(repo) or Path(meta['resourceRoot'])
    if read(root / PROFILE).get('kitId') != meta['resourceKitId']:
        raise ValueError('Configured kit differs from the frozen run; configure the original kit')
    return root


def file_hash(path):
    h = hashlib.sha256()
    with native_path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def inspect_manifest(root, full=False):
    root = Path(root).resolve()
    manifest = read(root / MANIFEST)
    if manifest.get('schemaVersion') != 1 or manifest.get('kitId') != read(root / PROFILE).get('kitId'):
        raise ValueError('Invalid resource manifest or kit identity')
    problems, seen = [], set()
    for item in manifest['files']:
        relative = item['path']
        if relative in seen:
            raise ValueError('Duplicate manifest path')
        seen.add(relative)
        path = native_path(within(root, relative))
        reason = None
        if not path.is_file():
            reason = 'missing_file'
        elif path.stat().st_size != item['size']:
            reason = 'size_mismatch'
        elif full and file_hash(path) != item['sha256']:
            reason = 'hash_mismatch'
        if reason:
            problems.append({'path': relative, 'reason': reason, 'resources': item.get('resources', [])})
    return {'mode': 'sha256' if full else 'existence-and-size', 'fileCount': len(seen),
            'totalBytes': sum(x['size'] for x in manifest['files']), 'problems': problems,
            'verified': not problems}


def check_endpoint(resource, host, port, timeout):
    try:
        with socket.create_connection((host, int(port)), timeout=timeout):
            pass
        return {'id': resource, 'port': int(port), 'status': 'reachable',
                'note': 'TCP only; authentication, protocol and business behavior unverified'}
    except (OSError, ValueError, TypeError):
        return {'id': resource, 'port': port, 'status': 'unreachable',
                'note': 'TCP connection failed; check routing, firewall, address and service'}


def inspect_resources(profile, definitions, cases, *, root=None, full=False, network=False):
    resolved = resolve_profile(profile, root)
    integrity = inspect_manifest(root, full) if profile.get('portableVersion') == 1 else None
    inventory = []
    for rid, spec in definitions.items():
        item = resolved.get('resources', {}).get(rid, {})
        state = item.get('status', 'missing')
        reason = ''
        if state == 'available' and spec['kind'] in ('file', 'directory'):
            path = native_path(item.get('path', ''))
            valid = bool(item.get('path')) and (path.is_file() if spec['kind'] == 'file' else path.is_dir())
            if not valid:
                state, reason = 'missing', 'Configured local file/directory does not exist'
            elif path.is_dir() and not any(p.is_file() for p in path.rglob('*')):
                state, reason = 'missing', 'Configured directory contains no files'
        if state == 'available' and any(item.get(k) in (None, '') for k in spec.get('fields', [])):
            state, reason = 'incomplete', 'Required resource configuration fields are empty'
        bad = [p for p in (integrity or {}).get('problems', []) if rid in p['resources']]
        if bad and state == 'available':
            state, reason = 'invalid', f'{len(bad)} packaged file(s) missing or changed; see integrity.problems'
        gap = '' if state in ('available', 'generated', 'not_applicable') else reason or item.get('gap') or spec.get('request', '')
        inventory.append({'id': rid, 'title': spec['title'], 'status': state,
                          'owner': spec.get('owner', '用户/设备管理员'), 'request': gap,
                          'affectedCases': [c['id'] for c in cases if rid in c['requires']],
                          'note': item.get('note', '')})
    endpoints = []
    if network:
        with ThreadPoolExecutor(max_workers=8) as pool:
            jobs = []
            for rid, item in resolved.get('resources', {}).items():
                if item.get('status') != 'available' or not item.get('host'):
                    continue
                # Explicit TCP ports only; SIP UDP cannot be proved by TCP.
                ports = item.get('checkPorts', [])
                if not ports and item.get('port'):
                    ports = [item['port']]
                for port in ports:
                    jobs.append(pool.submit(check_endpoint, rid, item['host'], port, 2))
            endpoints = [job.result() for job in jobs]
        for row in inventory:
            bad = [e for e in endpoints if e['id'] == row['id'] and e['status'] == 'unreachable']
            if bad and row['status'] == 'available':
                row.update(status='unreachable', request='Configured TCP port unavailable; recheck on this machine')
    return {'scope': 'Resource admission only; no device login, writes or business acceptance',
            'resources': inventory, 'gaps': [x for x in inventory if x['request']],
            'integrity': integrity, 'network': endpoints,
            'networkChecked': network,
            'remoteBoundary': 'Remote scripts/samples and device protocols require execution-time verification'}
