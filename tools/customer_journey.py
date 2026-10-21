#!/usr/bin/env python3
"""Case-driven Codex test ledger. No implicit network or device mutations.

The agent performs UI actions; this tool freezes the suite, supplies the queue,
validates evidence-backed outcomes and renders reports. It is not a browser bot.
"""
from __future__ import annotations

import argparse
import collections
import datetime as dt
import hashlib
import html
import json
import os
from pathlib import Path
import re
import sys
import shutil

try:
    from customer_journey_workbook import compile_workbook, workbook_cells, sha256, export_results
    import customer_journey_resources as resource_kit
    from customer_journey_quick import compile_quick
except ModuleNotFoundError:
    from tools.customer_journey_workbook import compile_workbook, workbook_cells, sha256, export_results
    from tools import customer_journey_resources as resource_kit
    from tools.customer_journey_quick import compile_quick

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SUITE = ROOT / 'test/customer-journey/CosmoEdge-客户旅程测试用例.xlsx'
GENERATED_SUITE = ROOT / 'test/customer-journey/suite.json'
DEFAULT_PROFILE = ROOT / 'output/agent-runs/cosmoedge-test-kit/resources.private.json'
LABELS = {'PASS': '通过', 'FAIL': '失败', 'BLOCKED': '阻塞',
          'PARTIAL': '部分完成', 'NOT_EXECUTED': '未执行', 'NOT_APPLICABLE': '不适用'}
REASONS = {'PRODUCT_DEFECT', 'RESOURCE_MISSING', 'RESOURCE_INVALID', 'CONFIGURATION',
           'AUTHORIZATION', 'HARDWARE_REQUIRED', 'GROUND_TRUTH', 'ACCEPTANCE_UNDEFINED',
           'TOOL_FAILURE', 'INCOMPLETE', 'NOT_APPLICABLE', 'LEGACY_EVIDENCE', 'NONE'}


def read(path):
    return json.loads(Path(path).read_text(encoding='utf-8-sig'))


def load_suite(path):
    if Path(path).suffix.lower() == '.xlsx':
        if '主要用例' in workbook_cells(path):
            suite = compile_quick(path)
        else:
            suite = compile_workbook(path)
    else:
        suite = read(path)
    validate_suite(suite)
    return suite


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    tmp = path.with_suffix(path.suffix + '.tmp')
    tmp.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    os.chmod(tmp, 0o600)
    tmp.replace(path)


def now():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec='seconds')


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False).encode()).hexdigest()


def private_run(path):
    path = Path(path).resolve()
    base = (ROOT / 'output/agent-runs').resolve()
    if path == base or base not in path.parents:
        raise ValueError('Run directory must be inside ignored output/agent-runs/')
    return path


def validate_suite(suite):
    if suite.get('schemaVersion') != 1 or not suite.get('version'):
        raise ValueError('Suite schemaVersion/version required')
    ids = set()
    resources = set(suite['resourceDefinitions'])
    for case in suite['cases']:
        cid = case['id']
        pattern = r'Q\d{2}' if suite.get('suiteKind') == 'quick' else r'[A-Z]+-\d+'
        if not re.fullmatch(pattern, cid) or cid in ids:
            raise ValueError('Invalid or duplicate case ID: ' + cid)
        ids.add(cid)
        for key in ('title', 'module', 'preconditions', 'expected', 'cleanup', 'source', 'revision'):
            if not case.get(key):
                raise ValueError(cid + ': missing ' + key)
        if not case['steps'] or not case['assertions']:
            raise ValueError(cid + ': empty steps/assertions')
        for kind in ('steps', 'assertions'):
            subids = [s['id'] for s in case[kind]]
            if len(set(subids)) != len(subids) or any(not s['text'].strip() for s in case[kind]):
                raise ValueError(cid + ': invalid ' + kind)
        if set(case['requires']) - resources:
            raise ValueError(cid + ': unknown resource IDs')
    return len(ids)


def init_run(run, suite_path, profile_path):
    run = private_run(run)
    if (run / 'run.json').exists():
        raise ValueError('Run already exists; resume it or choose a new run ID')
    suite = load_suite(suite_path)
    profile = read(profile_path)
    if not profile.get('resources', {}).get('DUT'):
        raise ValueError('Private profile requires DUT')
    run.mkdir(parents=True, exist_ok=True, mode=0o700)
    write(run / 'suite.snapshot.json', suite)
    metadata = {'schemaVersion': 1, 'createdAt': now(),
                            'suiteVersion': suite['version'], 'suiteSha256': digest(suite),
                            'profilePath': str(Path(profile_path).resolve()),
                            'profileSha256': digest(profile), 'deviceVersion': 'UNVERIFIED'}
    if profile.get('portableVersion') == 1:
        root = Path(profile_path).resolve().parent
        if not (root / resource_kit.MANIFEST).is_file():
            root = resource_kit.configured_root(ROOT)
        if root is None or read(root / resource_kit.PROFILE).get('kitId') != profile['kitId']:
            raise ValueError('Configure the matching resource kit before using a copied profile')
        resource_kit.resolve_profile(profile, root)
        write(run / 'resources.private.json', profile)
        metadata.update(profilePath='resources.private.json', profileInRun=True,
                        resourceRoot=str(root), resourceKitId=profile['kitId'])
    if Path(suite_path).suffix.lower() == '.xlsx':
        snapshot = run / 'casebook.snapshot.xlsx'
        shutil.copyfile(suite_path, snapshot)
        metadata.update(workbookSnapshot=snapshot.name, workbookSha256=sha256(snapshot))
        if metadata['workbookSha256'] != suite['generatedFrom']['sha256']:
            raise ValueError('Workbook changed during initialization; retry in a new run')
    write(run / 'run.json', metadata)
    write(run / 'results.json', {c['id']: {'status': 'NOT_EXECUTED', 'reasonCode': 'INCOMPLETE',
          'reason': '尚未执行，不能判为通过。', 'actual': '', 'evidence': [],
          'steps': [], 'assertions': [], 'attempts': []} for c in suite['cases']})
    checked = preflight(run)
    return {'run': str(run), 'caseCount': len(suite['cases']), 'suiteVersion': suite['version'],
            'resourceGapCount': len(checked['gaps']), 'preflight': str(run / 'preflight.json')}


def load_run(run):
    run = private_run(run)
    meta = read(run / 'run.json')
    suite = read(run / 'suite.snapshot.json')
    if digest(suite) != meta['suiteSha256']:
        raise ValueError('Frozen suite changed; create a new run for a new suite')
    if meta.get('workbookSnapshot'):
        snapshot = (run / meta['workbookSnapshot']).resolve()
        if snapshot.parent != run or not snapshot.is_file() or sha256(snapshot) != meta['workbookSha256']:
            raise ValueError('Frozen XLSX changed or is missing; restore the original snapshot')
    results = read(run / 'results.json')
    if set(results) != {c['id'] for c in suite['cases']}:
        raise ValueError('Result IDs must exactly equal frozen suite IDs')
    return run, meta, suite, results


def profile_file(meta, run=None):
    if meta.get('profileInRun'):
        if run is None:
            raise ValueError('Run path required for portable profile')
        return resource_kit.within(run, meta['profilePath'])
    return Path(meta['profilePath'])


def load_profile(meta, run=None, resolve=True):
    profile = read(profile_file(meta, run))
    if digest(profile) != meta['profileSha256']:
        raise ValueError('Profile changed. Use refresh-profile to record the change before continuing')
    if resolve and profile.get('portableVersion') == 1:
        return resource_kit.resolve_profile(profile, resource_kit.root_for_run(ROOT, meta))
    return profile


def refresh_profile(run):
    run, meta, _, _ = load_run(run)
    profile = read(profile_file(meta, run))
    meta.setdefault('profileChanges', []).append({'at': now(), 'previousSha256': meta['profileSha256']})
    meta['profileSha256'] = digest(profile)
    write(run / 'run.json', meta)


def set_device_version(run, version, evidence):
    run, meta, _, _ = load_run(run)
    if not version.strip() or version == 'UNVERIFIED':
        raise ValueError('Provide the measured device version')
    check_evidence(run, [evidence])
    meta.setdefault('versionHistory', []).append({'previous': meta['deviceVersion'], 'version': version, 'at': now(), 'evidence': evidence})
    meta['deviceVersion'] = version
    write(run / 'run.json', meta)
    return {'deviceVersion': version}


def preflight(run, full=False, network=False):
    run, meta, suite, _ = load_run(run)
    profile = load_profile(meta, run, resolve=False)
    root = resource_kit.root_for_run(ROOT, meta) if profile.get('portableVersion') == 1 else None
    result = resource_kit.inspect_resources(profile, suite['resourceDefinitions'], suite['cases'],
                                            root=root, full=full, network=network)
    result['at'] = now()
    write(run / 'preflight.json', result)
    return result


def queue(run, limit):
    run, meta, suite, results = load_run(run)
    load_profile(meta, run)
    rows = []
    for case in suite['cases']:
        r = results[case['id']]
        if r['status'] in ('NOT_EXECUTED', 'PARTIAL'):
            rows.append({'case': case, 'currentResult': r,
                         'privateResourceFile': str(profile_file(meta, run)),
                         'resourceRoot': str(resource_kit.root_for_run(ROOT, meta)) if meta.get('resourceKitId') else None,
                         'instruction': 'Read only this case resources; execute all steps, assert, clean up, then record.'})
            if len(rows) >= limit:
                break
    return rows


def check_evidence(run, refs):
    if not refs:
        raise ValueError('Measured result requires at least one evidence file')
    for ref in refs:
        path = (run / ref).resolve()
        if Path(ref).is_absolute() or run not in path.parents or not path.is_file():
            raise ValueError('Evidence must be an existing relative file inside this run: ' + str(ref))


def validate_analysis(run, analysis):
    for key in ('category', 'cause', 'certainty', 'remaining', 'nextAction', 'owner'):
        if not isinstance(analysis.get(key), str) or not analysis[key].strip():
            raise ValueError('Explanation requires ' + key)
    if analysis['certainty'] not in ('CONFIRMED', 'PARTLY_CONFIRMED', 'UNKNOWN'):
        raise ValueError('Explanation certainty must distinguish confirmed facts from unknown causes')
    check_evidence(run, analysis.get('evidence'))


def explain(run, cid, analysis):
    """Annotate a result without inventing a new test or overwriting its evidence."""
    run, _, _, results = load_run(run)
    if cid not in results: raise ValueError('Unknown case ID')
    validate_analysis(run, analysis)
    result = results[cid]
    if result.get('analysis'):
        result.setdefault('analysisHistory', []).append(result['analysis'])
    result['analysis'] = dict(analysis, reviewedAt=now())
    write(run / 'results.json', results)
    return {'caseId': cid, 'status': result['status'], 'newMeasurement': False}


def validate_result(run, case, result):
    status = result.get('status')
    if status not in LABELS:
        raise ValueError('Unknown status')
    if result.get('reasonCode') not in REASONS or not result.get('reason'):
        raise ValueError('reasonCode and specific reason required')
    if result.get('analysis'):
        validate_analysis(run, result['analysis'])
    if status != 'NOT_EXECUTED' and not result.get('actual'):
        raise ValueError('actual observation required')
    if status in ('PASS', 'FAIL', 'PARTIAL'):
        check_evidence(run, result.get('evidence'))
    for kind in ('steps', 'assertions'):
        expected = {s['id'] for s in case[kind]}
        rows = result.get(kind, [])
        if len({s['id'] for s in rows}) != len(rows) or {s['id'] for s in rows} - expected:
            raise ValueError('Unknown/duplicate ' + kind)
        for row in rows:
            if row.get('status') not in ('PASS', 'FAIL', 'BLOCKED', 'NOT_EXECUTED', 'NOT_APPLICABLE'):
                raise ValueError('Invalid step status')
            if row['status'] in ('PASS', 'FAIL'):
                if not row.get('actual'):
                    raise ValueError('Measured steps need actual observations')
                check_evidence(run, row.get('evidence'))
            if row['status'] == 'NOT_APPLICABLE':
                if not row.get('actual'):
                    raise ValueError('Not-applicable checks need a concrete applicability explanation')
                check_evidence(run, row.get('evidence'))
        accepted = ('PASS', 'NOT_APPLICABLE')
        if status == 'PASS' and (expected != {s['id'] for s in rows} or any(s['status'] not in accepted for s in rows)):
            raise ValueError('PASS requires every step and assertion to PASS or have evidenced non-applicability')
    if status == 'PASS':
        if not any(s['status'] == 'PASS' for s in result.get('steps', [])):
            raise ValueError('PASS requires at least one measured step; use NOT_APPLICABLE for an inapplicable case')
        cleanup = result.get('cleanup', {})
        if cleanup.get('status') not in ('VERIFIED', 'NOT_REQUIRED') or not cleanup.get('note'):
            raise ValueError('PASS requires verified cleanup or an explicit read-only explanation')
        if cleanup['status'] == 'VERIFIED':
            check_evidence(run, cleanup.get('evidence'))
    if status == 'FAIL' and not any(s['status'] == 'FAIL' for key in ('steps', 'assertions') for s in result.get(key, [])):
        raise ValueError('FAIL must identify a failed step/assertion')
    if status == 'NOT_APPLICABLE':
        check_evidence(run, result.get('evidence'))
        if result['reasonCode'] != 'NOT_APPLICABLE':
            raise ValueError('Not applicable is a capability decision, not a resource shortage')


def record(run, cid, result):
    run, meta, suite, results = load_run(run)
    load_profile(meta, run)
    case = next((c for c in suite['cases'] if c['id'] == cid), None)
    if case is None:
        raise ValueError('Unknown case ID')
    validate_result(run, case, result)
    result = dict(result, at=now(), provenance='measured-case-record', deviceVersion=meta['deviceVersion'])
    previous = results[cid]
    attempts = previous.get('attempts', [])
    if previous['status'] != 'NOT_EXECUTED':
        attempts = attempts + [{k: v for k, v in previous.items() if k != 'attempts'}]
    results[cid] = dict(result, attempts=attempts)
    write(run / 'results.json', results)
    report(run)
    return {'caseId': cid, 'status': result['status'], 'previousAttempts': len(attempts)}


def result_template(run, cid, output):
    run, _, suite, _ = load_run(run)
    case = next(c for c in suite['cases'] if c['id'] == cid)
    value = {'status': 'NOT_EXECUTED', 'reasonCode': 'INCOMPLETE', 'reason': '待实测',
             'actual': '', 'evidence': [], 'cleanup': {'status': 'PENDING', 'note': '', 'evidence': []}}
    for kind in ('steps', 'assertions'):
        value[kind] = [{'id': x['id'], 'status': 'NOT_EXECUTED', 'actual': '', 'evidence': []} for x in case[kind]]
    output = Path(output).resolve()
    if run not in output.parents:
        raise ValueError('Result template must stay in current run')
    write(output, value)


def migrate(run, source):
    """One-time import; no invented step results and never overwrites measurements."""
    run, meta, suite, results = load_run(run)
    source = Path(source).resolve()
    imported = read(source / 'case-results.json')
    if any(r['status'] != 'NOT_EXECUTED' for r in results.values()):
        raise ValueError('Legacy import only allowed into an untouched run')
    import shutil
    refs = {name for c in imported for name in c.get('evidence', [])}
    refs |= {'case-results.json', 'execution-summary.json', 'findings.json', 'cleanup-check.json',
             'precision-full.json', 'precision-retry.json', 'precision-effective.json'}
    target = run / 'evidence/legacy'
    target.mkdir(parents=True, exist_ok=True)
    for name in refs:
        if Path(name).name != name:
            raise ValueError('Unsafe legacy reference')
        if (source / name).is_file():
            shutil.copyfile(source / name, target / name)
    for old in imported:
        cid = old['id']
        if cid not in results:
            raise ValueError('Legacy case missing from suite: ' + cid)
        actual = '；'.join(old['observations'])
        reason = actual
        if old['status'] == 'NOT_EXECUTED' and actual == '本轮未执行该完整用例；无通过证据，需后续补测。':
            reason = '上轮未按用例队列执行到本项，尚无实测结论；这是执行覆盖缺口，不是已确认的产品故障或资源阻塞。'
        results[cid] = {'status': old['status'], 'reasonCode': 'LEGACY_EVIDENCE',
                       'reason': reason if old['status'] != 'PASS' else '按上轮该用例的已有记录通过；本次只迁移报告，未重新测试。',
                       'actual': actual, 'evidence': ['evidence/legacy/' + n for n in old.get('evidence', []) if (target / n).is_file()],
                       'steps': [], 'assertions': [], 'attempts': [], 'at': now(),
                       'provenance': 'legacy-case-summary',
                       'note': '历史记录没有逐步骤结论，不补造；保留原状态。NOT_EXECUTED表示上一轮没有执行，不等于资源缺失。'}
    meta['legacyImport'] = {'at': now(), 'caseCount': len(imported), 'newMeasurement': False}
    summary = read(source / 'execution-summary.json')
    meta['deviceVersion'] = summary.get('version', 'UNVERIFIED')
    write(run / 'results.json', results)
    write(run / 'run.json', meta)
    return {'imported': len(imported), 'newCasesNotExecuted': len(suite['cases']) - len(imported)}


CSS = '''body{font:15px/1.55 system-ui,"Microsoft YaHei",sans-serif;color:#1b2b42;margin:0;background:#f4f7fb}main{max-width:1450px;margin:auto;padding:24px}h1{font-size:27px;margin:0 0 12px}header,section{background:white;border:1px solid #dce4ef;border-radius:10px;padding:20px;margin-bottom:18px}.cards{display:flex;gap:10px;flex-wrap:wrap}.card{background:#f2f5fa;border-radius:7px;padding:12px 18px}.card b{display:block;font-size:25px}.filters{display:flex;gap:8px;flex-wrap:wrap;margin:16px 0;position:sticky;top:0;background:white;padding:8px;z-index:1}input,select,button{font:inherit;padding:8px;border:1px solid #bac8da;border-radius:5px}input{min-width:240px}button{cursor:pointer;background:#eef3fb}table{width:100%;border-collapse:collapse;font-size:14px}th,td{text-align:left;border-bottom:1px solid #e1e7ef;padding:10px;vertical-align:top}th{background:#eef3fa;white-space:nowrap}td{overflow-wrap:anywhere}.scroll{overflow:auto}.pill{display:inline-block;white-space:nowrap;border-radius:5px;padding:3px 8px;background:#e8edf5}.PASS{color:#12603a;background:#dcf5e8}.FAIL{color:#9e2020;background:#ffe4e4}.BLOCKED,.PARTIAL{color:#7b4a00;background:#fff0ce}.NOT_EXECUTED{color:#556479}.notice{background:#fff4d9;padding:12px;border-left:4px solid #d89114}details summary{cursor:pointer;color:#145db0}pre{white-space:pre-wrap;font:inherit}small{color:#58677b}a{color:#145db0}.detail td{background:#fafcff}dl{display:grid;grid-template-columns:110px 1fr;gap:8px}dt{font-weight:600}dd{margin:0}.detail table{font-size:13px}.reason{min-width:260px}.caseid{white-space:nowrap}@media(max-width:760px){main{padding:10px}header,section{padding:12px}h1{font-size:22px}.card{padding:8px}dl{display:block}dt{margin-top:8px}.reason{min-width:220px}}@media print{.filters{display:none}.scroll{overflow:visible}main{padding:0}}'''


def report(run, xlsx=False):
    run, meta, suite, results = load_run(run)
    labels = LABELS
    esc = lambda v: html.escape(str(v))
    counts = collections.Counter(r['status'] for r in results.values())
    total = len(suite['cases'])
    complete = all(r['status'] in ('PASS', 'FAIL', 'NOT_APPLICABLE') and r.get('provenance') != 'legacy-case-summary' for r in results.values())
    all_pass = complete and not counts['FAIL']
    summary = {'suiteVersion': suite['version'], 'suiteSha256': meta['suiteSha256'], 'cases': total,
               'counts': dict(counts), 'fullyExecuted': complete, 'allPass': all_pass,
               'legacyImport': meta.get('legacyImport'), 'generatedAt': now()}
    unexplained = [cid for cid, r in results.items() if r['status'] not in ('PASS', 'NOT_APPLICABLE') and not r.get('analysis')]
    summary['unexplainedCases'] = unexplained
    summary['explanationsComplete'] = not unexplained
    summary['unknownCauseCases'] = [cid for cid, r in results.items() if r.get('analysis', {}).get('certainty') == 'UNKNOWN']
    summary['causeCounts'] = dict(collections.Counter(r['analysis']['category'] for r in results.values() if r.get('analysis') and r['status'] not in ('PASS', 'NOT_APPLICABLE')))
    write(run / 'summary.json', summary)
    def links(refs):
        return ' · '.join('<a href="' + esc(n) + '">' + esc(Path(n).name) + '</a>' for n in refs)
    parts = ['<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>CosmoEdge 逐用例测试报告</title><style>' + CSS + '</style><main><header><h1>CosmoEdge 逐用例测试报告</h1>',
             '<p>用例库 ' + esc(suite['version']) + ' · 设备版本 ' + esc(meta['deviceVersion']) + ' · ' + str(total) + ' 条用例</p>',
             '<div class="cards">' + ''.join('<div class="card">' + labels[k] + '<b>' + str(counts[k]) + '</b></div>' for k in labels) + '</div>',
             '<p class="notice">' + ('全部适用用例通过。' if all_pass else '全量验收尚未通过。失败、阻塞、部分完成和未执行均不能算通过。') +
             (' 本报告是历史实测记录的重新编排，未重新测试设备；历史记录缺失的逐步骤证据不补造。' if meta.get('legacyImport') else '') + '</p>',
             '<p class="notice">未补齐原因分析的未通过用例：' + str(len(unexplained)) + '。原因未知必须明确标注；资源清单缺项不自动等于该用例停止的原因。</p>',
             '<p>历史停止原因仍未证实：' + str(len(summary['unknownCauseCases'])) + ' 条。补充说明和补测计划不等于已经补测或定位根因。</p>',
             '<p>通过：全部步骤和验收检查符合预期。失败：实际行为不符合预期。阻塞：具体前置条件未满足。部分完成：有实测但仍有未验步骤。未执行：尚未运行。每条结果都保留原因与证据。</p></header>',
             '<section><h2>逐用例结果</h2><div class="filters"><input id="query" aria-label="搜索用例" placeholder="编号、功能、实际结果、原因"><select id="status" aria-label="状态"><option value="">全部状态</option>' + ''.join('<option value="' + k + '">' + v + '</option>' for k, v in labels.items()) + '</select><select id="module" aria-label="模块"><option value="">全部模块</option>' + ''.join('<option>' + esc(m) + '</option>' for m in dict.fromkeys(c['module'] for c in suite['cases'])) + '</select><button id="notpass">仅看未通过</button><button id="reset">重置</button><span id="visibleCount"></span></div><div class="scroll"><table id="cases"><thead><tr><th>编号 / 优先级</th><th>模块 / 用例</th><th>结果</th><th>实际结果 / 未通过原因</th><th>步骤、预期与证据</th></tr></thead><tbody>']
    for c in suite['cases']:
        r = results[c['id']]
        status = r['status']
        step_rows = []
        for kind, title in (('steps', '操作步骤'), ('assertions', '验收检查')):
            actual = {x['id']: x for x in r.get(kind, [])}
            for s in c[kind]:
                a = actual.get(s['id'], {})
                step_rows.append('<tr><td>' + esc(s['id']) + '<br>' + title + '</td><td>' + esc(s['text']) + '</td><td>' + esc(labels.get(a.get('status'), '未记录')) + '</td><td>' + esc(a.get('actual', '历史记录未拆分步骤' if r.get('provenance') == 'legacy-case-summary' else '')) + '<br>' + links(a.get('evidence', [])) + '</td></tr>')
        cleanup_note = r.get('cleanup', {}).get('status', '未单独记录') + ' ' + r.get('cleanup', {}).get('note', '')
        analysis = r.get('analysis')
        explanation = ''
        if analysis:
            explanation = '<dl>' + ''.join('<dt>' + k + '</dt><dd><pre>' + esc(v) + '</pre></dd>' for k, v in [('原因分类', analysis['category']), ('证实程度', analysis['certainty']), ('原因分析', analysis['cause']), ('尚缺检查', analysis['remaining']), ('下一步', analysis['nextAction']), ('由谁处理', analysis['owner']), ('条件核对', analysis.get('conditions', '')), ('用例改进', analysis.get('caseDesign', ''))]) + '</dl><p>分析依据：' + links(analysis['evidence']) + '</p>'
        detail = '<dl>' + ''.join('<dt>' + k + '</dt><dd><pre>' + esc(v) + '</pre></dd>' for k, v in [('前置条件', c['preconditions']), ('资源编号', '、'.join(c['requires'])), ('预期结果', c['expected']), ('清理要求', c['cleanup']), ('清理实测', cleanup_note), ('原因类别', r.get('reasonCode', '')), ('记录说明', r.get('note', '')), ('来源', c['source'])]) + '</dl>'
        detail += explanation + '<table><thead><tr><th>步骤</th><th>操作 / 预期</th><th>结果</th><th>实际与证据</th></tr></thead><tbody>' + ''.join(step_rows) + '</tbody></table><p>用例证据：' + (links(r.get('evidence', [])) or '暂无') + '</p>'
        if r.get('attempts'):
            detail += '<p>历史尝试：' + esc('；'.join(x.get('at', '') + ' ' + labels[x['status']] + ' ' + x.get('reason', '') for x in r['attempts'])) + '</p>'
        reason = analysis['cause'] if analysis else r['reason']
        action = '<p><b>待补：</b>' + esc(analysis['remaining']) + '</p><p><b>下一步 / 责任方：</b>' + esc(analysis['nextAction'] + ' / ' + analysis['owner']) + '</p>' if analysis else ''
        parts.append('<tr data-status="' + status + '" data-module="' + esc(c['module']) + '"><td class="caseid">' + esc(c['id']) + '<br>' + esc(c['priority']) + '</td><td>' + esc(c['module']) + '<br><b>' + esc(c['title']) + '</b></td><td><span class="pill ' + status + '">' + labels[status] + '</span></td><td class="reason">' + esc(r.get('actual') or '尚无实测结果') + '<p><b>' + ('结论依据：' if status == 'PASS' else '未通过原因：') + '</b>' + esc(reason) + '</p>' + action + '</td><td><details><summary>展开详情</summary>' + detail + '</details></td></tr>')
    parts.append('</tbody></table></div></section>')
    if (run / 'preflight.json').exists():
        pf = read(run / 'preflight.json')
        parts.append('<section><h2>一次性资源缺口清单</h2><p>配置齐全不等于远端服务已通过；运行时还需核验可达性、协议能力和素材真值。缺口只阻塞所列用例，不替代其余用例的执行。</p><table><tr><th>资源</th><th>需要准备</th><th>影响用例</th></tr>')
        for gap in pf['gaps']:
            parts.append('<tr><td>' + esc(gap['id']) + '<br>' + esc(gap['title']) + '</td><td>' + esc(gap['request']) + '<br><small>' + esc(gap['owner']) + '</small></td><td>' + esc('、'.join(gap['affectedCases'])) + '</td></tr>')
        parts.append('</table></section>')
    parts.append('<section><h2>报告与执行记录</h2><p>' + links(['suite.snapshot.json', 'results.json', 'summary.json'] + ([meta['workbookSnapshot']] if meta.get('workbookSnapshot') else [])) + '</p><p>报告不包含资源配置中的密码；完整私有资源表由执行技能按编号读取。精度视频结果是 ACC / ALG 用例的证据，不另作功能通过数。</p></section></main>')
    parts.append('''<script>const q=document.getElementById('query'),s=document.getElementById('status'),m=document.getElementById('module'),rows=[...document.querySelectorAll('#cases tbody > tr[data-status]')];let only=false;function filter(){let n=0;rows.forEach(r=>{let show=(!s.value||r.dataset.status===s.value)&&(!m.value||r.dataset.module===m.value)&&(!only||!['PASS','NOT_APPLICABLE'].includes(r.dataset.status))&&r.textContent.toLowerCase().includes(q.value.toLowerCase());r.hidden=!show;if(show)n++});document.getElementById('visibleCount').textContent=n+' / '+rows.length+' 条'}q.oninput=s.onchange=m.onchange=filter;document.getElementById('notpass').onclick=()=>{only=true;s.value='';filter()};document.getElementById('reset').onclick=()=>{only=false;q.value=s.value=m.value='';filter()};filter();</script></html>''')
    (run / 'report.html').write_text(''.join(parts), encoding='utf-8')
    if xlsx:
        exported = export_results(run, suite, meta)
        summary['xlsxReport'] = exported['output']
        write(run / 'summary.json', summary)
        path = run / 'report.html'
        content = path.read_text(encoding='utf-8').replace('<h2>报告与执行记录</h2>', '<h2>报告与执行记录</h2><p>' + links([Path(exported['output']).name]) + '</p>')
        path.write_text(content, encoding='utf-8')
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('validate'); p.add_argument('--suite', default=str(DEFAULT_SUITE))
    p = sub.add_parser('compile'); p.add_argument('--workbook', default=str(DEFAULT_SUITE)); p.add_argument('--output', default=str(GENERATED_SUITE)); p.add_argument('--check', action='store_true')
    p = sub.add_parser('init'); p.add_argument('--run', required=True); p.add_argument('--suite', default=str(DEFAULT_SUITE)); p.add_argument('--profile', default=str(resource_kit.default_profile(ROOT)))
    p = sub.add_parser('configure-resources'); p.add_argument('--root', required=True)
    p = sub.add_parser('check-resources'); p.add_argument('--root'); p.add_argument('--suite', default=str(DEFAULT_SUITE)); p.add_argument('--full', action='store_true'); p.add_argument('--network', action='store_true'); p.add_argument('--output')
    p = sub.add_parser('set-version'); p.add_argument('--run', required=True); p.add_argument('--version', required=True); p.add_argument('--evidence', required=True)
    for name in ('preflight', 'report', 'refresh-profile'):
        p = sub.add_parser(name); p.add_argument('--run', required=True)
        if name == 'preflight':
            p.add_argument('--full', action='store_true'); p.add_argument('--network', action='store_true')
    p = sub.add_parser('next'); p.add_argument('--run', required=True); p.add_argument('--limit', type=int, default=1)
    p = sub.add_parser('record'); p.add_argument('--run', required=True); p.add_argument('--case', required=True); p.add_argument('--result', required=True)
    p = sub.add_parser('explain'); p.add_argument('--run', required=True); p.add_argument('--case', required=True); p.add_argument('--input', required=True)
    p = sub.add_parser('template'); p.add_argument('--run', required=True); p.add_argument('--case', required=True); p.add_argument('--output', required=True)
    p = sub.add_parser('import-legacy'); p.add_argument('--run', required=True); p.add_argument('--source', required=True)
    args = parser.parse_args()
    if args.command == 'validate': out = {'validCases': validate_suite(load_suite(args.suite))}
    elif args.command == 'compile':
        compiled = load_suite(args.workbook)
        if args.check:
            if not Path(args.output).is_file() or read(args.output) != compiled:
                raise ValueError('Generated JSON is stale; run customer_journey.py compile')
        else:
            if Path(args.output).resolve() == Path(args.workbook).resolve():
                raise ValueError('Generated JSON must not overwrite the workbook')
            write(args.output, compiled)
        out = {'caseCount': len(compiled['cases']), 'output': args.output, 'check': args.check}
    elif args.command == 'configure-resources': out = resource_kit.configure(ROOT, args.root)
    elif args.command == 'check-resources':
        root = Path(args.root).resolve() if args.root else resource_kit.configured_root(ROOT)
        if root is None:
            raise ValueError('Configure the resource root first')
        suite = load_suite(args.suite)
        checked = resource_kit.inspect_resources(read(root / resource_kit.PROFILE), suite['resourceDefinitions'],
                                                suite['cases'], root=root, full=args.full, network=args.network)
        output = private_run(args.output) if args.output else root / 'checks/latest.json'
        write(output, checked)
        out = {'resourceCount': len(checked['resources']), 'gaps': checked['gaps'],
               'integrity': checked['integrity'], 'network': checked['network'], 'savedTo': str(output)}
    elif args.command == 'init': out = init_run(args.run, args.suite, args.profile)
    elif args.command == 'set-version': out = set_device_version(args.run, args.version, args.evidence)
    elif args.command == 'preflight':
        checked = preflight(args.run, args.full, args.network)
        out = {'resourceCount': len(checked['resources']),
               'gaps': [{k: row[k] for k in ('id', 'title', 'request', 'affectedCases')} for row in checked['gaps']],
               'savedTo': str(Path(args.run) / 'preflight.json')}
    elif args.command == 'report': out = report(args.run, xlsx=True)
    elif args.command == 'refresh-profile': out = refresh_profile(args.run)
    elif args.command == 'next': out = queue(args.run, args.limit)
    elif args.command == 'template': out = result_template(args.run, args.case, args.output)
    elif args.command == 'record': out = record(args.run, args.case, read(args.result))
    elif args.command == 'explain': out = explain(args.run, args.case, read(args.input))
    elif args.command == 'import-legacy': out = migrate(args.run, args.source)
    print(json.dumps(out, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, StopIteration) as exc:
        print('error: ' + str(exc), file=sys.stderr)
        sys.exit(2)
