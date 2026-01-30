"""
Pytest configuration and fixtures.
"""

import os
import sys
import pytest
from unittest.mock import Mock, MagicMock, patch
import tempfile
import shutil

# Add project paths to sys.path
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'python'))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'plugins'))

# Import mocks
from mocks import (
    MockSubprocess, MockWirelessInterface, MockDmesg, MockPwnagotchiAgent,
    MockFlaskApp, MockFileSystem, MockSystemInfo,
    create_healthy_interface_mock, create_failed_interface_mock,
    create_subprocess_mock_healthy, create_subprocess_mock_with_errors,
    create_dmesg_mock_with_errors,
)


# ============================================================================
# Fixtures for subprocess mocking
# ============================================================================

@pytest.fixture
def mock_subprocess():
    """Provide a MockSubprocess instance."""
    return MockSubprocess()

@pytest.fixture
def mock_subprocess_healthy():
    """Provide a MockSubprocess for healthy system."""
    return create_subprocess_mock_healthy()

@pytest.fixture
def mock_subprocess_errors():
    """Provide a MockSubprocess that returns errors."""
    return create_subprocess_mock_with_errors()


# ============================================================================
# Fixtures for wireless interface mocking
# ============================================================================

@pytest.fixture
def mock_interface():
    """Provide a MockWirelessInterface instance."""
    return MockWirelessInterface()

@pytest.fixture
def mock_interface_healthy():
    """Provide a healthy MockWirelessInterface."""
    return create_healthy_interface_mock()

@pytest.fixture
def mock_interface_failed():
    """Provide a failed MockWirelessInterface."""
    return create_failed_interface_mock()


# ============================================================================
# Fixtures for dmesg mocking
# ============================================================================

@pytest.fixture
def mock_dmesg():
    """Provide a MockDmesg instance."""
    return MockDmesg()

@pytest.fixture
def mock_dmesg_with_errors():
    """Provide a MockDmesg with error messages."""
    return create_dmesg_mock_with_errors()


# ============================================================================
# Fixtures for Pwnagotchi mocking
# ============================================================================

@pytest.fixture
def mock_agent():
    """Provide a MockPwnagotchiAgent instance."""
    return MockPwnagotchiAgent()


# ============================================================================
# Fixtures for Flask app mocking
# ============================================================================

@pytest.fixture
def mock_flask_app():
    """Provide a MockFlaskApp instance."""
    return MockFlaskApp()


# ============================================================================
# Fixtures for file system
# ============================================================================

@pytest.fixture
def mock_fs():
    """Provide a MockFileSystem instance."""
    return MockFileSystem()

@pytest.fixture
def temp_dir():
    """Provide a temporary directory."""
    path = tempfile.mkdtemp()
    yield path
    shutil.rmtree(path)

@pytest.fixture
def temp_firmware(temp_dir):
    """Provide a temporary firmware file."""
    firmware_path = os.path.join(temp_dir, 'test_firmware.bin')
    with open(firmware_path, 'wb') as f:
        f.write(b'\x00' * 0x200000)  # 2MB
    return firmware_path


# ============================================================================
# Fixtures for system info
# ============================================================================

@pytest.fixture
def mock_system_info():
    """Provide a MockSystemInfo instance."""
    return MockSystemInfo()


# ============================================================================
# Fixtures for configuration
# ============================================================================

@pytest.fixture
def default_config():
    """Provide default plugin configuration."""
    return {
        'nexmon_stability': {
            'enabled': True,
            'min_channel_interval': 0.5,
            'max_recovery_attempts': 3,
            'recovery_cooldown': 60,
            'monitor_interval': 30,
            'interface': 'wlan0mon',
            'web_ui_enabled': True,
        }
    }

@pytest.fixture
def test_config():
    """Provide test configuration with short intervals."""
    return {
        'nexmon_stability': {
            'enabled': True,
            'min_channel_interval': 0.1,
            'max_recovery_attempts': 2,
            'recovery_cooldown': 1,
            'monitor_interval': 1,
            'interface': 'wlan0mon',
            'web_ui_enabled': False,
        }
    }


# ============================================================================
# Fixtures for patch context managers
# ============================================================================

@pytest.fixture
def patch_subprocess(mock_subprocess_healthy):
    """Patch subprocess module with healthy mock."""
    with patch('subprocess.run', mock_subprocess_healthy.run):
        with patch('subprocess.check_output', mock_subprocess_healthy.check_output):
            yield mock_subprocess_healthy

@pytest.fixture
def patch_subprocess_errors(mock_subprocess_errors):
    """Patch subprocess module with error mock."""
    with patch('subprocess.run', mock_subprocess_errors.run):
        with patch('subprocess.check_output', mock_subprocess_errors.check_output):
            yield mock_subprocess_errors

@pytest.fixture
def patch_os_geteuid():
    """Patch os.geteuid to return root."""
    with patch('os.geteuid', return_value=0):
        yield

@pytest.fixture
def patch_os_geteuid_nonroot():
    """Patch os.geteuid to return non-root."""
    with patch('os.geteuid', return_value=1000):
        yield


# ============================================================================
# Pytest configuration
# ============================================================================

def pytest_configure(config):
    """Configure pytest markers."""
    config.addinivalue_line(
        "markers", "slow: mark test as slow to run"
    )
    config.addinivalue_line(
        "markers", "integration: mark test as integration test"
    )
    config.addinivalue_line(
        "markers", "requires_root: mark test as requiring root privileges"
    )


def pytest_collection_modifyitems(config, items):
    """Modify test collection based on markers."""
    # Skip slow tests unless --runslow is passed
    if not config.getoption("--runslow", default=False):
        skip_slow = pytest.mark.skip(reason="need --runslow option to run")
        for item in items:
            if "slow" in item.keywords:
                item.add_marker(skip_slow)


def pytest_addoption(parser):
    """Add custom command line options."""
    parser.addoption(
        "--runslow", action="store_true", default=False, 
        help="run slow tests"
    )
    parser.addoption(
        "--runintegration", action="store_true", default=False,
        help="run integration tests"
    )
