"""Quick casebook and ledger/report integration tests; no device access."""
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import customer_journey as journey
import customer_journey_quick as quick
import customer_journey_workbook as workbook


class QuickSuiteTests(unittest.TestCase):
    def test_catalog_is_frozen_from_workbook_with_independent_model_gates(self):
        suite = journey.load_suite(quick.QUICK_BOOK)
        self.assertEqual('quick', suite['suiteKind'])
        self.assertEqual(29, journey.validate_suite(suite))
        self.assertEqual(102, sum(len(c['steps']) for c in suite['cases']))
        by_id = {c['id']: c for c in suite['cases']}
        self.assertIn('VLM-MODEL', by_id['Q26']['requires'])
        self.assertIn('DINO-MODEL', by_id['Q27']['requires'])
        self.assertNotIn('DINO-MODEL', by_id['Q26']['requires'])
        self.assertIn('LIVE-002', by_id['Q28']['sourceCaseIds'])
        self.assertEqual(2, len(by_id['Q18']['steps']))
        self.assertIn('OPENAI-VLM', by_id['Q29']['requires'])
        self.assertIn('VLM-MODEL', by_id['Q29']['requires'])

    def test_case_recording_exports_by_quick_id_and_preserves_source(self):
        with tempfile.TemporaryDirectory(prefix='quick-journey-', dir=ROOT / 'output/agent-runs') as name:
            base = pathlib.Path(name)
            run = base / 'run'
            profile = base / 'profile.json'
            journey.write(profile, {'resources': {'DUT': {'status': 'available', 'host': 'fixture.invalid'}}})
            original = workbook.sha256(quick.QUICK_BOOK)
            journey.init_run(run, quick.QUICK_BOOK, profile)
            suite = journey.load_run(run)[2]
            case = next(c for c in suite['cases'] if c['id'] == 'Q01')
            evidence = ['evidence/fixture.json']
            journey.write(run / evidence[0], {'kind': 'synthetic tool test, not a device result'})
            measured = {'status': 'PASS', 'reasonCode': 'NONE', 'reason': 'Synthetic tool integration',
                        'actual': 'Synthetic fixture only', 'evidence': evidence,
                        'cleanup': {'status': 'NOT_REQUIRED', 'note': 'No device or UI was changed'}}
            for key in ('steps', 'assertions'):
                measured[key] = [{'id': item['id'], 'status': 'PASS', 'actual': 'Fixture observation',
                                  'evidence': evidence} for item in case[key]]
            journey.record(run, 'Q01', measured)
            blocked = {'status': 'BLOCKED', 'reasonCode': 'RESOURCE_MISSING',
                       'reason': 'Compatible VLM package not supplied in fixture',
                       'actual': 'Fixture resource check', 'evidence': evidence}
            journey.record(run, 'Q26', blocked)
            output = journey.report(run, xlsx=True)
            self.assertEqual(original, workbook.sha256(quick.QUICK_BOOK))
            self.assertEqual(1, output['counts']['PASS'])
            self.assertEqual(1, output['counts']['BLOCKED'])
            self.assertEqual(27, output['counts']['NOT_EXECUTED'])
            cells = workbook.workbook_cells(output['xlsxReport'])
            rows = cells['主要用例']
            quick_rows = {r[1]['value']: r for n, r in rows.items() if n > 4 and 1 in r}
            self.assertEqual('通过', quick_rows['Q01'][10]['value'])
            self.assertEqual('阻塞', quick_rows['Q26'][10]['value'])
            self.assertIn('Compatible VLM', quick_rows['Q26'][11]['value'])
            self.assertEqual('27', cells['评审总览'][9][5]['value'])
            with self.assertRaisesRegex(ValueError, 'result workbook'):
                journey.load_suite(output['xlsxReport'])


if __name__ == '__main__':
    unittest.main()
