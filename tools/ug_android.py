"""Build the Android APK of Project Underground on this PC, from the player's own installed game (what the launcher's
"Android" button and setup.sh --android run). Nothing from the disc leaves this PC except into the APK written here.

usage: python -I tools/ug_android.py --root <install dir> [--ndk <dir>] [--accept-ndk-license] [--jobs N]
  needs a finished PC install: <root>/build/lifted (the translated game code) and <root>/game (the disc's files)
  1. the Android NDK (clang for arm64): --ndk, ANDROID_NDK_HOME, or <root>/android/ndk (downloaded from Google
     with --accept-ndk-license: only the compiler and the arm64 libraries are unpacked, about 1 GB)
  2. compile the game code for arm64 and link libmain.so with the kit's android/libugrt.a (our runtime)
  3. the APK: the kit's android/template.apk (SDL3 and our Java code; no game code)
     + lib/arm64-v8a/libmain.so + the disc's files as assets (the app copies them out on its first start)
  4. sign it (APK Signature Scheme v2) with this PC's own key, <root>/android/signing-key.json (made on first use;
     keep it: Android only installs a new build over the old one when the key is the same)
  -> <root>/ProjectUnderground-<version>-Android.apk: copy it to the phone and open it to install.
Progress lines start with "## ", a failure with "ERROR: " (the launcher shows both).
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import secrets
import shutil
import stat
import struct
import subprocess
import sys
import time
import urllib.request
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)
NDK_VER = "r27c"
API = 33


def step(msg):
    print("## " + msg, flush=True)


def fail(msg):
    print("ERROR: " + msg, flush=True)
    sys.exit(1)


# ---------------------------------------------------------------- NDK

def ndk_host():
    return "windows-x86_64" if os.name == "nt" else "linux-x86_64"


def ndk_ok(ndk):
    exe = ".exe" if os.name == "nt" else ""
    return ndk and os.path.exists(os.path.join(ndk, "toolchains", "llvm", "prebuilt", ndk_host(), "bin", "clang" + exe))


def ndk_get(root, accept):
    """The compiler part of the NDK in <root>/android/ndk (downloaded once)."""
    dst = os.path.join(root, "android", "ndk")
    if ndk_ok(dst):
        return dst
    if not accept:
        fail("The Android NDK is needed (about 700 MB download from Google, Android SDK license: "
             "https://developer.android.com/studio/terms). Run again with --accept-ndk-license, or give --ndk <dir>.")
    plat = "windows" if os.name == "nt" else "linux"
    url = f"https://dl.google.com/android/repository/android-ndk-{NDK_VER}-{plat}.zip"
    zpath = os.path.join(root, "android", f"android-ndk-{NDK_VER}-{plat}.zip")
    os.makedirs(os.path.dirname(zpath), exist_ok=True)
    if not os.path.exists(zpath):
        step("Downloading the Android NDK (about 700 MB)")
        tmp = zpath + ".part"
        with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            got = last = 0
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
                got += len(b)
                if total and got * 20 // total != last:
                    last = got * 20 // total
                    print(f"downloaded {got >> 20} of {total >> 20} MB", flush=True)
        os.replace(tmp, zpath)
    step("Unpacking the Android compiler")
    pre = f"android-ndk-{NDK_VER}/toolchains/llvm/prebuilt/{ndk_host()}/"
    keep_bin = ("clang", "clang.exe", "ld.lld", "ld.lld.exe", "lld", "lld.exe", "ld", "ld.exe")
    tmpdir = dst + ".new"
    shutil.rmtree(tmpdir, ignore_errors=True)
    with zipfile.ZipFile(zpath) as z:
        names = []
        for zi in z.infolist():
            n = zi.filename
            if not n.startswith(pre) or n.endswith("/"):
                continue
            rel = n[len(pre):]
            parts = rel.split("/")
            if parts[0] == "bin":
                base = parts[-1]
                if not (len(parts) == 2 and (base in keep_bin or base.startswith("clang-") and base[6:].isdigit()
                                             or base.lower().endswith(".dll"))):
                    continue
            elif parts[0] == "lib":
                if not (len(parts) > 2 and parts[1] == "clang"):
                    continue
            elif parts[0] == "sysroot":
                if not (rel.startswith("sysroot/usr/include/") or rel.startswith("sysroot/usr/lib/aarch64-linux-android/")):
                    continue
            else:
                continue
            names.append(zi)
        for i, zi in enumerate(names):
            out = os.path.join(tmpdir, "toolchains", "llvm", "prebuilt", ndk_host(), *zi.filename[len(pre):].split("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            mode = zi.external_attr >> 16
            if stat.S_ISLNK(mode) and os.name != "nt":
                os.symlink(z.read(zi).decode(), out)
                continue
            with z.open(zi) as s, open(out, "wb") as f:
                shutil.copyfileobj(s, f, 1 << 20)
            if mode & 0o111:
                os.chmod(out, 0o755)
            if i % 500 == 0:
                print(f"unpacked {i} of {len(names)} files", flush=True)
    shutil.rmtree(dst, ignore_errors=True)
    os.replace(tmpdir, dst)
    os.remove(zpath)
    if not ndk_ok(dst):
        fail("the Android NDK download did not contain the compiler")
    return dst


# ---------------------------------------------------------------- compile + link

def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, errors="replace")
    return r.returncode, r.stdout


def build_lib(root, ndk, kit, work, jobs):
    exe = ".exe" if os.name == "nt" else ""
    clang = os.path.join(ndk, "toolchains", "llvm", "prebuilt", ndk_host(), "bin", "clang" + exe)
    target = f"--target=aarch64-linux-android{API}"
    lifted = os.path.join(root, "build", "lifted")
    srcs = sorted(f for f in os.listdir(lifted) if f.endswith(".c"))
    if not srcs:
        fail("No translated game code in " + lifted + ": finish the PC install first.")
    objdir = os.path.join(work, "obj")
    os.makedirs(objdir, exist_ok=True)
    # tools/build.py LIFT_FLAGS (one copy of the lifted-code flags; -ffp-contract=off matters on arm64, whose clang
    # fuses multiply-adds by default) + plain char signed as on x86 (arm64 makes it unsigned)
    sys.path.insert(0, HERE)
    import build  # noqa: E402
    flags = [target] + [f for f in build.LIFT_FLAGS if f != "-c" and not f.startswith("-I")] +         ["-fsigned-char", "-fPIC", "-I" + os.path.join(kit, "include")]

    def one(f):
        return f, run([clang] + flags + ["-c", os.path.join(lifted, f), "-o", os.path.join(objdir, f[:-2] + ".o")])
    step(f"Compiling {len(srcs)} files for Android with {jobs} jobs (several minutes)")
    done = 0
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for f, (code, out) in ex.map(one, srcs):
            done += 1
            if code:
                fail(f"compile failed: {f}\n{out[-3000:]}")
            print(f"compiled {f} ({done}/{len(srcs)})", flush=True)
    step("Linking libmain.so")
    libdir = os.path.join(work, "lib")
    os.makedirs(libdir, exist_ok=True)
    with zipfile.ZipFile(os.path.join(kit, "template.apk")) as z:
        for n in z.namelist():
            if n.startswith("lib/arm64-v8a/") and n.endswith(".so"):
                with open(os.path.join(libdir, n.split("/")[-1]), "wb") as f:
                    f.write(z.read(n))
    so = os.path.join(work, "libmain.so")
    code, out = run([clang, target, "-shared", "-o", so] + [os.path.join(objdir, f[:-2] + ".o") for f in srcs] +
                    [os.path.join(kit, "libugrt.a"), "-L" + libdir, "-lSDL3", "-landroid", "-llog", "-ldl", "-lm", "-Wl,-z,max-page-size=16384",
                     "-Wl,--no-undefined", "-Wl,-soname,libmain.so", "-Wl,--build-id=none"])
    if code:
        fail("link failed:\n" + out[-3000:])
    return so


# ---------------------------------------------------------------- APK (zip with aligned stored entries)

class ApkWriter:
    def __init__(self, path):
        self.f = open(path, "w+b")
        self.cd = []

    def add(self, name, data=None, src=None, compress=True, align=4):
        """data (bytes) or src (file path, streamed and stored)"""
        nb = name.encode()
        if src is not None:
            size = os.path.getsize(src)
            crc = 0
            with open(src, "rb") as f:
                for b in iter(lambda: f.read(1 << 20), b""):
                    crc = zlib.crc32(b, crc)
            method, csize, payload = 0, size, None
        else:
            size, crc = len(data), zlib.crc32(data)
            if compress:
                c = zlib.compressobj(9, zlib.DEFLATED, -15)
                payload = c.compress(data) + c.flush()
                method = 8
                if len(payload) >= size:
                    method, payload = 0, data
            else:
                method, payload = 0, data
            csize = len(payload)
        off = self.f.tell()
        extra = b""
        if method == 0:
            pad = (-(off + 30 + len(nb))) % align
            extra = b"\0" * pad
        dos_time, dos_date = 0, (2026 - 1980) << 9 | 1 << 5 | 1
        self.f.write(struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0x800, method, dos_time, dos_date, crc, csize, size,
                                 len(nb), len(extra)) + nb + extra)
        if payload is not None:
            self.f.write(payload)
        else:
            with open(src, "rb") as f:
                shutil.copyfileobj(f, self.f, 1 << 20)
        self.cd.append(struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0x800, method, dos_time, dos_date, crc,
                                   csize, size, len(nb), 0, 0, 0, 0, 0, off) + nb)

    def finish(self):
        """returns (entries_end, cd bytes, eocd bytes); the file holds the entries only"""
        end = self.f.tell()
        cd = b"".join(self.cd)
        eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, len(self.cd), len(self.cd), len(cd), end, 0)
        return end, cd, eocd


# ---------------------------------------------------------------- RSA key, certificate, APK Signature Scheme v2

def is_prime(n):
    if n < 4:
        return n in (2, 3)
    for p in (3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0:
            return False
    d, s = n - 1, 0
    while d % 2 == 0:
        d //= 2
        s += 1
    for _ in range(40):
        a = secrets.randbelow(n - 3) + 2
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(s - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def prime(bits):
    while True:
        p = secrets.randbits(bits) | (3 << (bits - 2)) | 1
        if is_prime(p):
            return p


def der(tag, body):
    n = len(body)
    if n < 0x80:
        ln = bytes([n])
    else:
        b = n.to_bytes((n.bit_length() + 7) // 8, "big")
        ln = bytes([0x80 | len(b)]) + b
    return bytes([tag]) + ln + body


def der_int(v):
    b = v.to_bytes(max(1, (v.bit_length() + 8) // 8), "big")
    return der(0x02, b)


def der_seq(*items):
    return der(0x30, b"".join(items))


OID_RSA = bytes.fromhex("06092a864886f70d010101")
OID_SHA256_RSA = bytes.fromhex("06092a864886f70d01010b")
DIGESTINFO_SHA256 = bytes.fromhex("3031300d060960864801650304020105000420")


def rsa_sign(key, data):
    n, d = key["n"], key["d"]
    k = (n.bit_length() + 7) // 8
    t = DIGESTINFO_SHA256 + hashlib.sha256(data).digest()
    em = b"\x00\x01" + b"\xff" * (k - len(t) - 3) + b"\x00" + t
    return pow(int.from_bytes(em, "big"), d, n).to_bytes(k, "big")


def spki(key):
    rsapub = der_seq(der_int(key["n"]), der_int(key["e"]))
    return der_seq(der_seq(OID_RSA, b"\x05\x00"), der(0x03, b"\x00" + rsapub))


def make_key(path):
    if os.path.exists(path):
        with open(path) as f:
            k = json.load(f)
        return {"n": int(k["n"], 16), "e": int(k["e"], 16), "d": int(k["d"], 16), "cert": bytes.fromhex(k["cert"])}
    step("Making this PC's signing key (once)")
    e = 65537
    while True:
        p, q = prime(1024), prime(1024)
        phi = (p - 1) * (q - 1)
        if p != q and phi % e and (p * q).bit_length() == 2048:
            break
    n = p * q
    key = {"n": n, "e": e, "d": pow(e, -1, phi)}
    name = der_seq(der(0x31, der_seq(bytes.fromhex("0603550403"), der(0x0C, b"Project Underground (built on this PC)"))))
    validity = der_seq(der(0x17, time.strftime("%y%m%d%H%M%SZ", time.gmtime()).encode()),
                       der(0x18, b"20991231235959Z"))
    alg = der_seq(OID_SHA256_RSA, b"\x05\x00")
    tbs = der_seq(der(0xA0, der_int(2)), der_int(secrets.randbits(63) | 1), alg, name, validity, name, spki(key))
    key["cert"] = der_seq(tbs, alg, der(0x03, b"\x00" + rsa_sign(key, tbs)))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump({"n": f"{n:x}", "e": f"{e:x}", "d": f"{key['d']:x}", "cert": key["cert"].hex()}, f)
    return key


def lp(b):   # length-prefixed (uint32)
    return struct.pack("<I", len(b)) + b


def chunk_digest(fobj, start, end, extra_parts):
    """v2 content digest over [start, end) of the file plus the in-memory sections"""
    digests = []
    fobj.seek(start)
    left = end - start
    while left > 0:
        b = fobj.read(min(1 << 20, left))
        left -= len(b)
        digests.append(hashlib.sha256(b"\xa5" + struct.pack("<I", len(b)) + b).digest())
    for part in extra_parts:
        for i in range(0, len(part), 1 << 20):
            b = part[i:i + (1 << 20)]
            digests.append(hashlib.sha256(b"\xa5" + struct.pack("<I", len(b)) + b).digest())
    return hashlib.sha256(b"\x5a" + struct.pack("<I", len(digests)) + b"".join(digests)).digest()


def sign_v2(w, key, out_path):
    end, cd, eocd = w.finish()
    digest = chunk_digest(w.f, 0, end, [cd, eocd])   # EOCD's CD offset = the signing block's offset (= end)
    alg = 0x0103   # RSASSA-PKCS1-v1_5 with SHA2-256
    signed = lp(lp(struct.pack("<I", alg) + lp(digest))) + lp(lp(key["cert"])) + lp(b"")
    signer = lp(signed) + lp(lp(struct.pack("<I", alg) + lp(rsa_sign(key, signed)))) + lp(spki(key))
    value = lp(lp(signer))
    pair = struct.pack("<QI", 4 + len(value), 0x7109871A) + value
    size = len(pair) + 8 + 16
    block = struct.pack("<Q", size) + pair + struct.pack("<Q", size) + b"APK Sig Block 42"
    eocd = eocd[:16] + struct.pack("<I", end + len(block)) + eocd[20:]
    w.f.seek(end)
    w.f.write(block + cd + eocd)
    w.f.close()


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", required=True)
    ap.add_argument("--ndk")
    ap.add_argument("--accept-ndk-license", action="store_true")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--out")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    kit = os.path.join(KIT, "android")
    if not os.path.exists(os.path.join(kit, "template.apk")):
        fail("android/template.apk is missing from the build kit.")
    game = os.path.join(root, "game")
    if not os.path.exists(os.path.join(game, "default.xbe")):
        fail("No installed game in " + root + ": install Project Underground on this PC first.")
    jobs = a.jobs or max(1, min(os.cpu_count() or 2, 8))
    with open(os.path.join(KIT, "VERSION")) as f:
        version = f.read().strip()

    step("Finding the Android compiler")
    ndk = a.ndk or os.environ.get("ANDROID_NDK_HOME")
    if not ndk_ok(ndk):
        ndk = ndk_get(root, a.accept_ndk_license)
    print("NDK: " + ndk, flush=True)
    work = os.path.join(root, "build", "android")
    so = build_lib(root, ndk, kit, work, jobs)

    step("Packing the APK with your game files")
    files = []
    for d, _, fs in os.walk(game):
        for fn in sorted(fs):
            p = os.path.join(d, fn)
            rel = os.path.relpath(p, game).replace(os.sep, "/")
            if not rel.startswith(".") and "\n" not in rel and not rel.lower().endswith(".pdb"):   # symbols: PC only
                files.append((rel, p, os.path.getsize(p)))
    files.sort()
    build_id = hashlib.sha256("\n".join(f"{s} {r}" for r, _, s in files).encode()).hexdigest()[:24]
    out = a.out or os.path.join(root, f"ProjectUnderground-{version}-Android.apk")
    tmp = out + ".new"
    w = ApkWriter(tmp)
    with zipfile.ZipFile(os.path.join(kit, "template.apk")) as z:
        for zi in z.infolist():
            n = zi.filename
            if n.startswith("META-INF/") and n.rsplit(".", 1)[-1] in ("SF", "RSA", "EC", "DSA", "MF"):
                continue   # the template's own signature
            data = z.read(zi)
            if n.endswith(".so"):
                w.add(n, data, compress=False, align=16384)   # loaded straight from the APK: stored, page-aligned
            else:
                w.add(n, data, compress=zi.compress_type != zipfile.ZIP_STORED)
    with open(so, "rb") as f:
        w.add("lib/arm64-v8a/libmain.so", f.read(), compress=False, align=16384)
    lst = f"# {build_id}\n" + "".join(f"{s} {r}\n" for r, _, s in files)
    w.add("assets/game.lst", lst.encode(), compress=False)
    for i, (rel, p, s) in enumerate(files):
        w.add("assets/game/" + rel, src=p, compress=False)
        if i % 200 == 0:
            print(f"packed {i} of {len(files)} game files", flush=True)
    step("Signing the APK")
    key = make_key(os.path.join(root, "android", "signing-key.json"))
    sign_v2(w, key, tmp)
    os.replace(tmp, out)
    print("APK: " + out, flush=True)
    step("Done")


if __name__ == "__main__":
    main()
