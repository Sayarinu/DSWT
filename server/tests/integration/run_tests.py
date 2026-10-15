"""Run a Python integration module against the supplied server executable."""

import argparse
import sys
import unittest

from server_fixture import ServerTestCase


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("server_binary")
    parser.add_argument("test_module")
    args = parser.parse_args()
    ServerTestCase.server_binary = args.server_binary
    suite = unittest.defaultTestLoader.loadTestsFromName(args.test_module)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
