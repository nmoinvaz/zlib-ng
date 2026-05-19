#!/usr/bin/env python3
"""Confirm the static archive contains exactly the optimized variant
implementations the configured feature flags imply -- no more, no less.

Deterministic and self-deriving: it reads the build's active compile-definition
set (zlib-ng prints it as "CURRENT_SOURCE_DIR: ...") and a declarative
feature -> symbol map below. For every mapped feature:

  * feature enabled  -> its symbol(s) MUST be defined in the archive
  * feature disabled -> its symbol(s) MUST NOT be defined (dead/wrong variant)

It also asserts capability liveness: each capability has >=1 implementation
symbol (the library can never be left with no crc32/adler32/... at all).

Generic-fallback presence (crc32_braid, adler32_c, ...) is config-and-arch
dependent (e.g. LoongArch CRC is base-ISA so braid must be absent); that is
NOT guessed here. Pass verified per-config expectations via --expect/--forbid.

Usage: check_arch_symbols.py <build_dir> [nm] [--expect a,b] [--forbid x,y]
"""
import glob, re, subprocess, sys

# feature compile-definition -> optimized variant symbols it must produce.
# Keys are the macros zlib-ng emits in its "CURRENT_SOURCE_DIR:" line; values
# taken from the prototype guards in arch/*/*_functions.h.
FEATURE_SYMS = {
    # x86
    "X86_SSE2":            ["compare256_sse2", "slide_hash_sse2", "chunkmemset_safe_sse2"],
    "X86_SSSE3":           ["adler32_ssse3", "chunkmemset_safe_ssse3"],
    "X86_SSE42":           ["adler32_copy_sse42"],
    "X86_PCLMULQDQ_CRC":   ["crc32_pclmulqdq", "crc32_copy_pclmulqdq"],
    "X86_AVX2":            ["adler32_avx2", "compare256_avx2", "slide_hash_avx2", "chunkmemset_safe_avx2"],
    "X86_AVX512":          ["adler32_avx512", "compare256_avx512", "chunkmemset_safe_avx512"],
    "X86_AVX512VNNI":      ["adler32_avx512_vnni"],
    "X86_VPCLMULQDQ_AVX2": ["crc32_vpclmulqdq_avx2"],
    "X86_VPCLMULQDQ_AVX512": ["crc32_vpclmulqdq_avx512"],
    # arm (macro names confirmed from CI "CURRENT_SOURCE_DIR: ... ARM_*")
    "ARM_NEON":            ["adler32_neon", "compare256_neon", "slide_hash_neon"],
    "ARM_CRC32":           ["crc32_armv8"],
    "ARM_PMULL_EOR3":      ["crc32_armv8_pmull_eor3"],
    # power / s390 / riscv / loongarch: the emitted compile-def names are not
    # yet confirmed from a real run, so they are intentionally omitted to avoid
    # false assertions. Capability-liveness still applies on those arches; add
    # verified entries here once captured from a green run's cfg artifact.
}
# any zlib-ng arch/fallback implementation symbol (for the "found" report)
ARCH_SYM = re.compile(
    r"^(crc32_(braid|chorba\w*|pclmulqdq|vpclmulqdq\w*|armv8\w*|power8|"
    r"s390_vx|riscv64_zbc|loongarch64)|adler32_(c|ssse3|sse42|avx2|avx512\w*|"
    r"neon|vmx|power8|rvv|lsx)|compare256_\w+|longest_match\w*|"
    r"slide_hash_\w+|chunkmemset_safe_\w+|inflate_fast_\w+)$"
)

# every build must define at least one implementation per capability
CAPABILITIES = {
    "crc32":      re.compile(r"^crc32_(braid|chorba|pclmulqdq|vpclmulqdq|armv8|power8|s390_vx|riscv64_zbc|loongarch64)"),
    "adler32":    re.compile(r"^adler32_(c|ssse3|avx2|avx512|neon|vmx|power8|rvv|lsx)"),
    "compare256": re.compile(r"^compare256_"),
    "slide_hash": re.compile(r"^slide_hash_"),
}

def sh(a):
    try: return subprocess.run(a, capture_output=True, text=True).stdout
    except FileNotFoundError: return ""

def lst(flag):
    return ({s for s in re.split(r"[,\s]+", sys.argv[sys.argv.index(flag)+1]) if s}
            if flag in sys.argv else set())

def main():
    build = sys.argv[1]
    nmbin = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else "nm"
    if subprocess.run(["which", nmbin], capture_output=True).returncode: nmbin = "nm"
    expect, forbid = lst("--expect"), lst("--forbid")

    cfg = glob.glob(f"{build}/**/.cfg.log", recursive=True) + [f"{build}/.cfg.log"]
    log = ""
    for c in cfg:
        try: log = open(c).read(); break
        except OSError: continue
    m = re.search(r"CURRENT_SOURCE_DIR:\s*(.+)", log)
    feats = set(m.group(1).split()) if m else set()
    if not feats:
        print("::warning::could not read active feature set; skipping derived checks")

    ars = (glob.glob(f"{build}/**/libz-ng.a", recursive=True)
           or glob.glob(f"{build}/**/libz.a", recursive=True))
    if not ars:
        print(f"::error::no static archive under {build}"); return 1
    archive = ars[0]
    defined = {ln.split()[-1].lstrip("_")
               for ln in sh([nmbin, "--defined-only", archive]).splitlines()
               if len(ln.split()) >= 2 and ln.split()[-2] in ("T", "t")}
    # *_stub are functable dispatch shims, not implementations; functable.c
    # guards them with #ifndef DISABLE_RUNTIME_CPU_DETECTION, so they exist
    # iff runtime CPU detection is on. Keep them out of impl/liveness checks.
    stubs = sorted(s for s in defined if s.endswith("_stub"))
    impl = {s for s in defined if not s.endswith("_stub")}
    found = sorted(s for s in impl if ARCH_SYM.match(s))
    mapped = sorted(f for f in feats if f in FEATURE_SYMS)
    print(f"archive {archive}: {len(defined)} defined symbols")
    print("enabled features:")
    for f in mapped or ["(none mapped)"]:
        print(f"  {f}")
    print(f"arch implementation symbols found ({len(found)}):")
    for s in found:
        print(f"  {s}")

    rc = 0
    def fail(msg):
        nonlocal rc
        print(f"::error::{msg}")
        rc = 1

    # derived: enabled feature => present; mapped-but-disabled => absent
    for feat, syms in FEATURE_SYMS.items():
        on = feat in feats
        for s in syms:
            if on and s not in defined:
                fail(f"{feat} enabled but '{s}' not in archive")
            if not on and s in defined:
                fail(f"{feat} disabled but '{s}' present (wrong/dead variant)")

    # capability liveness (a stub is a shim, not an implementation)
    for cap, rx in CAPABILITIES.items():
        if not any(rx.match(s) for s in impl):
            fail(f"no '{cap}' implementation compiled into the archive")

    # functable stubs exist iff runtime CPU detection is on
    runtime_on = "DISABLE_RUNTIME_CPU_DETECTION" not in feats
    print(f"runtime CPU detection: {'on' if runtime_on else 'off'}; "
          f"{len(stubs)} *_stub symbols")
    if not runtime_on and stubs:
        fail("DISABLE_RUNTIME_CPU_DETECTION set but *_stub symbols present: "
             + " ".join(stubs))
    elif runtime_on and not stubs:
        # static stubs may be absent if the archive carries no local symbols
        # (stripped / nm without locals), so warn rather than hard-fail.
        print("::warning::runtime CPU detection on but no *_stub symbols found "
              "(stripped archive, or functable not built?)")

    # explicit verified per-config overrides
    for s in sorted(expect - defined): fail(f"expected '{s}' absent")
    for s in sorted(forbid & defined): fail(f"forbidden '{s}' present")

    if rc == 0:
        print("OK")
    return rc

if __name__ == "__main__":
    sys.exit(main())
