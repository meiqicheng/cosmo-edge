"""Private kit launcher; copy to kit.py alongside runtime/*.py.

The kit carries compatible ledger tools. The casebook comes from the selected
repository and is never independently maintained inside the kit.
"""
from pathlib import Path
import argparse
import os
import sys


def main():
    root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description='Private CosmoEdge resource kit')
    parser.add_argument('--repo', type=Path, default=Path.cwd(), help='CosmoEdge repository (default: current directory)')
    parser.add_argument('command', help='configure, check, or a customer_journey.py command')
    args, rest = parser.parse_known_args()
    repo = args.repo.resolve()
    casebook = repo / 'test/customer-journey/CosmoEdge-客户旅程测试用例.xlsx'
    if not casebook.is_file():
        parser.error('Run from the CosmoEdge repository, or supply --repo')
    if not (root / 'resources.private.json').is_file():
        parser.error('Run kit.py inside the private resource kit')
    sys.path.insert(0, str(root / 'runtime'))
    import customer_journey as journey
    journey.ROOT = repo
    journey.DEFAULT_SUITE = casebook
    journey.GENERATED_SUITE = repo / 'test/customer-journey/suite.json'
    os.chdir(repo)
    commands = {'configure': ['configure-resources', '--root', str(root)],
                'check': ['check-resources', '--root', str(root)]}
    sys.argv = [str(root / 'kit.py')] + commands.get(args.command, [args.command]) + rest
    if args.command == 'init' and '--profile' not in rest:
        sys.argv += ['--profile', str(root / 'resources.private.json')]
    journey.main()


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, StopIteration) as exc:
        print('error: ' + str(exc), file=sys.stderr)
        sys.exit(2)
