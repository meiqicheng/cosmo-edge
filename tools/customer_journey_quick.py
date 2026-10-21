"""Compile the maintained quick XLSX into the ordinary journey ledger schema."""
from __future__ import annotations

import json
from pathlib import Path
import re

try:
    from customer_journey_workbook import identifiers, sha256, table, workbook_cells
except ModuleNotFoundError:
    from tools.customer_journey_workbook import identifiers, sha256, table, workbook_cells


REPO = Path(__file__).resolve().parents[1]
QUICK_BOOK = REPO / 'test/customer-journey/CosmoEdge-主要功能客户旅程测试用例.xlsx'
FULL_JSON = REPO / 'test/customer-journey/suite.json'
CASE_HEADERS = ['编号', '模块', '主要功能旅程', '抽样与前置条件', '资源编号',
                '预计下限\n分钟', '预计上限\n分钟', '清理与依赖',
                '全量库来源\n仅部分覆盖', '执行状态', '未通过原因/证据', '评审意见']
STEP_HEADERS = ['用例编号', '步骤编号', '操作', '可判定预期', '最小证据要求',
                '实际结果', '步骤状态', '未通过原因/下一步', '证据路径']
MODEL_RESOURCES = {
    'VLM-MODEL': {'title': '视觉语言大模型运行资源', 'kind': 'hardware',
                  'request': '目标芯片可运行的 VLM 模型包、分词器及设备加载结果；原始 bmodel 不等于可导入模型。',
                  'owner': '测试执行者/模型提供方'},
    'DINO-MODEL': {'title': '视觉检测大模型运行资源', 'kind': 'hardware',
                   'request': '目标芯片可运行的 DINO 模型包、词表及设备加载结果。',
                   'owner': '测试执行者/模型提供方'},
}


def compile_quick(path=QUICK_BOOK):
    path = Path(path)
    original_hash = sha256(path)
    book = workbook_cells(path)
    subtitle = book.get('评审总览', {}).get(3, {}).get(1, {}).get('value', '')
    match = re.search(r'\bv(\d+)\.(\d+)\b', subtitle)
    if not match:
        raise ValueError('评审总览!A3: quick casebook version vMAJOR.MINOR required')
    version = f'{match.group(1)}.{match.group(2)}.0'
    source = json.loads(FULL_JSON.read_text(encoding='utf-8'))
    source_ids = {case['id'] for case in source['cases']}
    all_resources = dict(source['resourceDefinitions'], **MODEL_RESOURCES)
    cases = []
    lookup = {}
    modules = {}
    for number, row in table(book, '主要用例', CASE_HEADERS):
        cid, module, title, pre, resource_text, low, high, cleanup, refs, status, actual, review = row
        if not re.fullmatch(r'Q\d{2}', cid) or cid in lookup:
            raise ValueError(f'主要用例!A{number}: invalid or duplicate quick case ID')
        if status != '未执行' or actual:
            raise ValueError(f'主要用例!J{number}: result workbook cannot be used as the casebook')
        required = identifiers(resource_text)
        source_refs = identifiers(refs)
        if not title or not module or not pre or not cleanup or not required or not source_refs:
            raise ValueError(f'主要用例!row {number}: missing case design fields')
        if set(required) - set(all_resources):
            raise ValueError(f'主要用例!E{number}: unknown resource ID')
        if set(source_refs) - source_ids:
            raise ValueError(f'主要用例!I{number}: unknown full-suite case ID')
        try:
            estimates = [int(low), int(high)]
        except (ValueError, TypeError):
            raise ValueError(f'主要用例!F{number}: invalid duration') from None
        if estimates[0] < 1 or estimates[1] < estimates[0]:
            raise ValueError(f'主要用例!F{number}: invalid duration range')
        case = {'id': cid, 'revision': 1, 'title': title, 'module': module, 'priority': 'P1',
                'journey': '主要功能快测', 'preconditions': pre, 'requires': required,
                'resourceNotes': resource_text, 'steps': [], 'assertions': [], 'expected': '',
                'cleanup': cleanup, 'method': 'Codex逐步操作与证据记录',
                'executor': 'codex-ui-and-evidence', 'applicability': '当前部署具备对应功能',
                'source': '全量库用例：' + '、'.join(source_refs), 'sourceCaseIds': source_refs,
                'suite': '主要功能快测', 'review': review, 'evidenceRequirements': '',
                'estimatedMinutes': estimates}
        cases.append(case)
        lookup[cid] = case
        modules.setdefault(module, {'code': module, 'name': module, 'journey': '主要功能快测',
                                    'route': '主要用例', 'source': '全量库映射'})
    for number, row in table(book, '步骤与验收', STEP_HEADERS):
        cid, step_id, action, expected, evidence, actual, status, reason, path_text = row
        if cid not in lookup or not re.fullmatch(re.escape(cid) + r'-S\d{2}', step_id):
            raise ValueError(f'步骤与验收!A{number}: unknown case or invalid step ID')
        if status != '未执行' or actual or reason or path_text:
            raise ValueError(f'步骤与验收!G{number}: result workbook cannot be used as the casebook')
        if not action or not expected or not evidence:
            raise ValueError(f'步骤与验收!row {number}: action, expected and evidence required')
        case = lookup[cid]
        next_number = len(case['steps']) + 1
        if step_id != f'{cid}-S{next_number:02}':
            raise ValueError(f'步骤与验收!B{number}: steps must be sequential within each case')
        case['steps'].append({'id': step_id, 'text': action})
        case['assertions'].append({'id': f'{cid}-A{next_number:02}', 'text': expected})
        case['evidenceRequirements'] += (('\n' if case['evidenceRequirements'] else '') +
                                         step_id + '：' + evidence)
        case['expected'] += (('\n' if case['expected'] else '') + expected)
    if any(not c['steps'] for c in cases):
        raise ValueError('Every quick case needs at least one step and assertion')
    if sha256(path) != original_hash:
        raise ValueError('Quick workbook changed during compilation')
    used = {r for case in cases for r in case['requires']}
    return {'schemaVersion': 1, 'suiteKind': 'quick', 'version': version,
            'title': 'CosmoEdge主要功能客户旅程用例',
            'generatedFrom': {'workbook': path.name, 'sha256': original_hash,
                              'doNotEdit': '由快速XLSX生成；请修改正式工作簿。'},
            'resourceDefinitions': {rid: all_resources[rid] for rid in sorted(used)},
            'modules': list(modules.values()), 'cases': cases}
