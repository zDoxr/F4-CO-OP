"""Decrypt CoSync .cslog files (format: DoxCoSync/GUI/CoSyncLogCrypt.h).

Usage:
  python cslog_decrypt.py [files or folders ...] [--key PATH] [--out FOLDER]

With no files, decrypts every *.cslog in the game's Data\\F4SE\\Plugins.
Writes <name>.log next to each file (or into --out).
The private key: --key, else %COSYNC_LOG_KEY%, else
  %USERPROFILE%\\Documents\\CoSyncLogKey\\cosync_log_private.pem
Needs: pip install cryptography
"""
import argparse, glob, os, struct, sys

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

MAGIC = b"CSLOG\x00\x01\x00"
DEFAULT_DIR = r"C:\Program Files (x86)\Steam\steamapps\common\Fallout 4\Data\F4SE\Plugins"


def load_key(path):
    with open(path, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def decrypt(path, key):
    data = open(path, "rb").read()
    if not data.startswith(MAGIC) or len(data) < 10:
        raise ValueError("not a CoSync encrypted log")
    (n,) = struct.unpack_from("<H", data, 8)
    wrapped = data[10:10 + n]
    secret = key.decrypt(wrapped, padding.OAEP(mgf=padding.MGF1(hashes.SHA256()),
                                               algorithm=hashes.SHA256(), label=None))
    aes_key, iv = secret[:32], secret[32:48]
    dec = Cipher(algorithms.AES(aes_key), modes.CTR(iv)).decryptor()
    return dec.update(data[10 + n:]) + dec.finalize()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--key")
    ap.add_argument("--out")
    a = ap.parse_args()
    key_path = a.key or os.environ.get("COSYNC_LOG_KEY") or os.path.join(
        os.path.expanduser("~"), "Documents", "CoSyncLogKey", "cosync_log_private.pem")
    key = load_key(key_path)
    files = []
    for p in (a.paths or [DEFAULT_DIR]):
        files += glob.glob(os.path.join(p, "*.cslog")) if os.path.isdir(p) else [p]
    if not files:
        print("no .cslog files found")
        return 1
    for f in files:
        try:
            text = decrypt(f, key)
        except Exception as e:
            print(f"FAILED {f}: {e}")
            continue
        out_dir = a.out or os.path.dirname(f)
        os.makedirs(out_dir, exist_ok=True)
        dst = os.path.join(out_dir, os.path.basename(f)[:-len(".cslog")] + ".log")
        with open(dst, "wb") as o:
            o.write(text)
        print(f"{f} -> {dst} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
