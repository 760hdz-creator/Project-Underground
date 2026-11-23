"""Fetch the portable build toolchain into <repo>/toolchain/ (no Visual Studio, no Windows SDK).

llvm-mingw (clang + lld + mingw-w64 headers/libs, UCRT) from github.com/mstorsjo/llvm-mingw,
pinned by version and SHA-256. Run: python -I tools/bootstrap.py
"""
import hashlib, os, sys, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TC = os.path.join(ROOT, "toolchain")

LLVM_MINGW_VER = "20261104"
LLVM_MINGW_ZIP = f"llvm-mingw-{LLVM_MINGW_VER}-ucrt-x86_64.zip"
LLVM_MINGW_URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW_VER}/{LLVM_MINGW_ZIP}"
LLVM_MINGW_SHA = "552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def fetch(url, dst, want):
    if os.path.exists(dst) and sha256(dst) == want:
        return
    print("## downloading " + url, flush=True)
    tmp = dst + ".part"
    urllib.request.urlretrieve(url, tmp)
    got = sha256(tmp)
    if got != want:
        os.remove(tmp)
        sys.exit(f"ERROR: checksum mismatch for {url}: {got}")
    os.replace(tmp, dst)


def llvm_mingw_dir():
    return os.path.join(TC, f"llvm-mingw-{LLVM_MINGW_VER}-ucrt-x86_64")


def main():
    os.makedirs(TC, exist_ok=True)
    d = llvm_mingw_dir()
    if not os.path.exists(os.path.join(d, "bin", "clang.exe")):
        z = os.path.join(TC, LLVM_MINGW_ZIP)
        fetch(LLVM_MINGW_URL, z, LLVM_MINGW_SHA)
        print("## extracting llvm-mingw", flush=True)
        with zipfile.ZipFile(z) as zf:
            zf.extractall(TC)
        os.remove(z)
    print("llvm-mingw: " + os.path.join(d, "bin"))


if __name__ == "__main__":
    main()
