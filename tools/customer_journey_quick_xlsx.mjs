// Export the frozen quick casebook by Q case/step ID. The source stays unchanged.
import fs from 'node:fs/promises';
import path from 'node:path';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';

const [runArg, modules] = process.argv.slice(2);
if (!runArg || !modules) throw new Error('Usage: customer_journey_quick_xlsx.mjs <run> <bundled-node-modules>');
const run = path.resolve(runArg);
const requireBundled = createRequire(path.join(path.resolve(modules), '_journey_loader.cjs'));
const { FileBlob, SpreadsheetFile } = await import(pathToFileURL(requireBundled.resolve('@oai/artifact-tool')).href);
const read = async name => JSON.parse(await fs.readFile(path.join(run, name), 'utf8'));
const meta = await read('run.json'), suite = await read('suite.snapshot.json'), results = await read('results.json');
if (suite.suiteKind !== 'quick' || !meta.workbookSnapshot || path.basename(meta.workbookSnapshot) !== meta.workbookSnapshot) {
  throw new Error('Frozen quick XLSX snapshot required');
}
const wb = await SpreadsheetFile.importXlsx(await FileBlob.load(path.join(run, meta.workbookSnapshot)));
const cases = wb.worksheets.getItem('主要用例');
const steps = wb.worksheets.getItem('步骤与验收');
const overview = wb.worksheets.getItem('评审总览');
const labels = {PASS:'通过',FAIL:'失败',BLOCKED:'阻塞',PARTIAL:'部分完成',NOT_EXECUTED:'未执行',NOT_APPLICABLE:'不适用'};
const safe = value => typeof value === 'string' && value.startsWith('=') ? "'" + value : value;
const rowMap = (sheet, column, header, expectedIds) => {
  const used = sheet.getUsedRange();
  const end = used.rowIndex + used.values.length;
  const values = sheet.getRange(`${column}1:${column}${end}`).values;
  if (values[3][0] !== header) throw new Error(`Unexpected ${sheet.name} header`);
  const map = new Map();
  for (let i=4;i<values.length;i++) {
    const id = values[i][0];
    if (!id) continue;
    if (map.has(id)) throw new Error('Duplicate workbook ID: ' + id);
    map.set(id, i+1);
  }
  if (map.size !== expectedIds.length || expectedIds.some(id => !map.has(id))) throw new Error('Frozen workbook and suite IDs differ');
  return map;
};
const caseIds = suite.cases.map(c=>c.id);
if (Object.keys(results).length !== caseIds.length || caseIds.some(id=>!results[id])) throw new Error('Result IDs differ');
const caseRows = rowMap(cases,'A','编号',caseIds);
const stepIds = suite.cases.flatMap(c=>c.steps.map(s=>s.id));
const stepRows = rowMap(steps,'B','步骤编号',stepIds);
const counts = Object.fromEntries(Object.keys(labels).map(k=>[k,0]));
const explanations = [];
for (const c of suite.cases) {
  const result = results[c.id], row = caseRows.get(c.id);
  if (!labels[result.status]) throw new Error('Invalid status for ' + c.id);
  counts[result.status]++;
  cases.getRange(`J${row}`).values = [[labels[result.status]]];
  const reason = result.analysis?.cause || result.reason || '';
  const actual = result.actual || '';
  const evidence = (result.evidence || []).join('\n');
  const issues = [actual && '实际：' + actual, reason && '原因：' + reason,
    result.analysis?.remaining && '尚缺：' + result.analysis.remaining,
    result.analysis?.nextAction && '下一步：' + result.analysis.nextAction,
    evidence && '证据：' + evidence].filter(Boolean).join('\n');
  cases.getRange(`K${row}`).values = [[safe(issues)]];
  cases.getRange(`K${row}`).format.rowHeight = Math.min(350, Math.max(36, issues.split('\n').length*20+16));
  if (!['PASS','NOT_APPLICABLE'].includes(result.status)) {
    explanations.push([c.id,c.title,labels[result.status],reason,result.analysis?.remaining || '',
      result.analysis?.nextAction || '',result.analysis?.owner || '',result.analysis?.certainty || '',evidence].map(safe));
  }
  const observedSteps = new Map((result.steps || []).map(x=>[x.id,x]));
  const observedChecks = new Map((result.assertions || []).map(x=>[x.id,x]));
  for (let i=0;i<c.steps.length;i++) {
    const step = c.steps[i], check = c.assertions[i], sr=stepRows.get(step.id);
    if (!sr || !check) throw new Error('Incomplete quick case schema: ' + c.id);
    const a=observedSteps.get(step.id), b=observedChecks.get(check.id);
    const statuses=[a?.status || 'NOT_EXECUTED',b?.status || 'NOT_EXECUTED'];
    const order=['FAIL','BLOCKED','PARTIAL','NOT_EXECUTED','NOT_APPLICABLE','PASS'];
    const combined=order.find(s=>statuses.includes(s));
    const observation=[a?.actual,b?.actual && '验收：'+b.actual].filter(Boolean).join('\n');
    const problem=statuses.some(s=>!['PASS','NOT_APPLICABLE'].includes(s)) ? reason : '';
    const refs=[...new Set([...(a?.evidence || []),...(b?.evidence || [])])].join('\n');
    steps.getRange(`F${sr}:I${sr}`).values = [[safe(observation),labels[combined],safe(problem),safe(refs)]];
  }
}
overview.getRange('A2').values = [['CosmoEdge 主要功能客户旅程测试结果']];
overview.getRange('A3').values = [[`v${suite.version} · 测试结果 · 设备版本 ${meta.deviceVersion}`]];
overview.getRange('F5:F6').values = [[`设备版本：${meta.deviceVersion}`],[`本轮标识：${path.basename(run)}`]];
if (explanations.length) {
  const sheet=wb.worksheets.add('原因与补测');
  sheet.showGridLines=false;
  sheet.getRange('A2').values=[['未通过原因与下一步']];
  const headers=['用例编号','功能','状态','具体原因','尚缺检查','下一步','处理方','证实程度','证据'];
  sheet.getRange(`A4:I${explanations.length+4}`).values=[headers,...explanations];
  sheet.getRange(`A4:I${explanations.length+4}`).format={font:{name:'Microsoft YaHei',size:10,color:'#20334B'},wrapText:true,verticalAlignment:'top'};
  sheet.getRange('A4:I4').format={fill:'#284568',font:{name:'Microsoft YaHei',size:10,bold:true,color:'#FFFFFF'},rowHeight:32};
  [12,37,15,65,60,60,22,18,45].forEach((width,i)=>sheet.getRange(`${String.fromCharCode(65+i)}4:${String.fromCharCode(65+i)}${explanations.length+4}`).format.columnWidth=width);
  for(let i=0;i<explanations.length;i++)sheet.getRange(`A${i+5}:I${i+5}`).format.rowHeight=58;
  sheet.freezePanes.freezeRows(4);
}
wb.recalculate();
if (overview.getRange('B5').values[0][0] !== caseIds.length || overview.getRange('E9').values[0][0] !== counts.NOT_EXECUTED) {
  throw new Error('Quick summary formulas do not match frozen results');
}
const output=path.join(run,'CosmoEdge-主要功能逐用例测试结果.xlsx');
const temp=path.join(run,'quick-results-export.tmp.xlsx');
await (await SpreadsheetFile.exportXlsx(wb)).save(temp);
await fs.rename(temp,output);
console.log(JSON.stringify({output,cases:caseIds.length,checks:stepIds.length,counts}));
