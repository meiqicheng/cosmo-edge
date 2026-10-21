import copy
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / 'tools'))
import customer_journey as journey
import customer_journey_resources as kit


class PortableResourcesTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(dir=REPO / 'output/agent-runs', prefix='portable-unit-')
        self.base = Path(self.tmp.name)
        self.root = self.base / 'old-root'
        self.asset = self.root / 'assets/people/image.dat'
        self.asset.parent.mkdir(parents=True)
        self.asset.write_bytes(b'original')
        self.profile = {'portableVersion': 1, 'kitId': 'unit-kit', 'resources': {
            'DUT': {'status': 'available', 'host': 'example.invalid', 'username': 'admin', 'password': 'private-secret', 'checkPorts': [80]},
            'FACE-ZIP': {'status': 'available', 'path': 'assets/people/image.dat'},
            'PEOPLE-MEDIA': {'status': 'available', 'path': 'assets/people'},
            'PRECISION-SERVER': {'workspace': '/remote/workspace', 'script': './run_precision.sh'}}}
        kit.save(self.root / kit.PROFILE, self.profile)
        kit.save(self.root / kit.MANIFEST, {'schemaVersion': 1, 'kitId': 'unit-kit', 'files': [
            {'path': 'assets/people/image.dat', 'size': 8, 'sha256': kit.file_hash(self.asset),
             'resources': ['FACE-ZIP', 'PEOPLE-MEDIA']}]})
        self.definitions = {
            'DUT': {'kind': 'device', 'title': 'Device', 'fields': ['host', 'username', 'password']},
            'FACE-ZIP': {'kind': 'file', 'title': 'Face'},
            'PEOPLE-MEDIA': {'kind': 'directory', 'title': 'People'}}
        self.cases = [{'id': 'MEDIA-001', 'requires': ['FACE-ZIP', 'PEOPLE-MEDIA']}]
        self.env = patch.dict(os.environ, {'COSMO_JOURNEY_RESOURCE_ROOT': ''})
        self.env.start()

    def tearDown(self):
        self.env.stop()
        self.tmp.cleanup()

    def test_copy_to_new_root_requires_only_root_configuration(self):
        moved = self.base / 'new machine/中文资源包'
        shutil.copytree(self.root, moved)
        shutil.rmtree(self.root)
        repo = self.base / 'new-repo'
        kit.configure(repo, moved)
        self.assertEqual(moved.resolve(), kit.configured_root(repo))
        profile = kit.resolve_profile(kit.read(kit.default_profile(repo)), kit.configured_root(repo))
        self.assertTrue(Path(profile['resources']['FACE-ZIP']['path']).is_file())
        self.assertEqual('/remote/workspace', profile['resources']['PRECISION-SERVER']['workspace'])
        self.assertTrue(kit.inspect_manifest(moved, full=True)['verified'])

    def test_absolute_paths_and_traversal_are_rejected(self):
        for value in ['../secret', '/etc/passwd', 'C:\\private\\file', 'C:relative', '\\\\host\\share\\file']:
            with self.subTest(value=value), self.assertRaises(ValueError):
                kit.within(self.root, value)

    def test_missing_child_detected_even_when_directory_exists(self):
        self.asset.unlink()
        (self.asset.parent / 'different.dat').write_bytes(b'present')
        report = kit.inspect_resources(self.profile, self.definitions, self.cases, root=self.root)
        self.assertFalse(report['integrity']['verified'])
        row = next(r for r in report['gaps'] if r['id'] == 'PEOPLE-MEDIA')
        self.assertEqual(['MEDIA-001'], row['affectedCases'])
        self.assertEqual('invalid', row['status'])
        self.assertNotIn('private-secret', json.dumps(report))

    def test_full_hash_detects_same_size_corruption(self):
        self.asset.write_bytes(b'changed!')
        self.assertTrue(kit.inspect_manifest(self.root)['verified'])
        self.assertEqual('hash_mismatch', kit.inspect_manifest(self.root, full=True)['problems'][0]['reason'])

    def test_network_is_opt_in_and_tcp_is_not_business_acceptance(self):
        with patch.object(kit.socket, 'create_connection', side_effect=OSError('sensitive hostname')) as probe:
            kit.inspect_resources(self.profile, self.definitions, self.cases, root=self.root)
            probe.assert_not_called()
            report = kit.inspect_resources(self.profile, self.definitions, self.cases, root=self.root, network=True)
            self.assertEqual('unreachable', report['network'][0]['status'])
            self.assertNotIn('sensitive hostname', json.dumps(report))

    def test_legacy_profile_unchanged_and_wrong_kit_rejected(self):
        legacy = {'resources': {'FILE': {'path': 'D:/old/file'}}}
        self.assertEqual(legacy, kit.resolve_profile(legacy))
        meta = {'resourceRoot': str(self.root), 'resourceKitId': 'other-kit'}
        with self.assertRaisesRegex(ValueError, 'differs'):
            kit.root_for_run(self.base / 'empty-repo', meta)

    def test_portable_run_and_profile_survive_repo_and_kit_move(self):
        suite_path = REPO / 'test/customer-journey/CosmoEdge-客户旅程测试用例.xlsx'
        old_repo = self.base / 'old-repo'
        old_run = old_repo / 'output/agent-runs/run'
        with patch.object(journey, 'ROOT', old_repo):
            journey.init_run(old_run, suite_path, self.root / kit.PROFILE)
        new_repo = self.base / 'new-repo'
        shutil.move(str(old_repo), new_repo)
        new_root = self.base / 'new-root'
        shutil.move(str(self.root), new_root)
        kit.configure(new_repo, new_root)
        run = new_repo / 'output/agent-runs/run'
        with patch.object(journey, 'ROOT', new_repo):
            report = journey.preflight(run, full=True)
            self.assertTrue(report['integrity']['verified'])
            self.assertTrue(journey.queue(run, 1))
            meta = journey.read(run / 'run.json')
            self.assertEqual('resources.private.json', meta['profilePath'])
            original = journey.load_profile(meta, run)
            # Editing shared defaults must not silently change a historical run.
            current = kit.read(new_root / kit.PROFILE)
            current['resources']['DUT']['password'] = 'replacement-secret'
            kit.save(new_root / kit.PROFILE, current)
            self.assertEqual(original, journey.load_profile(meta, run))


if __name__ == '__main__':
    unittest.main()
