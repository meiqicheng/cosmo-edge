"""Read the maintained XLSX into the execution schema (Python standard library).

Authoring/export uses the Codex bundled Artifact Tool; this reader never runs
cell formulas, macros or external links. Sheet/row/cell errors are actionable.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import posixpath
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
import zipfile

NS = {'x': 'http://schemas.openxmlformats.org/spreadsheetml/2006/main'}
REL = '{http://schemas.openxmlformats.org/officeDocument/2006/relationships}id'
CASE_HEADERS = ['用例ID', '旅程', '模块', '优先级', '测试内容', '前置条件', '资源',
                '操作步骤', '预期结果', '清理恢复', '取证要求', '执行方式', '回归层级',
                '适用边界', '来源', '执行状态', '评审意见', '实际结果/证据链接', '缺陷ID',
                '修订', '资源编号', '执行器', '未通过原因', '失败步骤', '证据链接',
                '测试版本', '执行时间', '清理结果', '记录来源']
RESOURCE_HEADERS = ['资源编号', '资源名称', '类型', '准备要求', '提供方', '必填字段', '备注']
EXTRA_HEADERS = ['用例ID', '检查ID', '检查内容']
MATRIX_HEADERS = {
    '设备矩阵': ['实例ID', '设备', '协议', '通道或码流', '组合与前置', '关联用例', '完成标志', '执行状态', '说明'],
    '参数覆盖': ['参数记录ID', '归属类型', '归属ID', '归属名称', '参数键', '参数名称', '资源平台', '默认值', '约束或枚举', '依赖字段', '语义描述', '输入与行为测试', '关联用例', '执行状态', '评审意见'],
    '组件覆盖': ['组件目录', '组件ID', '组件名称', '资源平台', '关联用例', '组件说明', '逐项验证', '源码依据', '执行状态', '评审意见'],
    '接口覆盖': ['接口记录ID', '接口路径', '认证声明', '业务模块', '业务用例', '公共契约用例', '操作性质', '注册来源', '执行状态', '评审意见'],
}


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def column_number(ref):
    result = 0
    for c in re.match(r'[A-Z]+', ref).group():
        result = result * 26 + ord(c) - 64
    return result


def workbook_cells(path):
    """Return sparse sheet rows, preserving string IDs and rejecting formulas later."""
    with zipfile.ZipFile(path) as z:
        if len(z.infolist()) > 10000 or sum(i.file_size for i in z.infolist()) > 100_000_000:
            raise ValueError('Workbook exceeds the supported test-catalog size')
        for info in z.infolist():
            if info.filename.endswith('.xml'):
                raw = z.read(info.filename)
                if b'<!DOCTYPE' in raw or b'<!ENTITY' in raw:
                    raise ValueError('XML entity declarations are not supported')
        strings = []
        if 'xl/sharedStrings.xml' in z.namelist():
            strings = [''.join(t.text or '' for t in si.findall('.//x:t', NS))
                       for si in ET.fromstring(z.read('xl/sharedStrings.xml')).findall('x:si', NS)]
        relationships = {r.attrib['Id']: r.attrib for r in ET.fromstring(z.read('xl/_rels/workbook.xml.rels'))}
        wb = ET.fromstring(z.read('xl/workbook.xml'))
        output = {}
        for sheet in wb.findall('x:sheets/x:sheet', NS):
            rel = relationships[sheet.attrib[REL]]
            if rel.get('TargetMode') == 'External':
                raise ValueError('External sheets are not supported')
            target = rel['Target']
            filename = target.lstrip('/') if target.startswith('/') else posixpath.normpath('xl/' + target)
            if not filename.startswith('xl/'):
                raise ValueError('Invalid sheet relationship')
            rows = {}
            tree = ET.fromstring(z.read(filename))
            for row in tree.findall('x:sheetData/x:row', NS):
                cells = {}
                for cell in row.findall('x:c', NS):
                    ref = cell.attrib['r']
                    value = cell.find('x:v', NS)
                    value = value.text if value is not None else ''
                    kind = cell.get('t')
                    if kind == 's': value = strings[int(value)]
                    elif kind == 'inlineStr': value = ''.join(t.text or '' for t in cell.findall('.//x:t', NS))
                    cells[column_number(ref)] = {'value': value or '', 'formula': cell.find('x:f', NS) is not None, 'ref': ref}
                rows[int(row.attrib['r'])] = cells
            output[sheet.attrib['name']] = rows
        return output


def table(book, sheet, headers, required=True):
    if sheet not in book:
        if required: raise ValueError('Missing sheet: ' + sheet)
        return []
    rows = book[sheet]
    actual = [rows.get(4, {}).get(i + 1, {}).get('value', '') for i in range(len(headers))]
    if actual != headers:
        raise ValueError(f'{sheet}!row 4: expected headers {headers}, found {actual}')
    output = []
    for number, cells in sorted(rows.items()):
        if number <= 4: continue
        values = []
        for i in range(len(headers)):
            c = cells.get(i + 1, {})
            if c.get('formula'):
                raise ValueError(f'{sheet}!{c["ref"]}: catalog fields must be literal values, not formulas')
            values.append(c.get('value', ''))
        if not any(str(v).strip() for v in values): continue
        if not values[0]: raise ValueError(f'{sheet}!A{number}: missing record ID')
        output.append((number, values))
    return output


def identifiers(value):
    return [s.strip() for s in re.split(r'[;；,，、\n]+', str(value)) if s.strip()]


def compile_workbook(path):
    source_hash = sha256(path)
    book = workbook_cells(path)
    overview = book.get('评审总览', {})
    settings = {cells.get(1, {}).get('value'): cells.get(2, {}).get('value', '') for cells in overview.values()}
    for row in overview.values():
        if row.get(1, {}).get('value') in ('用例库版本', '工作簿格式', '文档类型') and row.get(2, {}).get('formula'):
            raise ValueError('Workbook settings cannot be formulas')
    if settings.get('工作簿格式') != 'cosmoedge-casebook-v1':
        raise ValueError('Unsupported workbook format; use the maintained casebook, not the v0.1 draft')
    if settings.get('文档类型') != '用例库':
        raise ValueError('Result workbooks cannot be used as the maintained casebook')
    version = settings.get('用例库版本')
    if not version or not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('评审总览: 用例库版本 must be semantic version, e.g. 1.1.0')
    resources = {}
    for n, row in table(book, '资源与缺口', RESOURCE_HEADERS):
        rid, title, kind, request, owner, fields, note = row
        if rid in resources: raise ValueError(f'资源与缺口!A{n}: duplicate resource ID {rid}')
        if not title or not kind or not request: raise ValueError(f'资源与缺口!row {n}: missing resource definition')
        resource = dict(title=title, kind=kind, request=request, owner=owner)
        if fields: resource['fields'] = identifiers(fields)
        if note: resource['note'] = note
        resources[rid] = resource
    cases = []
    modules = {}
    for n, row in table(book, '详细用例', CASE_HEADERS):
        d = dict(zip(CASE_HEADERS, row)); cid = d['用例ID']
        if not re.fullmatch(r'[A-Z]+-\d+', cid): raise ValueError(f'详细用例!A{n}: invalid case ID {cid}')
        revision = str(d['修订'])
        if not re.fullmatch(r'[1-9]\d*', revision): raise ValueError(f'详细用例!T{n}: 修订 must be a positive integer')
        actions = [re.sub(r'^\d+[.、]\s*', '', s).strip() for s in d['操作步骤'].splitlines() if s.strip()]
        c = dict(id=cid, revision=int(revision), title=d['测试内容'], module=d['模块'], priority=d['优先级'], journey=d['旅程'],
                 preconditions=d['前置条件'], requires=identifiers(d['资源编号']), resourceNotes=d['资源'],
                 steps=[{'id': f'{cid}.S{i + 1:02}', 'text': text} for i, text in enumerate(actions)],
                 assertions=[{'id': cid + '.A01', 'text': d['预期结果']}], expected=d['预期结果'], cleanup=d['清理恢复'],
                 method=d['执行方式'], executor=d['执行器'], applicability=d['适用边界'], source=d['来源'], suite=d['回归层级'],
                 review=d['评审意见'], evidenceRequirements=d['取证要求'])
        if c['priority'] not in ('P0', 'P1', 'P2'): raise ValueError(f'详细用例!D{n}: priority must be P0/P1/P2')
        cases.append(c)
        code = cid.split('-')[0]
        modules.setdefault(code, {'code': code, 'name': c['module'], 'journey': c['journey'], 'route': '见功能覆盖', 'source': c['source']})
    lookup = {c['id']: c for c in cases}
    if len(lookup) != len(cases): raise ValueError('详细用例: duplicate case ID')
    def attach(ids, aid, text, location):
        owners = identifiers(ids)
        if not owners: raise ValueError(f'{location}: missing case reference')
        for cid in owners:
            if cid not in lookup: raise ValueError(f'{location}: unknown case reference {cid}')
            lookup[cid]['assertions'].append({'id': cid + '.' + aid, 'text': text})
    for n, r in table(book, '设备矩阵', MATRIX_HEADERS['设备矩阵']):
        attach(r[5], r[0], f'{r[1]} / {r[2]} / {r[3]}：{r[4]}；{r[6]}。{r[8]}', f'设备矩阵!row {n}')
    for n, r in table(book, '参数覆盖', MATRIX_HEADERS['参数覆盖']):
        attach(r[12], r[0], f'参数 {r[3]} / {r[4]}（{r[5]}）：{r[11]}。默认值：{r[7]}；约束：{r[8]}；依赖：{r[9]}；语义：{r[10]}；平台：{r[6]}。', f'参数覆盖!row {n}')
    for n, r in table(book, '组件覆盖', MATRIX_HEADERS['组件覆盖']):
        attach(r[4], 'COMP-' + r[0] + '-' + r[1], f'{r[0]}/{r[1]} {r[2]}：{r[5]}；{r[6]}；平台：{r[3]}。', f'组件覆盖!row {n}')
    for n, r in table(book, '接口覆盖', MATRIX_HEADERS['接口覆盖']):
        attach('API-001', r[0], f'{r[1]}：匿名/登录/失效凭据边界；认证声明：{r[2]}。写操作仅对本轮对象执行。', f'接口覆盖!row {n}')
        attach('API-002', r[0], f'{r[1]}：按当前契约准备独立合法/缺参/非法类型请求，核对字段与错误；{r[6]}。', f'接口覆盖!row {n}')
    for n, r in table(book, '补充检查', EXTRA_HEADERS):
        if not re.fullmatch(r'[A-Za-z0-9_-]+', r[1]) or not r[2]: raise ValueError(f'补充检查!row {n}: invalid check ID/text')
        attach(r[0], 'EXTRA-' + r[1], r[2], f'补充检查!row {n}')
    if sha256(path) != source_hash:
        raise ValueError('Workbook changed during compilation; save it and retry')
    return dict(schemaVersion=1, version=version, title='CosmoEdge全量客户旅程用例库',
                generatedFrom={'workbook': Path(path).name, 'sha256': source_hash, 'doNotEdit': '由XLSX生成；请修改正式工作簿。'},
                resourceDefinitions=resources, modules=list(modules.values()), cases=cases)


def artifact_runtime():
    """Resolve the Codex runtime without installing or using repository dependencies."""
    base = Path.home() / '.cache/codex-runtimes/codex-primary-runtime/dependencies'
    node = Path(os.environ.get('COSMO_JOURNEY_NODE', str(base / 'node/bin' / ('node.exe' if os.name == 'nt' else 'node'))))
    modules = Path(os.environ.get('COSMO_JOURNEY_NODE_MODULES', str(base / 'node/node_modules')))
    if not node.is_file() or not (modules / '@oai/artifact-tool/package.json').is_file():
        raise ValueError('XLSX export needs the Codex bundled Node and @oai/artifact-tool. Locate with load_workspace_dependencies, then set COSMO_JOURNEY_NODE and COSMO_JOURNEY_NODE_MODULES. HTML/JSON results remain saved.')
    return node, modules


def export_results(run, suite, meta):
    run = Path(run)
    if not meta.get('workbookSnapshot'):
        raise ValueError('This legacy run has no frozen XLSX. Create a new XLSX-backed run and import legacy records explicitly.')
    node, modules = artifact_runtime()
    script_name = 'customer_journey_quick_xlsx.mjs' if suite.get('suiteKind') == 'quick' else 'customer_journey_xlsx.mjs'
    script = Path(__file__).with_name(script_name)
    result = subprocess.run([str(node), str(script), str(run), str(modules)], text=True,
                            encoding='utf-8', errors='replace', capture_output=True, timeout=180)
    if result.returncode:
        raise ValueError('XLSX export failed; HTML/JSON remain saved. ' + result.stderr[-1500:])
    return json.loads(result.stdout.strip().splitlines()[-1])
