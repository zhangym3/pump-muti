"""Run production journal/crypto using the pinned SDK's real mbedTLS, no RF/IO."""
import argparse
from pathlib import Path
import subprocess
import sys

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--compiler',required=True)
ap.add_argument('--framework',type=Path,required=True)
args=ap.parse_args()
root=Path(__file__).resolve().parents[1]
peer=root.parent/'pump-muti-slave'
for relative in ('include/recovery_journal.h','include/recovery_nvs.h','include/recovery_diagnostics.h','include/recovery_crypto.h','src/recovery_crypto.cpp','include/recovery_credentials.h','include/recovery_channel.h','include/master_identity.h','src/master_identity.cpp','include/recovery_reset_intent.h','include/recovery_attempt_secret.h','src/recovery_attempt_secret.cpp'):
    if (root/relative).read_bytes()!=(peer/relative).read_bytes():
        raise RuntimeError('recovery contract differs: '+relative)
out=root/'.pio/recovery-tests';out.mkdir(parents=True,exist_ok=True)
library=args.framework/'components/mbedtls/mbedtls'
common=['-I',str(library/'include'),'-I',str(library/'library'),'-I',str(root/'test'),
        '-DMBEDTLS_CONFIG_FILE="recovery_crypto_config.h"']
zig=Path(args.compiler).stem.lower()=='zig'
objects=[]
for name in ('aes','cipher','cipher_wrap','gcm','md','sha256','hkdf','platform','platform_util','constant_time','block_cipher',
             'bignum','bignum_core','bignum_mod','bignum_mod_raw','ecp','ecp_curves','ecdsa','hmac_drbg','asn1parse','asn1write'):
    obj=out/(name+'.o');objects.append(str(obj))
    subprocess.run([args.compiler]+(['cc'] if zig else [])+common+['-O2','-c',str(library/'library'/(name+'.c')),'-o',str(obj)],check=True)
for test in ('recovery_tests','recovery_crypto_tests','gateway_session_tests','recovery_credentials_tests','recovery_channel_tests','master_identity_tests','recovery_reset_intent_tests','recovery_attempt_secret_tests'):
    exe=out/(test+('.exe' if sys.platform=='win32' else ''))
    sources=[str(root/'test'/(test+'.cpp'))]
    if test in ('recovery_crypto_tests','recovery_channel_tests'):sources+=[str(root/'src/recovery_crypto.cpp')]+objects
    if test in ('master_identity_tests','recovery_reset_intent_tests'):sources+=[str(root/'src/master_identity.cpp')]+objects
    if test=='recovery_attempt_secret_tests':sources+=[str(root/'src/master_identity.cpp'),str(root/'src/recovery_crypto.cpp'),str(root/'src/recovery_attempt_secret.cpp')]+objects
    subprocess.run([args.compiler]+(['c++'] if zig else [])+common+
                   ['-std=c++17','-Wall','-Wextra','-Werror','-O0','-g','-I',str(root/'include')]+sources+['-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
