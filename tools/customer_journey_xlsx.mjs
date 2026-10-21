// Result export only. The maintained workbook is never overwritten.
import fs from 'node:fs/promises';
import path from 'node:path';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';

const [runArg, modules] = process.argv.slice(2);
if (!runArg || !modules) throw new Error('Usage: customer_journey_xlsx.mjs <run> <bundled-node-modules>');
const run = path.resolve(runArg);
const requireBundled = createRequire(path.join(path.resolve(modules), '_journey_loader.cjs'));
const { FileBlob, SpreadsheetFile } = await import(pathToFileURL(requireBundled.resolve('@oai/artifact-tool')).href);
const read = async name => JSON.parse(await fs.readFile(path.join(run, name), 'utf8'));
const meta = await read('run.json'), suite = await read('suite.snapshot.json'), results = await read('results.json');
if (!meta.workbookSnapshot || path.basename(meta.workbookSnapshot) !== meta.workbookSnapshot) throw new Error('A frozen XLSX snapshot is required');
const wb = await SpreadsheetFile.importXlsx(await FileBlob.load(path.join(run, meta.workbookSnapshot)));
const labels = { PASS: '通过', FAIL: '失败', BLOCKED: '阻塞', PARTIAL: '部分完成', NOT_EXECUTED: '未执行', NOT_APPLICABLE: '不适用' };
const literal = x => typeof x === 'string' && x.startsWith('=') ? "'" + x : x;
const col = n => { let s = ''; for (; n; n = Math.floor((n - 1) / 26)) s = String.fromCharCode(65 + (n - 1) % 26) + s; return s; };
const sheet = wb.worksheets.getItem('详细用例');
const used = sheet.getUsedRange();
const values = sheet.getRange(`A1:AC${used.rowIndex + used.values.length}`).values;
const rows = new Map();
for (let i = 4; i < values.length; i++) {
  const id = values[i][0];
  if (!id) continue;
  if (rows.has(id)) throw new Error(`Duplicate workbook case ID: ${id}`);
  rows.set(id, i + 1);
}
const ids = suite.cases.map(c => c.id);
if (rows.size !== ids.length || Object.keys(results).length !== ids.length || ids.some(id => !rows.has(id) || !results[id])) throw new Error('Workbook/snapshot/result case IDs differ');
const counts = Object.fromEntries(Object.keys(labels).map(k => [k, 0]));
const detailRows = [], historyRows = [], analysisRows = [];
for (const c of suite.cases) {
  const r = results[c.id], row = rows.get(c.id), legacy = r.provenance === 'legacy-case-summary';
  if (!labels[r.status]) throw new Error('Invalid result status: ' + c.id);
  counts[r.status]++;
  const failed = [...(r.steps || []), ...(r.assertions || [])].filter(x => x.status === 'FAIL').map(x => x.id).join('\n');
  const provenance = legacy ? '历史汇总迁移；未按此版XLSX重测；无逐步骤记录' : (r.provenance || '尚无实测记录');
  sheet.getRange(`P${row}`).values = [[labels[r.status]]];
  sheet.getRange(`R${row}`).values = [[literal(r.actual || '')]];
  sheet.getRange(`S${row}`).values = [[literal(r.defectId || '')]];
  const at = !legacy && r.at ? new Date(r.at) : null;
  const cleanup = r.cleanup ? `${r.cleanup.status || ''} ${r.cleanup.note || ''}` : '未单独记录';
  const analysis = r.analysis;
  const reason = analysis?.cause || r.reason || '';
  sheet.getRange(`W${row}:AC${row}`).values = [[reason, failed || (legacy && r.status === 'FAIL' ? '旧记录未标注步骤，见实际结果' : ''), (r.evidence || []).join('\n'), r.deviceVersion || meta.deviceVersion, at, cleanup, provenance].map(literal)];
  if (!['PASS', 'NOT_APPLICABLE'].includes(r.status)) analysisRows.push([c.id, c.title, labels[r.status], analysis?.category || '尚未分析', reason, analysis?.remaining || '待逐项核对', analysis?.nextAction || '补齐原因分析', analysis?.owner || 'Codex', analysis?.certainty || 'UNKNOWN', analysis?.conditions || '', analysis?.caseDesign || '', (analysis?.evidence || []).join('\n')]);
  const oldHeight = sheet.getRange(`A${row}:AC${row}`).format.rowHeight;
  const lines = Math.max(String(r.actual || '').length / 24, String(reason).length / 29, (r.evidence || []).length * 2, failed.split('\n').length);
  sheet.getRange(`A${row}:AC${row}`).format.rowHeight = Math.min(390, Math.max(Number(oldHeight) || 60, Math.ceil(lines) * 15 + 10));
  for (const [key, kind] of [['steps', '步骤'], ['assertions', '验收检查']]) {
    const observed = new Map((r[key] || []).map(s => [s.id, s]));
    for (const s of c[key]) {
      const a = observed.get(s.id);
      detailRows.push([c.id, s.id, kind, s.text, a ? labels[a.status] : '未记录', a?.actual || (legacy ? '旧记录未逐项拆分，不补造结果。' : ''), (a?.evidence || []).join('\n')]);
    }
  }
  for (const [i, a] of (r.attempts || []).entries()) historyRows.push([c.id, i + 1, labels[a.status], a.actual || '', a.reason || '', (a.evidence || []).join('\n'), a.at ? new Date(a.at) : null]);
}
sheet.getRange(`AA5:AA${values.length}`).setNumberFormat('yyyy-mm-dd hh:mm:ss');
sheet.getRange(`P5:P${values.length}`).conditionalFormats.deleteAll();
for (const [text, fill, color] of [['通过', '#DCF5E8', '#12603A'], ['失败', '#FCE3E1', '#9B2424'], ['阻塞', '#FFF0C5', '#8A520B'], ['部分完成', '#FFF6DB', '#75520C']]) sheet.getRange(`P5:P${values.length}`).conditionalFormats.add('containsText', { text, format: { fill, font: { color, bold: true } } });
const overview = wb.worksheets.getItem('评审总览');
// Rebuild ranges after appended or reordered cases, including rows added outside
// the original Excel table. Never rely on the master's cached summary values.
const lastRow = Math.max(...rows.values());
for (const table of [...sheet.tables.items]) table.delete();
sheet.tables.add(`A4:AC${lastRow}`, true, 'TestCases').showFilterButton = true;
overview.getRange('B5').formulas = [[`=COUNTA('详细用例'!A5:A${lastRow})`]];
['通过', '失败', '阻塞', '未执行', '部分完成', '不适用'].forEach((status, i) => overview.getRange(`B${6 + i}`).formulas = [[`=COUNTIFS('详细用例'!P5:P${lastRow},"${status}")`]]);
overview.getRange('A2').values = [['CosmoEdge 客户旅程测试结果']];
overview.getRange('B14:B17').values = [[meta.legacyImport ? '历史记录整理；未按本版XLSX重新测试设备。' : '按冻结XLSX执行，结果按用例ID匹配。'], ['筛选详细用例的执行状态，查看实际结果、原因、失败步骤及证据。'], ['记录时间使用UTC；历史迁移没有逐条执行时间，留空。'], [`用例库版本 ${suite.version}；设备版本 ${meta.deviceVersion}`]];
overview.getRange('B36').values = [['执行结果']];
overview.getRange('A41:B43').values = [['被测版本', meta.deviceVersion], ['原始工作簿SHA-256', meta.workbookSha256], ['结果生成时间（UTC）', new Date()]];
overview.getRange('B43').setNumberFormat('yyyy-mm-dd hh:mm:ss');
overview.getRange('A41:B43').format = { font: { name: 'Microsoft YaHei', size: 10, color: '#24334A' }, rowHeight: 32, wrapText: true };
function addDetails(name, headers, body, widths) {
  const s = wb.worksheets.add(name); s.showGridLines = false;
  s.getRange('A2').values = [[name]];
  s.getRange('A2').format.font = { name: 'Microsoft YaHei', size: 15, bold: true, color: '#183153' };
  const data = body.length ? body : [headers.map(() => '')];
  s.getRange(`A4:${col(headers.length)}${data.length + 4}`).values = [headers, ...data.map(row => row.map(literal))];
  s.getRange(`A4:${col(headers.length)}${data.length + 4}`).format = { font: { name: 'Microsoft YaHei', size: 10, color: '#24334A' }, verticalAlignment: 'top', wrapText: true };
  s.getRange(`A4:${col(headers.length)}4`).format = { fill: '#253F63', font: { name: 'Microsoft YaHei', size: 10, color: '#FFFFFF', bold: true }, rowHeight: 34 };
  widths.forEach((w, i) => s.getRange(`${col(i + 1)}4:${col(i + 1)}${data.length + 4}`).format.columnWidth = w);
  for (let i = 0; i < data.length; i++) {
    const lines = Math.max(...data[i].map((v, c) => String(v ?? '').split('\n').reduce((sum, line) => sum + Math.max(1, Math.ceil([...line].reduce((n, ch) => n + (ch.charCodeAt(0) > 255 ? 1.8 : 1), 0) / (widths[c] - 2))), 0)));
    s.getRange(`A${i + 5}:${col(headers.length)}${i + 5}`).format.rowHeight = Math.min(390, Math.max(34, lines * 15 + 8));
  }
  s.freezePanes.freezeRows(4); s.freezePanes.freezeColumns(2);
  s.tables.add(`A4:${col(headers.length)}${data.length + 4}`, true, { '步骤与检查结果': 'ExecutionChecks', '重测历史': 'ExecutionHistory', '原因与补测': 'CaseExplanations' }[name]).showFilterButton = true;
  return s;
}
addDetails('步骤与检查结果', ['用例ID', '检查ID', '类别', '操作或预期', '结果', '实际结果', '证据'], detailRows, [18, 42, 14, 100, 16, 70, 55]);
if (historyRows.length) addDetails('重测历史', ['用例ID', '尝试序号', '结果', '实际结果', '原因', '证据', '记录时间（UTC）'], historyRows, [18, 14, 16, 60, 65, 55, 26]).getRange(`G5:G${historyRows.length + 4}`).setNumberFormat('yyyy-mm-dd hh:mm:ss');
if (analysisRows.length) {
  addDetails('原因与补测', ['用例ID', '测试内容', '状态', '原因分类', '为什么未通过', '尚缺哪些检查', '下一步具体动作', '处理方', '证实程度', '条件核对（不是自动归因）', '用例改进', '分析依据'], analysisRows, [17, 32, 15, 28, 72, 72, 75, 28, 27, 65, 60, 45]);
  overview.getRange('A45:B47').values = [['原因阅读入口', '查看“原因与补测”：每项列原因、尚缺检查、下一步、处理方及证据。'], ['分析边界', 'CONFIRMED=有依据；PARTLY_CONFIRMED=部分有依据；UNKNOWN=原记录未说明根因，不能猜测。'], ['未通过且未分析', analysisRows.filter(r => r[3] === '尚未分析').length]];
  overview.getRange('A45:B47').format = { font: { name: 'Microsoft YaHei', size: 10, color: '#24334A' }, rowHeight: 46, wrapText: true };
  overview.getRange('A48:B49').values = [['历史停止原因未证实', analysisRows.filter(r => r[8] === 'UNKNOWN').length], ['结论边界', '补充原因说明和补测计划不等于重新实测；原状态保留。潜在资源缺口不自动视为阻塞根因。']];
  overview.getRange('A48:B49').format = { font: { name: 'Microsoft YaHei', size: 10, color: '#24334A' }, rowHeight: 46, wrapText: true };
}
wb.recalculate();
if (overview.getRange('B5').values[0][0] !== ids.length) throw new Error('Workbook total formula mismatch');
for (const [key, row] of Object.entries({ PASS: 6, FAIL: 7, BLOCKED: 8, NOT_EXECUTED: 9, PARTIAL: 10, NOT_APPLICABLE: 11 })) if (overview.getRange(`B${row}`).values[0][0] !== counts[key]) throw new Error('Workbook status formula mismatch: ' + key);
const output = path.join(run, 'CosmoEdge-逐用例测试结果.xlsx');
const temporary = path.join(run, 'results-export.tmp.xlsx');
await (await SpreadsheetFile.exportXlsx(wb)).save(temporary);
await fs.rename(temporary, output);
console.log(JSON.stringify({ output, cases: ids.length, counts, checks: detailRows.length }));
