"""Export a public master trust file through trusted USB, with a fresh P-256 proof.

The .pmmaster file contains no secret and grants no administrator permission.
Its provenance must be trusted when importing it on the phone. It is not a BLE
discovery certificate and never replaces an existing device identity.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import secrets
import struct
import time
from typing import NamedTuple

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec, utils

MAGIC = b'PMMSTR01'
SIZE = 225
ORDER = int('ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551', 16)


class Identity(NamedTuple):
    device: bytes
    epoch: int
    network: bytes
    master: bytes
    public: bytes


def validate(identity):
    for value in (identity.device, identity.network, identity.master):
        if len(value) != 16 or not any(value): raise ValueError('Invalid master identity')
    if not 1 <= identity.epoch <= 0xffffffff: raise ValueError('Invalid master epoch')
    if len(identity.public) != 65 or identity.public[0] != 4: raise ValueError('Invalid P-256 key')
    public = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), identity.public)
    if hashlib.sha256(identity.public).digest()[:16] != identity.master:
        raise ValueError('Master fingerprint mismatch')
    return public


def proof_message(identity, nonce):
    validate(identity)
    if len(nonce) != 32 or not any(nonce): raise ValueError('Invalid proof challenge')
    return (b'PUMP-MASTER-PROOF-V1' + identity.device + struct.pack('<I', identity.epoch)
            + identity.network + identity.master + identity.public + nonce)


def verify(identity, nonce, signature):
    message = proof_message(identity, nonce)
    if len(signature) != 64: raise ValueError('Invalid signature length')
    r, s = int.from_bytes(signature[:32], 'big'), int.from_bytes(signature[32:], 'big')
    if not (0 < r < ORDER and 0 < s <= ORDER // 2): raise ValueError('Noncanonical signature')
    validate(identity).verify(utils.encode_dss_signature(r, s), message, ec.ECDSA(hashes.SHA256()))


def encode(identity, nonce, signature):
    verify(identity, nonce, signature)
    return (MAGIC + struct.pack('<HH', 1, SIZE) + identity.device + struct.pack('<I', identity.epoch)
            + identity.network + identity.master + identity.public + nonce + signature)


def decode(data):
    if len(data) != SIZE or data[:12] != MAGIC + struct.pack('<HH', 1, SIZE):
        raise ValueError('Invalid master trust file')
    identity = Identity(data[12:28], struct.unpack_from('<I', data, 28)[0], data[32:48], data[48:64], data[64:129])
    verify(identity, data[129:161], data[161:225])
    return identity


def parse_identity(lines):
    if len(lines) != 3: raise ValueError('Unexpected USB identity response')
    match = re.fullmatch(r'MASTER_ID schema=1 initialized=1 epoch=([1-9][0-9]{0,9}) '
        r'master=([0-9a-f]{32}) network=([0-9a-f]{32}) uuid=([0-9a-f]{32}) recovery_enabled=0', lines[0])
    if not match: raise ValueError('Unexpected USB identity response')
    epoch, master, network, device = match.groups()
    parts = []
    for number, length in ((1, 66), (2, 64)):
        matches = [re.fullmatch(r'MASTER_KEY master=' + master + ' part=' + str(number)
                   + r' value=([0-9a-f]{' + str(length) + '})', line) for line in lines[1:]]
        values = [item.group(1) for item in matches if item]
        if len(values) != 1: raise ValueError('Missing or duplicate public key part')
        parts.append(values[0])
    identity = Identity(bytes.fromhex(device), int(epoch), bytes.fromhex(network), bytes.fromhex(master), bytes.fromhex(''.join(parts)))
    validate(identity)
    return identity


class Console:
    def __init__(self, name):
        import serial
        self.port = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=2)
        self.port.dtr = False
        self.port.rts = False
        self.port.port = name
        self.port.open()

    def close(self): self.port.close()

    def request(self, command, prefix, complete):
        self.port.reset_input_buffer()
        self.port.write(command.encode('ascii') + b'\n')
        deadline = time.monotonic() + 8
        lines, line = [], bytearray()
        while time.monotonic() < deadline:
            byte = self.port.read(1)
            if byte == b'\n':
                text = line.decode('ascii', errors='replace').strip()
                line.clear()
                if text.startswith(prefix):
                    lines.append(text)
                    if len(lines) > 4: raise ValueError('Too many USB response parts')
                    if complete(lines): return lines
                elif text.startswith(('BUSY ', 'INVALID ', 'STORAGE ', 'CONFLICT ', 'UNAUTHORIZED ')):
                    raise RuntimeError('USB identity operation refused; device state unchanged by exporter')
            elif byte:
                line.extend(byte)
                if len(line) > 512: raise ValueError('Oversize USB response')
        raise RuntimeError('USB reply unavailable; retry export, never erase identity storage')

    def identity(self, initialize=False):
        command = 'master-identity-init CONFIRM' if initialize else 'master-identity'
        return self.request(command, 'MASTER_', lambda lines: lines == ['MASTER_ID initialized=0 recovery_enabled=0'] or len(lines) == 3)

    def prove(self, nonce):
        lines = self.request('master-identity-prove ' + nonce.hex(), 'MASTER_PROOF ', lambda lines: len(lines) == 2)
        parts = {}
        for line in lines:
            match = re.fullmatch(r'MASTER_PROOF nonce=' + nonce.hex() + r' ([rs])=([0-9a-f]{64})', line)
            if not match or match[1] in parts: raise ValueError('Stale or duplicate USB proof')
            parts[match[1]] = bytes.fromhex(match[2])
        return parts['r'] + parts['s']


def collect(console, allow_initialize=False):
    lines = console.identity()
    if lines == ['MASTER_ID initialized=0 recovery_enabled=0']:
        if not allow_initialize: raise RuntimeError('Master identity not initialized; trusted USB initialization required')
        lines = console.identity(initialize=True)
    identity = parse_identity(lines)
    nonce = secrets.token_bytes(32)
    signature = console.prove(nonce)
    return encode(identity, nonce, signature)


def write_public_file(path, data):
    decode(data)
    # No credentials are present, but an existing installation file is never overwritten.
    with path.open('xb') as output:
        output.write(data)
        output.flush()
        os.fsync(output.fileno())
    if path.read_bytes() != data: raise RuntimeError('Saved master trust verification failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--initialize-if-empty', action='store_true', help='Explicit trusted USB initialization; never replaces an existing key')
    args = parser.parse_args()
    if args.out.exists(): raise FileExistsError('Choose a new output file; never overwrite installation material')
    if not args.out.parent.is_dir(): raise FileNotFoundError('Output directory must already exist')
    console = Console(args.port)
    try: data = collect(console, args.initialize_if_empty)
    finally: console.close()
    write_public_file(args.out, data)
    identity = decode(data)
    print('Public master trust file saved; fresh USB identity proof verified.')
    print('Fingerprint: ' + identity.master.hex())
    print('This file establishes public identity only. Recovery management remains disabled.')


if __name__ == '__main__': main()
