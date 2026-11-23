"""Fetch the portable build toolchain into <repo>/toolchain/ (no Visual Studio, no Windows SDK).

llvm-mingw (clang + lld + mingw-w64 headers/libs, UCRT) from github.com/mstorsjo/llvm-mingw,
pinned by version and SHA-256. Run: python -I tools/bootstrap.py
--dev also fetches glslang (Khronos, official release) for compiling the renderer's shaders to SPIR-V.
Players never need it: the SPIR-V is committed as C arrays (runtime/src/shaders/*.h).
"""
import hashlib, os, sys, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TC = os.environ.get("UG_TOOLCHAIN_DIR") or os.path.join(ROOT, "toolchain")   # ug_build.py: <install>/toolchain

LLVM_MINGW_VER = "20261104"
LLVM_MINGW_ZIP = f"llvm-mingw-{LLVM_MINGW_VER}-ucrt-x86_64.zip"
LLVM_MINGW_URL = f"https://github.com/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW_VER}/{LLVM_MINGW_ZIP}"
LLVM_MINGW_SHA = "552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f"


GLSLANG_VER = "16.6.0"
GLSLANG_ZIP = f"glslang-{GLSLANG_VER}-windows-x86_64-release.zip"
GLSLANG_URL = f"https://github.com/KhronosGroup/glslang/releases/download/{GLSLANG_VER}/{GLSLANG_ZIP}"
GLSLANG_SHA = "82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b"


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


def glslang_dir():
    return os.path.join(TC, f"glslang-{GLSLANG_VER}")


def fetch_glslang():
    d = glslang_dir()
    if os.path.exists(d):
        return
    z = os.path.join(TC, GLSLANG_ZIP)
    fetch(GLSLANG_URL, z, GLSLANG_SHA)
    print("## extracting glslang", flush=True)
    with zipfile.ZipFile(z) as zf:
        zf.extractall(d)
    os.remove(z)


def main():
    os.makedirs(TC, exist_ok=True)
    if "--dev" in sys.argv:
        fetch_glslang()
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
