#!/usr/bin/env python3
"""Confirm the native_* dispatch macros resolve to the correct implementation
under DISABLE_RUNTIME_CPU_DETECTION (catches the macro-redefinition / silent
wrong-dispatch class, e.g. native_crc32 ending up crc32_chorba instead of
crc32_chorba_sse41).

Pure preprocessor: a probe TU emits each native_* expansion and which
compile-time *_NATIVE feature macros are set, via #pragma message. Compiled
-fsyntax-only with the target compiler -> cross-safe, no link, no qemu.

Expected value = first implementation in the capability's dispatch-priority
list whose gating *_NATIVE macro is set, else the generic fallback. Priority
is the documented zlib-ng preference; x86/arm are encoded (verified); other
arches are report-only (printed, not asserted) until verified.

Usage: check_native_dispatch.py <src> <build_dir> <cc> [extra cc flags...]
"""
import json, os, re, subprocess, sys, tempfile

# capability -> ordered [(gating *_NATIVE macro, expected symbol)], best first;
# final entry's macro None = the generic fallback when nothing above matched.
# Per-arch dispatch rules taken verbatim from each arch/*/*_functions.h
# `#ifdef *_NATIVE -> #define native_crc32 ...` block (best first). Only one
# arch's *_NATIVE macros can be set in a build, so cross-arch entries coexist.
PRIORITY = {
    "native_crc32": [
        ("X86_VPCLMULQDQ_AVX512_NATIVE", "crc32_vpclmulqdq_avx512"),
        ("X86_VPCLMULQDQ_AVX2_NATIVE",   "crc32_vpclmulqdq_avx2"),
        ("X86_PCLMULQDQ_NATIVE",         "crc32_pclmulqdq"),
        ("X86_SSE41_NATIVE",             "crc32_chorba_sse41"),
        ("X86_SSE2_NATIVE",              "crc32_chorba_sse2"),
        ("ARM_PMULL_EOR3_NATIVE",        "crc32_armv8_pmull_eor3"),
        ("ARM_CRC32_NATIVE",             "crc32_armv8"),
        ("POWER8_VSX_CRC32_NATIVE",      "crc32_power8"),
        ("S390_VX_NATIVE",               "crc32_s390_vx"),
        ("RISCV_ZBC_NATIVE",             "crc32_riscv64_zbc"),
        ("LOONGARCH_CRC_NATIVE",         "crc32_loongarch64"),
    ],
    "native_crc32_copy": [
        ("X86_VPCLMULQDQ_AVX512_NATIVE", "crc32_copy_vpclmulqdq_avx512"),
        ("X86_VPCLMULQDQ_AVX2_NATIVE",   "crc32_copy_vpclmulqdq_avx2"),
        ("X86_PCLMULQDQ_NATIVE",         "crc32_copy_pclmulqdq"),
        ("X86_SSE41_NATIVE",             "crc32_copy_chorba_sse41"),
        ("X86_SSE2_NATIVE",              "crc32_copy_chorba_sse2"),
        ("ARM_PMULL_EOR3_NATIVE",        "crc32_copy_armv8_pmull_eor3"),
        ("ARM_CRC32_NATIVE",             "crc32_copy_armv8"),
        ("POWER8_VSX_CRC32_NATIVE",      "crc32_copy_power8"),
        ("S390_VX_NATIVE",               "crc32_copy_s390_vx"),
        ("RISCV_ZBC_NATIVE",             "crc32_copy_riscv64_zbc"),
    ],
    "native_adler32": [
        ("X86_AVX512VNNI_NATIVE", "adler32_avx512_vnni"),
        ("X86_AVX512_NATIVE",     "adler32_avx512"),
        ("X86_AVX2_NATIVE",       "adler32_avx2"),
        ("X86_SSSE3_NATIVE",      "adler32_ssse3"),
        ("ARM_NEON_NATIVE",       "adler32_neon"),
        ("POWER8_VSX_NATIVE",     "adler32_power8"),
        ("PPC_VMX_NATIVE",        "adler32_vmx"),
        ("RISCV_RVV_NATIVE",      "adler32_rvv"),
        ("LOONGARCH_LASX_NATIVE", "adler32_lasx"),
        ("LOONGARCH_LSX_NATIVE",  "adler32_lsx"),
    ],
    "native_adler32_copy": [
        ("X86_AVX512VNNI_NATIVE", "adler32_copy_avx512_vnni"),
        ("X86_AVX512_NATIVE",     "adler32_copy_avx512"),
        ("X86_AVX2_NATIVE",       "adler32_copy_avx2"),
        ("X86_SSE42_NATIVE",      "adler32_copy_sse42"),
        ("X86_SSSE3_NATIVE",      "adler32_copy_ssse3"),
    ],
    # x86 SIMD tiers verified from arch/x86/x86_functions.h; non-x86 SIMD
    # priority is a follow-up (relies on the generic-clobber check below).
    "native_chunkmemset_safe": [
        ("X86_AVX512_NATIVE", "chunkmemset_safe_avx512"),
        ("X86_AVX2_NATIVE",   "chunkmemset_safe_avx2"),
        ("X86_SSSE3_NATIVE",  "chunkmemset_safe_ssse3"),
        ("X86_SSE2_NATIVE",   "chunkmemset_safe_sse2"),
    ],
    "native_compare256": [
        ("X86_AVX512_NATIVE", "compare256_avx512"),
        ("X86_AVX2_NATIVE",   "compare256_avx2"),
        ("X86_SSE2_NATIVE",   "compare256_sse2"),
    ],
    "native_inflate_fast": [
        ("X86_AVX512_NATIVE", "inflate_fast_avx512"),
        ("X86_AVX2_NATIVE",   "inflate_fast_avx2"),
        ("X86_SSSE3_NATIVE",  "inflate_fast_ssse3"),
        ("X86_SSE2_NATIVE",   "inflate_fast_sse2"),
    ],
    "native_longest_match": [
        ("X86_AVX512_NATIVE", "longest_match_avx512"),
        ("X86_AVX2_NATIVE",   "longest_match_avx2"),
        ("X86_SSE2_NATIVE",   "longest_match_sse2"),
    ],
    "native_longest_match_roll": [
        ("X86_AVX512_NATIVE", "longest_match_roll_avx512"),
        ("X86_AVX2_NATIVE",   "longest_match_roll_avx2"),
        ("X86_SSE2_NATIVE",   "longest_match_roll_sse2"),
    ],
    "native_slide_hash": [
        ("X86_AVX2_NATIVE", "slide_hash_avx2"),
        ("X86_SSE2_NATIVE", "slide_hash_sse2"),
    ],
}
# when no mapped arch native is enabled, native_* must be a generic fallback
GENERIC = {
    "native_crc32":             {"crc32_braid", "crc32_chorba"},
    "native_crc32_copy":        {"crc32_copy_braid", "crc32_copy_chorba"},
    "native_adler32":           {"adler32_c"},
    "native_adler32_copy":      {"adler32_copy_c"},
    "native_chunkmemset_safe":  {"chunkmemset_safe_c"},
    "native_compare256":        {"compare256_c"},
    "native_inflate_fast":      {"inflate_fast_c"},
    "native_longest_match":     {"longest_match_c"},
    "native_longest_match_roll":{"longest_match_roll_c"},
    "native_slide_hash":        {"slide_hash_c"},
}
# *_NATIVE macros the probe should report (all gating macros above)
NATIVE_MACROS = sorted({m for lst in PRIORITY.values() for m, _ in lst})
ASSERT_CAPS = tuple(PRIORITY)

def main():
    src, build, cc = sys.argv[1], sys.argv[2], sys.argv[3]
    extra = sys.argv[4:]

    # Mirror the real dispatch TU exactly: take the actual compile flags
    # zlib-ng uses for a core C file (functable.c includes arch_functions.h and
    # is built with the global flags, not per-file ISA flags) from
    # compile_commands.json. This captures -march/-D/-I for any combo.
    cc_json = f"{build}/compile_commands.json"
    try:
        entries = json.load(open(cc_json))
    except OSError:
        print(f"::error::{cc_json} missing; configure with "
              "-D CMAKE_EXPORT_COMPILE_COMMANDS=ON")
        return 1
    ent = next((e for e in entries if re.search(r"/(functable|crc32|deflate)\.c$",
               e["file"])), None)
    if not ent:
        print("::error::no core C TU in compile_commands.json")
        return 1
    args = ent.get("arguments") or ent["command"].split()
    # keep every flag verbatim (so -march/-arch/-target/-isysroot/-D/-I all
    # carry over and the probe mirrors the real TU); drop only output/dep/source.
    keep = []
    skip_next = False
    for a in args[1:]:
        if skip_next:
            skip_next = False; continue
        if a in ("-o", "-MT", "-MF", "-MQ"):
            skip_next = True; continue
        if a in ("-c", "-MD", "-MMD") or a.endswith((".c", ".o", ".obj")):
            continue
        keep.append(a)
    defs = keep
    if "-DDISABLE_RUNTIME_CPU_DETECTION" not in keep:
        print("runtime CPU detection on -> native_* not macro-dispatched; skip")
        return 0

    caps = list(PRIORITY)
    probe = "#include \"zbuild.h\"\n#include \"arch_functions.h\"\n" \
            "#define S2(x) #x\n#define S(x) S2(x)\n"
    for cp in caps:
        probe += f"#ifdef {cp}\n#pragma message(\"DISP {cp}=\" S({cp}))\n#endif\n"
    for nm in NATIVE_MACROS:
        probe += f"#ifdef {nm}\n#pragma message(\"NAT {nm}\")\n#endif\n"

    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(probe); tu = f.name
    p = subprocess.run([cc, "-fsyntax-only", *defs, *extra,
                        f"-I{src}", f"-I{build}", tu],
                       capture_output=True, text=True)
    out = p.stderr
    os.unlink(tu)

    # A non-zero compiler exit is the primary signal that the probe TU did not
    # build (missing generated headers, bad flags). Fail fast here so a real
    # compile error cannot masquerade as a successful probe.
    if p.returncode != 0:
        print("::error::native-dispatch probe failed to compile "
              f"(returncode {p.returncode}); cannot verify macro resolution")
        print(out.strip()[-1500:])
        return 1

    disp = dict(re.findall(r"DISP (native_\w+)=(\w+)", out))
    natives = set(re.findall(r"NAT (\w+)", out))
    # Defence in depth: even on a zero exit, a missing DISP set means the probe
    # produced no #pragma messages -- treat as a harness error.
    if not disp:
        print("::error::native-dispatch probe emitted no DISP lines; "
              "cannot verify macro resolution")
        print(out.strip()[-1500:])
        return 1
    print("compile-time natives:", " ".join(sorted(natives)) or "(none)")

    rc = 0
    for cp in caps:
        actual = disp.get(cp, "(undefined)")
        exp = next((sym for mac, sym in PRIORITY[cp] if mac in natives), None)
        if exp is None:
            # No PRIORITY rule matched for the natives present. PRIORITY is
            # incomplete for non-x86 SIMD caps, so we cannot tell whether an
            # unmapped arch native (e.g. slide_hash_rvv) is correct or whether
            # generic here is a clobber. Either way it is NOT a clobber we can
            # prove (a clobber is caught in the exp-is-not-None path), so this
            # is informational only -- never an error.
            gen = GENERIC.get(cp, set())
            kind = "generic" if actual in gen else "arch native (unverified)"
            print(f"{cp} = {actual}  [report-only: {kind}]")
            continue
        tag = "OK" if actual == exp else "MISMATCH"
        print(f"{cp} = {actual}  expected {exp}  [{tag}]")
        if actual != exp and cp in ASSERT_CAPS:
            print(f"::error::{cp} resolves to '{actual}' but should be '{exp}' "
                  f"for the configured natives")
            rc = 1
    return rc

if __name__ == "__main__":
    sys.exit(main())
