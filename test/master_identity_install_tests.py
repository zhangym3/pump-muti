"""Synthetic cross-platform master identity fixtures; no hardware or real keys."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('master_installer', Path(__file__).resolve().parents[1] / 'tools/master_identity_install.py')
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)

PUBLIC = bytes.fromhex('046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296'
    '4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5')
IDENTITY = installer.Identity(bytes([9]) * 16, 1, bytes([3]) * 16, hashlib.sha256(PUBLIC).digest()[:16], PUBLIC)
NONCE = bytes([6]) * 32
SIGNATURE = bytes.fromhex('ad54eabaa39ee7456bf9a8add25ecb43cc596f8374ba7cc4228379c09413555a'
                         '304c6b1cbac2d3e300c545ff2a9b03bad627e67d57ed4133182611bef5c8e570')


class Console:
    def __init__(self, initialized=True):
        self.initialized = initialized
        self.initializations = 0
    def identity(self, initialize=False):
        if initialize:
            if self.initialized: raise AssertionError('Attempted to overwrite identity')
            self.initializations += 1
            self.initialized = True
        if not self.initialized: return ['MASTER_ID initialized=0 recovery_enabled=0']
        return ['MASTER_ID schema=1 initialized=1 epoch=1 master=' + IDENTITY.master.hex()
            + ' network=' + IDENTITY.network.hex() + ' uuid=' + IDENTITY.device.hex() + ' recovery_enabled=0',
            'MASTER_KEY master=' + IDENTITY.master.hex() + ' part=1 value=' + PUBLIC[:33].hex(),
            'MASTER_KEY master=' + IDENTITY.master.hex() + ' part=2 value=' + PUBLIC[33:].hex()]
    def prove(self, nonce):
        return SIGNATURE  # Valid only for NONCE, detects stale challenge handling.


class MasterInstallationTests(unittest.TestCase):
    def test_actual_sdk_vector_and_fixture(self):
        data = installer.encode(IDENTITY, NONCE, SIGNATURE)
        self.assertEqual(225, len(data))
        self.assertEqual(IDENTITY, installer.decode(data))
        fixture = Path(__file__).resolve().parents[2] / 'pump-muti-app/app/src/androidTest/assets/master-trust-mbedtls.pmmaster'
        self.assertEqual(data, fixture.read_bytes())

    def test_all_bytes_truncation_and_trailing_data_refused(self):
        data = installer.encode(IDENTITY, NONCE, SIGNATURE)
        for index in range(len(data)):
            bad = bytearray(data); bad[index] ^= 1
            with self.assertRaises(Exception): installer.decode(bytes(bad))
            with self.assertRaises(ValueError): installer.decode(data[:index])
        with self.assertRaises(ValueError): installer.decode(data + b'\0')

    def test_high_s_invalid_points_and_fingerprint(self):
        high = SIGNATURE[:32] + (installer.ORDER - int.from_bytes(SIGNATURE[32:], 'big')).to_bytes(32, 'big')
        with self.assertRaises(ValueError): installer.verify(IDENTITY, NONCE, high)
        with self.assertRaises(ValueError): installer.validate(IDENTITY._replace(public=bytes(65)))
        with self.assertRaises(ValueError): installer.validate(IDENTITY._replace(master=bytes([1]) * 16))

    def test_first_initialization_is_explicit_and_never_repeated(self):
        console = Console(False)
        with self.assertRaises(RuntimeError): installer.collect(console)
        self.assertEqual(0, console.initializations)
        with patch.object(installer.secrets, 'token_bytes', return_value=NONCE):
            self.assertEqual(IDENTITY, installer.decode(installer.collect(console, True)))
            self.assertEqual(IDENTITY, installer.decode(installer.collect(console, True)))
        self.assertEqual(1, console.initializations)

    def test_stale_proof_and_changed_usb_fields_refused_without_initializing(self):
        console = Console()
        with patch.object(installer.secrets, 'token_bytes', return_value=bytes([7]) * 32):
            with self.assertRaises(Exception): installer.collect(console, True)
        self.assertEqual(0, console.initializations)
        for lines in ([], console.identity()[:2], console.identity() + [console.identity()[1]],
                      [console.identity()[0], console.identity()[1], console.identity()[1]]):
            with self.assertRaises(ValueError): installer.parse_identity(lines)

    def test_no_file_overwrite_and_invalid_file_never_written(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'master.pmmaster'
            with self.assertRaises(ValueError): installer.write_public_file(path, b'bad')
            self.assertFalse(path.exists())
            data = installer.encode(IDENTITY, NONCE, SIGNATURE)
            installer.write_public_file(path, data)
            with self.assertRaises(FileExistsError): installer.write_public_file(path, data)
            self.assertEqual(data, path.read_bytes())


if __name__ == '__main__': unittest.main()
