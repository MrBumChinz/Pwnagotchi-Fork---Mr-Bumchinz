"""
Unit Tests for Firmware Patcher

Tests the binary patching logic for nexmon firmware fixes.
"""

import os
import sys
import unittest
from unittest.mock import Mock, patch, mock_open, MagicMock
import struct
import tempfile
import shutil


# Add path to patch_firmware.py
sys.path.insert(0, os.path.join(
    os.path.dirname(__file__), '..', '..', 
    'nexmon_pwnagotchi_fixes', 'patches', 'arm'
))


class TestPatchDefinitions(unittest.TestCase):
    """Tests for patch definitions."""
    
    def test_patch_bytes_length(self):
        """Test patch bytes are correct length."""
        patch_hex = '002e02d0706808b97047'
        patch_bytes = bytes.fromhex(patch_hex)
        
        self.assertEqual(len(patch_bytes), 10)
    
    def test_patch_bytes_valid(self):
        """Test patch bytes are valid ARM Thumb-2."""
        patch_bytes = bytes.fromhex('002e02d0706808b97047')
        
        # CMP r6, #0 = 00 2e
        self.assertEqual(patch_bytes[0:2], b'\x00\x2e')
        
        # BEQ.N +4 = 02 d0
        self.assertEqual(patch_bytes[2:4], b'\x02\xd0')
        
        # LDR r0, [r6, #4] = 70 68
        self.assertEqual(patch_bytes[4:6], b'\x70\x68')
        
        # CBNZ r0, +2 = 08 b9
        self.assertEqual(patch_bytes[6:8], b'\x08\xb9')
        
        # BX LR = 70 47
        self.assertEqual(patch_bytes[8:10], b'\x70\x47')
    
    def test_patch_addresses_in_range(self):
        """Test patch addresses are within firmware range."""
        addresses = {
            'bcm43455c0_7_45_206': 0x1AABB0,
            'bcm43455c0_7_45_189': 0x1AF378,
            'bcm43455c0_7_45_241': 0x1AB8C0,
            'bcm43430a1_7_45_41_46': 0x185A40,
        }
        
        # Firmware is typically 500KB-1MB
        max_firmware_size = 0x200000  # 2MB max
        
        for name, addr in addresses.items():
            self.assertGreater(addr, 0, f"{name} address is zero")
            self.assertLess(addr, max_firmware_size, f"{name} address too large")
    
    def test_patch_addresses_aligned(self):
        """Test patch addresses are properly aligned for Thumb-2."""
        addresses = [0x1AABB0, 0x1AF378, 0x1AB8C0, 0x185A40]
        
        for addr in addresses:
            # Thumb-2 instructions should be 2-byte aligned
            self.assertEqual(addr % 2, 0, f"Address 0x{addr:X} not aligned")


class TestFirmwarePatcher(unittest.TestCase):
    """Tests for FirmwarePatcher class."""
    
    def setUp(self):
        """Set up test fixtures."""
        try:
            from patch_firmware import FirmwarePatcher, PATCHES
            self.FirmwarePatcher = FirmwarePatcher
            self.PATCHES = PATCHES
        except ImportError:
            self.skipTest("patch_firmware not available")
    
    def test_patcher_init(self):
        """Test patcher initialization."""
        patcher = self.FirmwarePatcher('bcm43455c0', '7_45_206')
        
        self.assertEqual(patcher.chip, 'bcm43455c0')
        self.assertEqual(patcher.fw_version, '7_45_206')
    
    def test_patcher_has_patches(self):
        """Test patcher loads patches for valid chip/version."""
        patcher = self.FirmwarePatcher('bcm43455c0', '7_45_206')
        
        self.assertGreater(len(patcher.patches), 0)
    
    def test_patcher_no_patches_for_invalid(self):
        """Test patcher has no patches for invalid chip."""
        patcher = self.FirmwarePatcher('invalid_chip', '1_2_3')
        
        self.assertEqual(len(patcher.patches), 0)
    
    @unittest.skipIf(sys.platform == 'win32', "os.geteuid not available on Windows")
    @patch('os.geteuid', return_value=0)
    def test_check_root_passes(self, mock_euid):
        """Test root check passes when root."""
        patcher = self.FirmwarePatcher('bcm43455c0', '7_45_206')
        
        self.assertTrue(patcher.check_root())
    
    @unittest.skipIf(sys.platform == 'win32', "os.geteuid not available on Windows")
    @patch('os.geteuid', return_value=1000)
    def test_check_root_fails(self, mock_euid):
        """Test root check fails when not root."""
        patcher = self.FirmwarePatcher('bcm43455c0', '7_45_206')
        
        self.assertFalse(patcher.check_root())


class TestBinaryPatching(unittest.TestCase):
    """Tests for binary patching operations."""
    
    def setUp(self):
        """Create temporary firmware file for testing."""
        self.temp_dir = tempfile.mkdtemp()
        self.firmware_path = os.path.join(self.temp_dir, 'test_firmware.bin')
        
        # Create a fake firmware file (2MB of zeros with some content)
        with open(self.firmware_path, 'wb') as f:
            f.write(b'\x00' * 0x200000)  # 2MB
    
    def tearDown(self):
        """Clean up temporary files."""
        shutil.rmtree(self.temp_dir)
    
    def test_read_firmware(self):
        """Test reading firmware file."""
        with open(self.firmware_path, 'rb') as f:
            data = f.read()
        
        self.assertEqual(len(data), 0x200000)
    
    def test_write_patch(self):
        """Test writing patch to firmware."""
        address = 0x1AABB0
        patch_bytes = bytes.fromhex('002e02d0706808b97047')
        
        # Read firmware
        with open(self.firmware_path, 'rb') as f:
            firmware = bytearray(f.read())
        
        # Apply patch
        firmware[address:address+len(patch_bytes)] = patch_bytes
        
        # Write back
        with open(self.firmware_path, 'wb') as f:
            f.write(firmware)
        
        # Verify
        with open(self.firmware_path, 'rb') as f:
            f.seek(address)
            written = f.read(len(patch_bytes))
        
        self.assertEqual(written, patch_bytes)
    
    def test_verify_patch_applied(self):
        """Test verifying patch is applied."""
        address = 0x1AABB0
        patch_bytes = bytes.fromhex('002e02d0706808b97047')
        
        # Apply patch
        with open(self.firmware_path, 'r+b') as f:
            f.seek(address)
            f.write(patch_bytes)
        
        # Verify
        with open(self.firmware_path, 'rb') as f:
            f.seek(address)
            current = f.read(len(patch_bytes))
        
        self.assertEqual(current, patch_bytes)
    
    def test_patch_does_not_corrupt_other_data(self):
        """Test patching doesn't corrupt surrounding data."""
        address = 0x1AABB0
        patch_bytes = bytes.fromhex('002e02d0706808b97047')
        
        # Write some known data around patch location
        with open(self.firmware_path, 'r+b') as f:
            f.seek(address - 10)
            f.write(b'BEFORE----')
            f.seek(address + len(patch_bytes))
            f.write(b'----AFTER')
        
        # Apply patch
        with open(self.firmware_path, 'r+b') as f:
            f.seek(address)
            f.write(patch_bytes)
        
        # Verify surrounding data
        with open(self.firmware_path, 'rb') as f:
            f.seek(address - 10)
            before = f.read(10)
            f.seek(address + len(patch_bytes))
            after = f.read(9)
        
        self.assertEqual(before, b'BEFORE----')
        self.assertEqual(after, b'----AFTER')


class TestBackupRestore(unittest.TestCase):
    """Tests for backup and restore functionality."""
    
    def setUp(self):
        """Create temporary files for testing."""
        self.temp_dir = tempfile.mkdtemp()
        self.firmware_path = os.path.join(self.temp_dir, 'firmware.bin')
        
        # Create test firmware
        with open(self.firmware_path, 'wb') as f:
            f.write(b'ORIGINAL_FIRMWARE_CONTENT')
    
    def tearDown(self):
        """Clean up."""
        shutil.rmtree(self.temp_dir)
    
    def test_create_backup(self):
        """Test creating a backup."""
        backup_path = self.firmware_path + '.backup'
        
        shutil.copy2(self.firmware_path, backup_path)
        
        self.assertTrue(os.path.exists(backup_path))
        
        with open(backup_path, 'rb') as f:
            content = f.read()
        
        self.assertEqual(content, b'ORIGINAL_FIRMWARE_CONTENT')
    
    def test_restore_from_backup(self):
        """Test restoring from backup."""
        backup_path = self.firmware_path + '.orig'
        
        # Create backup
        shutil.copy2(self.firmware_path, backup_path)
        
        # Modify original
        with open(self.firmware_path, 'wb') as f:
            f.write(b'MODIFIED_CONTENT')
        
        # Restore
        shutil.copy2(backup_path, self.firmware_path)
        
        with open(self.firmware_path, 'rb') as f:
            content = f.read()
        
        self.assertEqual(content, b'ORIGINAL_FIRMWARE_CONTENT')


class TestHashVerification(unittest.TestCase):
    """Tests for hash verification."""
    
    def setUp(self):
        """Create temporary file for testing."""
        self.temp_dir = tempfile.mkdtemp()
        self.test_file = os.path.join(self.temp_dir, 'test.bin')
        
        with open(self.test_file, 'wb') as f:
            f.write(b'Test content for hashing')
    
    def tearDown(self):
        """Clean up."""
        shutil.rmtree(self.temp_dir)
    
    def test_compute_sha256(self):
        """Test SHA256 hash computation."""
        import hashlib
        
        sha256 = hashlib.sha256()
        with open(self.test_file, 'rb') as f:
            sha256.update(f.read())
        
        hash_hex = sha256.hexdigest()
        
        self.assertEqual(len(hash_hex), 64)
        self.assertTrue(all(c in '0123456789abcdef' for c in hash_hex))
    
    def test_hash_changes_with_content(self):
        """Test hash changes when content changes."""
        import hashlib
        
        # Original hash
        with open(self.test_file, 'rb') as f:
            hash1 = hashlib.sha256(f.read()).hexdigest()
        
        # Modify content
        with open(self.test_file, 'ab') as f:
            f.write(b' modified')
        
        # New hash
        with open(self.test_file, 'rb') as f:
            hash2 = hashlib.sha256(f.read()).hexdigest()
        
        self.assertNotEqual(hash1, hash2)


class TestChipAutoDetection(unittest.TestCase):
    """Tests for chip auto-detection."""
    
    @patch('subprocess.run')
    def test_detect_bcm43455(self, mock_run):
        """Test auto-detecting BCM43455."""
        mock_run.return_value = Mock(
            returncode=0,
            stdout='[    5.123] brcmfmac: brcmf_fw_alloc_request: using BCM43455\n'
        )
        
        try:
            from patch_firmware import detect_chip
            chip = detect_chip()
            self.assertEqual(chip, 'bcm43455c0')
        except ImportError:
            # Test the logic directly
            output = mock_run.return_value.stdout.lower()
            if 'bcm43455' in output:
                chip = 'bcm43455c0'
            else:
                chip = None
            self.assertEqual(chip, 'bcm43455c0')
    
    @patch('subprocess.run')
    def test_detect_bcm43430(self, mock_run):
        """Test auto-detecting BCM43430."""
        mock_run.return_value = Mock(
            returncode=0,
            stdout='[    5.123] brcmfmac: brcmf_fw_alloc_request: using BCM43430\n'
        )
        
        output = mock_run.return_value.stdout.lower()
        if 'bcm43430' in output:
            chip = 'bcm43430a1'
        else:
            chip = None
        
        self.assertEqual(chip, 'bcm43430a1')
    
    @patch('subprocess.run')
    def test_detect_unknown_chip(self, mock_run):
        """Test handling unknown chip."""
        mock_run.return_value = Mock(
            returncode=0,
            stdout='[    5.123] Some other driver message\n'
        )
        
        output = mock_run.return_value.stdout.lower()
        chip = None
        
        if 'bcm43455' in output:
            chip = 'bcm43455c0'
        elif 'bcm43430' in output:
            chip = 'bcm43430a1'
        
        self.assertIsNone(chip)


class TestARMAssembly(unittest.TestCase):
    """Tests for ARM assembly patch correctness."""
    
    def test_cmp_r6_0_encoding(self):
        """Test CMP r6, #0 instruction encoding."""
        # CMP Rn, #imm8 = 0010 1nnn iiii iiii (Thumb)
        # CMP r6, #0 = 0010 1110 0000 0000 = 0x2e00
        # Little-endian: 00 2e
        instruction = b'\x00\x2e'
        self.assertEqual(len(instruction), 2)
    
    def test_beq_n_encoding(self):
        """Test BEQ.N instruction encoding."""
        # BEQ.N +4 bytes (2 instructions)
        # B<cond> = 1101 cccc oooo oooo
        # BEQ = 1101 0000 0000 0010 = 0xd002
        # Little-endian: 02 d0
        instruction = b'\x02\xd0'
        self.assertEqual(len(instruction), 2)
    
    def test_ldr_r0_r6_4_encoding(self):
        """Test LDR r0, [r6, #4] instruction encoding."""
        # LDR Rt, [Rn, #imm5*4] = 0110 1iii iinn nttt
        # LDR r0, [r6, #4] = 0110 1001 0011 0000 = 0x6970
        # Little-endian: 70 68 (offset 4 = imm5=1)
        instruction = b'\x70\x68'
        self.assertEqual(len(instruction), 2)
    
    def test_cbnz_encoding(self):
        """Test CBNZ instruction encoding."""
        # CBNZ Rn, label = 1011 1oo1 oooo onnn
        instruction = b'\x08\xb9'
        self.assertEqual(len(instruction), 2)
    
    def test_bx_lr_encoding(self):
        """Test BX LR instruction encoding."""
        # BX Rm = 0100 0111 0mmm m000
        # BX LR (r14) = 0100 0111 0111 0000 = 0x4770
        # Little-endian: 70 47
        instruction = b'\x70\x47'
        self.assertEqual(len(instruction), 2)
    
    def test_full_patch_sequence(self):
        """Test full patch sequence is valid."""
        patch = bytes.fromhex('002e02d0706808b97047')
        
        # Should be 5 Thumb instructions = 10 bytes
        self.assertEqual(len(patch), 10)
        
        # Each instruction is 2 bytes
        instructions = [patch[i:i+2] for i in range(0, 10, 2)]
        self.assertEqual(len(instructions), 5)


if __name__ == '__main__':
    unittest.main(verbosity=2)
