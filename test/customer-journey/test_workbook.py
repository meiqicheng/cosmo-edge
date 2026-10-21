"""XLSX contract tests. ZIP mutations below are synthetic reader test fixtures."""
import copy
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import customer_journey as j
import customer_journey_workbook as w


def mutate(source, target, sheet_name, changes=None, swap=False, append=False):
    with zipfile.ZipFile(source) as z:
        files = {name: z.read(name) for name in z.namelist()}
    book = ET.fromstring(files['xl/workbook.xml'])
    rels = {r.attrib['Id']: r.attrib['Target'] for r in ET.fromstring(files['xl/_rels/workbook.xml.rels'])}
    sheet = next(s for s in book.findall('x:sheets/x:sheet', w.NS) if s.attrib['name'] == sheet_name)
    filename = rels[sheet.attrib[w.REL]]
    filename = filename.lstrip('/') if filename.startswith('/') else 'xl/' + filename
    tree = ET.fromstring(files[filename])
    data = tree.find('x:sheetData', w.NS)
    def move(row, number):
        row.set('r', str(number))
        for cell in row.findall('x:c', w.NS):
            cell.set('r', re.sub(r'\d+$', str(number), cell.attrib['r']))
    if swap:
        a = next(r for r in data if r.attrib['r'] == '5')
        b = next(r for r in data if r.attrib['r'] == '6')
        move(a, 6); move(b, 5)
    if append:
        row = copy.deepcopy(next(r for r in data if r.attrib['r'] == '5'))
        move(row, append); data.append(row)
    for ref, value in (changes or {}).items():
        number = re.search(r'\d+$', ref).group()
        row = next((r for r in data if r.attrib['r'] == number), None)
        if row is None: row = ET.SubElement(data, '{%s}row' % w.NS['x'], r=number)
        cell = next((c for c in row if c.attrib['r'] == ref), None)
        if cell is None: cell = ET.SubElement(row, '{%s}c' % w.NS['x'], r=ref)
        cell.clear(); cell.set('r', ref)
        if isinstance(value, tuple):
            ET.SubElement(cell, '{%s}f' % w.NS['x']).text = value[0]
            ET.SubElement(cell, '{%s}v' % w.NS['x']).text = '1'
        else:
            cell.set('t', 'inlineStr')
            ET.SubElement(ET.SubElement(cell, '{%s}is' % w.NS['x']), '{%s}t' % w.NS['x']).text = value
    data[:] = sorted(data, key=lambda r: int(r.attrib['r']))
    files[filename] = ET.tostring(tree, encoding='utf-8', xml_declaration=True)
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, content in files.items(): z.writestr(name, content)


class WorkbookTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='journey-xlsx-unit-', dir=ROOT / 'output/agent-runs')
        self.base = pathlib.Path(self.tmp.name)
        self.book = self.base / 'cases.xlsx'
        self.profile = self.base / 'profile.json'
        detail = w.workbook_cells(j.DEFAULT_SUITE)['详细用例']
        self.appendrow = max(n for n, r in detail.items() if n > 4 and r.get(1, {}).get('value')) + 1
        self.casecount = len(j.load_suite(j.DEFAULT_SUITE)['cases'])
        j.write(self.profile, {'resources': {'DUT': {'host': 'example.test', 'username': 'fixture', 'password': 'fixture-secret'}}})
    def tearDown(self): self.tmp.cleanup()
    def test_edit_and_append_read_from_xlsx(self):
        mutate(j.DEFAULT_SUITE, self.book, '详细用例', {'E5': 'Edited title', 'H5': '1. First action\n2. Second action', f'A{self.appendrow}': 'REG-999'}, append=self.appendrow)
        suite = j.load_suite(self.book)
        self.assertEqual(self.casecount + 1, len(suite['cases']))
        self.assertEqual('Edited title', suite['cases'][0]['title'])
        self.assertEqual(['First action', 'Second action'], [s['text'] for s in suite['cases'][0]['steps']])
        j.init_run(self.base / 'run', self.book, self.profile)
        self.assertEqual('NOT_EXECUTED', j.read(self.base / 'run/results.json')['REG-999']['status'])
    def test_invalid_contract_rejected_with_location(self):
        for cells, message in [({'A6': 'ENV-001'}, 'duplicate case'), ({'H4': 'wrong'}, 'row 4'), ({'T5': '0'}, 'T5'), ({'U5': 'UNKNOWN'}, 'unknown resource'), ({'H5': ('1+1',)}, 'H5'), ({'I5': ''}, 'expected')]:
            with self.subTest(cells=cells):
                mutate(j.DEFAULT_SUITE, self.book, '详细用例', cells)
                with self.assertRaisesRegex(ValueError, message): j.load_suite(self.book)
    def test_matrix_edit_and_extra_checks_are_executable(self):
        mutate(j.DEFAULT_SUITE, self.book, '参数覆盖', {'L5': 'Verify changed constraint'})
        second = self.base / 'extended.xlsx'
        mutate(self.book, second, '补充检查', {'A5': 'ENV-001', 'B5': 'CUSTOM-1', 'C5': 'Verify extra observable'})
        suite = j.load_suite(second)
        assertions = [a for c in suite['cases'] for a in c['assertions']]
        self.assertTrue(any('Verify changed constraint' in a['text'] for a in assertions))
        self.assertIn({'id': 'ENV-001.EXTRA-CUSTOM-1', 'text': 'Verify extra observable'}, assertions)
    def test_bad_matrix_reference_rejected(self):
        mutate(j.DEFAULT_SUITE, self.book, '设备矩阵', {'F5': 'UNKNOWN-999'})
        with self.assertRaisesRegex(ValueError, 'unknown case reference'): j.load_suite(self.book)
        mutate(j.DEFAULT_SUITE, self.book, '参数覆盖', {'M5': ''})
        with self.assertRaisesRegex(ValueError, 'missing case reference'): j.load_suite(self.book)
    def test_frozen_book_is_independent_and_tamper_detected(self):
        mutate(j.DEFAULT_SUITE, self.book, '详细用例', {'E5': 'Original'})
        run = self.base / 'run'; j.init_run(run, self.book, self.profile)
        mutate(j.DEFAULT_SUITE, self.book, '详细用例', {'E5': 'New version'})
        self.assertEqual('Original', j.load_run(run)[2]['cases'][0]['title'])
        mutate(self.book, run / 'casebook.snapshot.xlsx', '详细用例', {'E5': 'Tampered'})
        with self.assertRaisesRegex(ValueError, 'Frozen XLSX'): j.load_run(run)
    def test_generated_json_stale_check(self):
        generated = self.base / 'suite.json'
        cmd = [sys.executable, str(ROOT / 'tools/customer_journey.py'), 'compile', '--output', str(generated)]
        self.assertEqual(0, subprocess.run(cmd, capture_output=True).returncode)
        self.assertEqual(0, subprocess.run(cmd + ['--check'], capture_output=True).returncode)
        value = j.read(generated); value['cases'][0]['title'] = 'stale'; j.write(generated, value)
        self.assertEqual(2, subprocess.run(cmd + ['--check'], capture_output=True).returncode)
    def test_export_by_id_with_retries_and_new_rows(self):
        # Sort the first rows and add a case outside the original table/range.
        mutate(j.DEFAULT_SUITE, self.book, '详细用例', {f'A{self.appendrow}': 'REG-999'}, swap=True, append=self.appendrow)
        before = w.sha256(self.book); run = self.base / 'run'
        j.init_run(run, self.book, self.profile)
        j.write(run / 'evidence/fixture.json', {'fixtureOnly': True})
        suite = j.load_run(run)[2]
        case = next(c for c in suite['cases'] if c['id'] == 'ENV-001')
        evidence = ['evidence/fixture.json']
        record = {'status': 'PASS', 'reasonCode': 'NONE', 'reason': 'Fixture passes', 'actual': '=1+1', 'evidence': evidence,
                  'cleanup': {'status': 'NOT_REQUIRED', 'note': 'Read-only synthetic fixture'}}
        for key in ('steps', 'assertions'):
            record[key] = [{'id': s['id'], 'status': 'PASS', 'actual': 'Fixture observed', 'evidence': evidence} for s in case[key]]
        failed = copy.deepcopy(record); failed.update(status='FAIL', reasonCode='PRODUCT_DEFECT', reason='Fixture mismatch')
        failed['steps'][0]['status'] = 'FAIL'
        j.record(run, case['id'], failed); j.record(run, case['id'], record)
        j.record(run, 'ENV-002', {'status': 'BLOCKED', 'reasonCode': 'RESOURCE_MISSING', 'reason': 'Fixture package absent', 'actual': 'Fixture check'})
        j.explain(run, 'ENV-002', {'category': 'Fixture missing resource', 'cause': 'Verified package absent', 'certainty': 'CONFIRMED', 'remaining': 'Import package', 'nextAction': 'Provide fixture package', 'owner': 'Fixture owner', 'evidence': evidence})
        report = j.report(run, xlsx=True)
        self.assertEqual(before, w.sha256(self.book))
        self.assertEqual(before, w.sha256(run / 'casebook.snapshot.xlsx'))
        book = w.workbook_cells(report['xlsxReport']); detail = book['详细用例']
        self.assertEqual('ENV-001', detail[6][1]['value'])
        self.assertEqual('通过', detail[6][16]['value'])
        self.assertEqual('阻塞', detail[5][16]['value'])
        self.assertEqual('Verified package absent', detail[5][23]['value'])
        explanation = next(r for n, r in book['原因与补测'].items() if n > 4 and r.get(1, {}).get('value') == 'ENV-002')
        self.assertEqual('Provide fixture package', explanation[7]['value'])
        self.assertEqual('未执行', detail[self.appendrow][16]['value'])
        self.assertFalse(detail[6][18]['formula'])
        self.assertTrue(detail[6][18]['value'].endswith('=1+1'))
        self.assertEqual(str(self.casecount + 1), book['评审总览'][5][2]['value'])
        self.assertEqual('1', book['评审总览'][6][2]['value'])
        self.assertEqual(str(self.casecount - 1), book['评审总览'][9][2]['value'])
        with zipfile.ZipFile(report['xlsxReport']) as z:
            tables = [ET.fromstring(z.read(name)) for name in z.namelist() if re.fullmatch(r'xl/tables/[^/]+\.xml', name)]
            self.assertEqual(f'A4:AC{self.appendrow}', next(t for t in tables if t.attrib['name'] == 'TestCases').attrib['ref'])
        self.assertEqual('失败', book['重测历史'][5][3]['value'])
        self.assertEqual(sum(len(c['steps']) + len(c['assertions']) for c in suite['cases']), len([r for n, r in book['步骤与检查结果'].items() if n > 4 and r.get(1, {}).get('value')]))
        with self.assertRaisesRegex(ValueError, 'Result workbooks'): j.load_suite(report['xlsxReport'])
        self.assertNotIn('fixture-secret', (run / 'report.html').read_text(encoding='utf-8'))
        self.assertFalse(report['allPass'])


if __name__ == '__main__': unittest.main()
