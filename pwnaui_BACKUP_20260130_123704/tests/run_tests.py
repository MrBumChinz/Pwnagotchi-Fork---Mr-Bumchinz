#!/usr/bin/env python3
"""
Test runner for the pwnaui test suite.

Usage:
    python run_tests.py              # Run all tests
    python run_tests.py -v           # Verbose output
    python run_tests.py -k channel   # Run tests matching 'channel'
    python run_tests.py --cov        # Run with coverage
"""

import os
import sys
import argparse
import unittest
import time

# Add project root to path
project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, project_root)
sys.path.insert(0, os.path.join(project_root, 'python'))
sys.path.insert(0, os.path.join(project_root, 'plugins'))


def discover_and_run_tests(verbosity=1, pattern='test_*.py', failfast=False):
    """Discover and run all tests."""
    test_dir = os.path.dirname(os.path.abspath(__file__))
    
    # Discover tests
    loader = unittest.TestLoader()
    suite = loader.discover(test_dir, pattern=pattern)
    
    # Count tests
    def count_tests(suite):
        count = 0
        for test in suite:
            if isinstance(test, unittest.TestSuite):
                count += count_tests(test)
            else:
                count += 1
        return count
    
    test_count = count_tests(suite)
    print(f"\n{'='*70}")
    print(f"Pwnaui Nexmon Stability Test Suite")
    print(f"{'='*70}")
    print(f"Discovered {test_count} tests")
    print(f"{'='*70}\n")
    
    # Run tests
    runner = unittest.TextTestRunner(verbosity=verbosity, failfast=failfast)
    start_time = time.time()
    result = runner.run(suite)
    elapsed = time.time() - start_time
    
    # Print summary
    print(f"\n{'='*70}")
    print(f"Test Summary")
    print(f"{'='*70}")
    print(f"Tests run: {result.testsRun}")
    print(f"Failures: {len(result.failures)}")
    print(f"Errors: {len(result.errors)}")
    print(f"Skipped: {len(result.skipped)}")
    print(f"Time: {elapsed:.2f}s")
    print(f"{'='*70}")
    
    if result.wasSuccessful():
        print("\n✅ All tests passed!")
        return 0
    else:
        print("\n❌ Some tests failed!")
        return 1


def run_specific_tests(test_modules, verbosity=1):
    """Run specific test modules."""
    suite = unittest.TestSuite()
    loader = unittest.TestLoader()
    
    for module_name in test_modules:
        try:
            module = __import__(module_name)
            suite.addTests(loader.loadTestsFromModule(module))
        except ImportError as e:
            print(f"Warning: Could not import {module_name}: {e}")
    
    runner = unittest.TextTestRunner(verbosity=verbosity)
    result = runner.run(suite)
    
    return 0 if result.wasSuccessful() else 1


def main():
    parser = argparse.ArgumentParser(description='Run pwnaui test suite')
    
    parser.add_argument(
        '-v', '--verbose', 
        action='count', 
        default=1,
        help='Increase verbosity (can be repeated)'
    )
    parser.add_argument(
        '-k', '--keyword',
        help='Only run tests matching keyword'
    )
    parser.add_argument(
        '--failfast', '-f',
        action='store_true',
        help='Stop on first failure'
    )
    parser.add_argument(
        '--module', '-m',
        action='append',
        help='Run specific test module(s)'
    )
    parser.add_argument(
        '--unit-only',
        action='store_true',
        help='Run only unit tests (exclude integration)'
    )
    parser.add_argument(
        '--integration-only',
        action='store_true', 
        help='Run only integration tests'
    )
    parser.add_argument(
        '--list', '-l',
        action='store_true',
        help='List available tests without running'
    )
    parser.add_argument(
        '--cov', '--coverage',
        action='store_true',
        help='Run with coverage (requires pytest-cov)'
    )
    
    args = parser.parse_args()
    
    # Handle coverage mode
    if args.cov:
        try:
            import pytest
            pytest_args = [
                'tests/',
                '-v' if args.verbose > 1 else '',
                '--cov=.',
                '--cov-report=term-missing',
                '--cov-report=html:coverage_report',
            ]
            pytest_args = [a for a in pytest_args if a]  # Remove empty strings
            return pytest.main(pytest_args)
        except ImportError:
            print("Coverage requires pytest and pytest-cov. Install with:")
            print("  pip install pytest pytest-cov")
            return 1
    
    # Handle list mode
    if args.list:
        test_dir = os.path.dirname(os.path.abspath(__file__))
        loader = unittest.TestLoader()
        suite = loader.discover(test_dir, pattern='test_*.py')
        
        print("\nAvailable Tests:")
        print("=" * 60)
        
        def print_tests(suite, indent=0):
            for test in suite:
                if isinstance(test, unittest.TestSuite):
                    print_tests(test, indent)
                else:
                    print(f"  {test}")
        
        print_tests(suite)
        return 0
    
    # Handle specific modules
    if args.module:
        return run_specific_tests(args.module, args.verbose)
    
    # Determine pattern based on test type filter
    pattern = 'test_*.py'
    if args.unit_only:
        pattern = 'test_[!i]*.py'  # Exclude test_integration.py
    elif args.integration_only:
        pattern = 'test_integration.py'
    
    # Handle keyword filtering
    if args.keyword:
        # Use pytest for keyword filtering
        try:
            import pytest
            pytest_args = [
                'tests/',
                '-k', args.keyword,
                '-v' if args.verbose > 1 else '',
            ]
            pytest_args = [a for a in pytest_args if a]
            return pytest.main(pytest_args)
        except ImportError:
            print("Keyword filtering requires pytest. Install with: pip install pytest")
            return 1
    
    # Run all tests
    return discover_and_run_tests(
        verbosity=args.verbose,
        pattern=pattern,
        failfast=args.failfast
    )


if __name__ == '__main__':
    sys.exit(main())
