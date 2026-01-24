#!/usr/bin/env python3
"""
PwnaUI Python Test Runner

Runs all Python unit tests and generates a summary report.
"""

import unittest
import sys
import os
import time
from io import StringIO

# Add paths
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'python'))

# Color codes
GREEN = '\033[92m'
RED = '\033[91m'
BLUE = '\033[94m'
YELLOW = '\033[93m'
RESET = '\033[0m'


def run_tests():
    """Run all Python tests and return results."""
    print()
    print("╔═══════════════════════════════════════════════════════════╗")
    print("║           PwnaUI Python Unit Test Suite                   ║")
    print("║                                                           ║")
    print("║  Testing: pwnaui_client, pwnaui_view                      ║")
    print("╚═══════════════════════════════════════════════════════════╝")
    print()
    
    # Discover and run tests
    loader = unittest.TestLoader()
    suite = unittest.TestSuite()
    
    # Get the tests directory
    tests_dir = os.path.dirname(os.path.abspath(__file__))
    
    # Load test modules
    test_modules = [
        'test_pwnaui_client',
        'test_pwnaui_view'
    ]
    
    for module_name in test_modules:
        try:
            # Try to discover tests from file
            test_file = os.path.join(tests_dir, f"{module_name}.py")
            if os.path.exists(test_file):
                discovered = loader.discover(tests_dir, pattern=f"{module_name}.py")
                suite.addTests(discovered)
                print(f"{BLUE}Loaded:{RESET} {module_name}")
        except Exception as e:
            print(f"{RED}Error loading {module_name}: {e}{RESET}")
    
    print()
    print(f"{BLUE}═══ Running Tests ═══{RESET}")
    print()
    
    # Run with verbosity
    runner = unittest.TextTestRunner(verbosity=2)
    start_time = time.time()
    result = runner.run(suite)
    elapsed = time.time() - start_time
    
    # Print summary
    print()
    print(f"{BLUE}═══════════════════════════════════════{RESET}")
    print(f"{BLUE}           TEST SUMMARY{RESET}")
    print(f"{BLUE}═══════════════════════════════════════{RESET}")
    
    total = result.testsRun
    failures = len(result.failures)
    errors = len(result.errors)
    skipped = len(result.skipped)
    passed = total - failures - errors - skipped
    
    print(f"  Tests run:   {total}")
    if passed > 0:
        print(f"  {GREEN}Passed:      {passed}{RESET}")
    if failures > 0:
        print(f"  {RED}Failures:    {failures}{RESET}")
    if errors > 0:
        print(f"  {RED}Errors:      {errors}{RESET}")
    if skipped > 0:
        print(f"  {YELLOW}Skipped:     {skipped}{RESET}")
    print(f"  Time:        {elapsed:.2f}s")
    print(f"{BLUE}═══════════════════════════════════════{RESET}")
    print()
    
    if result.wasSuccessful():
        print(f"{GREEN}All tests passed! ✓{RESET}")
    else:
        print(f"{RED}Some tests failed! ✗{RESET}")
        
        # Print failure details
        if result.failures:
            print(f"\n{RED}Failures:{RESET}")
            for test, trace in result.failures:
                print(f"  - {test}")
                
        if result.errors:
            print(f"\n{RED}Errors:{RESET}")
            for test, trace in result.errors:
                print(f"  - {test}")
    
    print()
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(run_tests())
