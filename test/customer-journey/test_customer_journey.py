import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
spec = importlib.util.spec_from_file_location('journey', ROOT / 'tools/customer_journey.py')
j = importlib.util.module_from_spec(spec)
spec.loader.exec_module(j)


class JourneyTests(unittest.TestCase):
    def setUp(self):
        base = ROOT / 'output/agent-runs'
        base.mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(prefix='journey-unit-', dir=base)
        self.base = pathlib.Path(self.tmp.name)
        self.run = self.base / 'run'
        self.suite = j.load_suite(j.DEFAULT_SUITE)
        self.suite['cases'] = self.suite['cases'][:2]
        self.case = self.suite['cases'][0]
        j.write(self.base / 'suite.json', self.suite)
        self.profile = {'resources': {'DUT': {'host': 'example.test', 'username': 'admin', 'password': 'test-secret'}}}
        j.write(self.base / 'profile.json', self.profile)
        j.init_run(self.run, self.base / 'suite.json', self.base / 'profile.json')
        j.write(self.run / 'evidence/check.json', {'observation': 'fixture only; not a device test'})

    def tearDown(self):
        self.tmp.cleanup()

    def passing(self):
        evidence = ['evidence/check.json']
        r = {'status': 'PASS', 'reasonCode': 'NONE', 'reason': 'All checks measured', 'actual': 'fixture result', 'evidence': evidence,
             'cleanup': {'status': 'VERIFIED', 'note': 'fixture cleanup', 'evidence': evidence}}
        for kind in ('steps', 'assertions'):
            r[kind] = [{'id': x['id'], 'status': 'PASS', 'actual': 'fixture observation', 'evidence': evidence} for x in self.case[kind]]
        return r

    def test_default_catalog_unique_and_matrices_required(self):
        suite = j.load_suite(j.DEFAULT_SUITE)
        self.assertEqual(len(suite['cases']), j.validate_suite(suite))
        self.assertGreaterEqual(len(suite['cases']), 312)
        assertions = [a for c in suite['cases'] for a in c['assertions']]
        self.assertGreaterEqual(sum('.PAR-' in a['id'] for a in assertions), 581)
        self.assertGreaterEqual(sum('.APIREF-' in a['id'] for a in assertions), 366)

    def test_missing_step_cannot_pass(self):
        r = self.passing(); r['steps'].pop()
        with self.assertRaisesRegex(ValueError, 'every step'):
            j.record(self.run, self.case['id'], r)
        self.assertEqual('NOT_EXECUTED', j.read(self.run / 'results.json')[self.case['id']]['status'])

    def test_missing_assertion_and_cleanup_cannot_pass(self):
        r = self.passing(); r['assertions'] = []
        with self.assertRaises(ValueError): j.record(self.run, self.case['id'], r)
        r = self.passing(); r['cleanup']['status'] = 'PENDING'
        with self.assertRaises(ValueError): j.record(self.run, self.case['id'], r)

    def test_evidenced_inapplicable_step_does_not_block_measured_case(self):
        r = self.passing()
        r['steps'][0].update(status='NOT_APPLICABLE', actual='Fixture version has no separate confirmation step')
        j.record(self.run, self.case['id'], r)
        saved = j.read(self.run / 'results.json')[self.case['id']]
        self.assertEqual('PASS', saved['status'])
        self.assertEqual('NOT_APPLICABLE', saved['steps'][0]['status'])
        for field in ('actual', 'evidence'):
            invalid = self.passing()
            invalid['steps'][0].update(status='NOT_APPLICABLE')
            invalid['steps'][0].pop(field)
            with self.assertRaises(ValueError): j.record(self.run, self.case['id'], invalid)

    def test_unexecuted_blocked_or_entirely_inapplicable_steps_cannot_pass(self):
        for status in ('NOT_EXECUTED', 'BLOCKED', 'FAIL'):
            r = self.passing(); r['steps'][0]['status'] = status
            with self.assertRaises(ValueError): j.record(self.run, self.case['id'], r)
        r = self.passing()
        for row in r['steps']: row['status'] = 'NOT_APPLICABLE'
        with self.assertRaisesRegex(ValueError, 'at least one measured step'):
            j.record(self.run, self.case['id'], r)

    def test_evidence_must_exist_and_cannot_escape_run(self):
        r = self.passing(); r['evidence'] = ['missing.json']
        with self.assertRaises(ValueError): j.record(self.run, self.case['id'], r)
        r['evidence'] = ['../profile.json']
        with self.assertRaises(ValueError): j.record(self.run, self.case['id'], r)

    def test_fail_requires_identified_failure(self):
        r = self.passing(); r['status'] = 'FAIL'; r['reasonCode'] = 'PRODUCT_DEFECT'
        with self.assertRaisesRegex(ValueError, 'identify'): j.record(self.run, self.case['id'], r)

    def test_retry_preserves_failure_and_resume_queue(self):
        r = self.passing(); r['status'] = 'FAIL'; r['reasonCode'] = 'PRODUCT_DEFECT'; r['steps'][0]['status'] = 'FAIL'
        j.record(self.run, self.case['id'], r)
        j.record(self.run, self.case['id'], self.passing())
        state = j.read(self.run / 'results.json')[self.case['id']]
        self.assertEqual('FAIL', state['attempts'][0]['status'])
        self.assertEqual(self.suite['cases'][1]['id'], j.queue(self.run, 1)[0]['case']['id'])
        self.assertFalse(j.report(self.run)['allPass'])

    def test_frozen_suite_tampering_rejected(self):
        s = j.read(self.run / 'suite.snapshot.json'); s['cases'][0]['expected'] = 'changed'
        j.write(self.run / 'suite.snapshot.json', s)
        with self.assertRaisesRegex(ValueError, 'Frozen suite'): j.load_run(self.run)

    def test_profile_change_is_explicit_and_no_secrets_reported(self):
        self.profile['resources']['DUT']['password'] = 'new-secret'
        j.write(self.base / 'profile.json', self.profile)
        with self.assertRaisesRegex(ValueError, 'Profile changed'): j.queue(self.run, 1)
        j.refresh_profile(self.run)
        self.assertEqual(1, len(j.read(self.run / 'run.json')['profileChanges']))
        j.report(self.run)
        self.assertNotIn('new-secret', (self.run / 'report.html').read_text(encoding='utf-8'))

    def test_preflight_missing_local_file_and_case_mapping(self):
        self.profile['resources']['FACE-ZIP'] = {'status': 'available', 'path': str(self.base / 'absent.zip')}
        j.write(self.base / 'profile.json', self.profile); j.refresh_profile(self.run)
        data = j.preflight(self.run)
        gap = next(x for x in data['gaps'] if x['id'] == 'FACE-ZIP')
        self.assertEqual('missing', gap['status'])

    def test_not_applicable_needs_evidence(self):
        r = {'status': 'NOT_APPLICABLE', 'reasonCode': 'NOT_APPLICABLE', 'reason': 'capability absent', 'actual': 'unsupported'}
        with self.assertRaises(ValueError): j.record(self.run, self.case['id'], r)

    def test_html_escapes_observation_and_preserves_case_id(self):
        r = self.passing(); r['actual'] = '<script>alert(1)</script>'
        j.record(self.run, self.case['id'], r)
        text = (self.run / 'report.html').read_text(encoding='utf-8')
        self.assertIn('&lt;script&gt;', text)
        self.assertNotIn('<script>alert', text)
        self.assertIn(self.case['id'], text)

    def test_blocked_is_not_complete_or_pass(self):
        for c in self.suite['cases']:
            j.record(self.run, c['id'], {'status': 'BLOCKED', 'reasonCode': 'RESOURCE_MISSING', 'reason': 'package missing', 'actual': 'checked profile'})
        result = j.report(self.run)
        self.assertFalse(result['allPass']); self.assertFalse(result['fullyExecuted'])
        self.assertEqual([], j.queue(self.run, 1))

    def test_version_evidence_and_upgrade_preserve_recorded_version(self):
        with self.assertRaises(ValueError): j.set_device_version(self.run, 'fixture-v1', 'missing.json')
        j.set_device_version(self.run, 'fixture-v1', 'evidence/check.json')
        j.record(self.run, self.case['id'], self.passing())
        j.set_device_version(self.run, 'fixture-v2', 'evidence/check.json')
        self.assertEqual('fixture-v1', j.read(self.run / 'results.json')[self.case['id']]['deviceVersion'])
        self.assertEqual('fixture-v2', j.load_run(self.run)[1]['deviceVersion'])

    def test_explanation_preserves_measurement_and_requires_action_and_evidence(self):
        before = j.read(self.run / 'results.json')[self.case['id']]
        analysis = {'category': '执行覆盖缺口', 'cause': 'No execution evidence; historical stop cause unknown',
                    'certainty': 'UNKNOWN', 'remaining': 'Perform the three fixture steps',
                    'nextAction': 'Execute fixture step 1', 'owner': 'Codex', 'evidence': ['evidence/check.json']}
        bad = dict(analysis, nextAction='')
        with self.assertRaisesRegex(ValueError, 'nextAction'): j.explain(self.run, self.case['id'], bad)
        with self.assertRaises(ValueError): j.explain(self.run, self.case['id'], dict(analysis, evidence=['missing.json']))
        j.explain(self.run, self.case['id'], analysis)
        after = j.read(self.run / 'results.json')[self.case['id']]
        for key, value in before.items(): self.assertEqual(value, after[key])
        report = j.report(self.run)
        self.assertEqual([self.suite['cases'][1]['id']], report['unexplainedCases'])
        self.assertEqual([self.case['id']], report['unknownCauseCases'])
        self.assertIn('Execute fixture step 1', (self.run / 'report.html').read_text(encoding='utf-8'))


if __name__ == '__main__':
    unittest.main()
