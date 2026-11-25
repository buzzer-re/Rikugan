#!/usr/bin/env python3
"""Stand-ins for a host's libraries, so a plugin links without the host installed (RIKUGAN_SDK_ONLY, cmake/host_stubs.cmake).

A probe of the plugin is linked with the host's symbols left unresolved. Its link output (ELF, PE) or its own symbol
table (Mach-O) lists exactly what the host has to provide, and this script writes libraries that provide those names
under the host libraries' own names, which the plugin is then linked against for real.

  host_stubs.py capture <log> <link command...>
      CMake's RULE_LAUNCH_LINK for the probe: runs the link and keeps everything it prints in <log>, quietly (the
      linker reports every symbol the host provides, which is expected, not a build warning).

  host_stubs.py harvest --kind binja|ida --format elf|macho|pe --log <log> --probe <file> --out <dir>
                        [--nm <nm>] [--cxxfilt <c++filt>] [--qt Core=<lib> --qt Gui=<lib> --qt Widgets=<lib>]
      binja: every unresolved symbol is Binary Ninja's UI library's: <dir>/binaryninjaui.c, or on Windows
             <dir>/binaryninjaui.def for an import library of binaryninjaui.dll.
      ida:   every unresolved symbol is IDA's Qt (built with -qtnamespace QT). Each one is matched, without its QT::,
             against the exports of the official Qt build of the same version (--qt), which says which library it
             belongs to and whether it is code or data: <dir>/Qt6Core.c, Qt6Gui.c, Qt6Widgets.c. A symbol that is no
             Qt export stops the build: the plugin uses something the host does not have.
"""
import argparse
import os
import platform
import re
import subprocess
import sys


def capture(log, command):
    os.makedirs(os.path.dirname(os.path.abspath(log)), exist_ok=True)
    with open(log, "wb") as out:
        r = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT)
    # The probe exists to collect unresolved symbols: a link that reports them is the expected outcome. A probe that
    # was not written at all is caught by `harvest`, which shows this log.
    if r.returncode != 0:
        with open(log, "ab") as out:
            out.write(f"\nhost_stubs: the link exited with {r.returncode}\n".encode())
    return 0


def fail(msg, log=None):
    sys.stderr.write(f"host_stubs: {msg}\n")
    if log and os.path.exists(log):
        with open(log, encoding="utf-8", errors="replace") as f:
            tail = f.read()[-4000:]
        sys.stderr.write(f"--- end of {log} ---\n{tail}\n")
    sys.exit(1)


# ---- what the probe left unresolved ----

ELF_PATTERNS = [
    re.compile(r"undefined reference to [`'\u2018]([^'`\u2019]+)['\u2019]"),  # GNU ld
    re.compile(r"undefined symbol: (\S+)"),  # lld
]
PE_PATTERNS = [
    re.compile(r'unresolved external symbol "[^"]*" \(([^)\s]+)\)'),  # C++: demangled, then the decorated name
    re.compile(r"unresolved external symbol ([^\s\"]+)"),  # C: the name alone
]


def unresolved_from_log(log, patterns):
    names = set()
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            for p in patterns:
                m = p.search(line)
                if m:
                    names.add(m.group(1))
                    break
    return names


def unresolved_macho(nm, probe):
    # `nm -m` marks the symbols that -undefined dynamic_lookup left for the host: "(dynamically looked up)"
    out = run([nm, "-m", probe])
    names = set()
    for line in out.splitlines():
        if "(undefined)" in line and "dynamically looked up" in line:
            m = re.search(r"external (\S+)", line)
            if m:
                names.add(m.group(1))
    return names


def run(cmd, stdin=None):
    r = subprocess.run(cmd, input=stdin, capture_output=True, text=True)
    if r.returncode != 0:
        fail(f"{' '.join(cmd[:3])}... failed: {r.stderr.strip()[:500]}")
    return r.stdout


# ---- IDA's Qt: which library each symbol is in ----

def exports(nm, lib, fmt):
    """{symbol: is_data} for the official Qt library `lib` (names as the plugin references them on this platform)."""
    if fmt == "elf":
        out = run([nm, "-D", "--defined-only", lib])
    else:  # Qt's macOS frameworks are universal: this machine's architecture
        arch = "arm64" if platform.machine() in ("arm64", "aarch64") else "x86_64"
        out = run([nm, "-arch", arch, "-g", "-U", lib])
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        kind, name = parts[-2], parts[-1].split("@")[0]
        if kind in "TtWwiI":
            syms[name] = False
        elif kind in "DdBbRrVvSsGgu":
            syms[name] = True
    return syms


def demangle(cxxfilt, names, fmt):
    """Demangled names, in order. Mach-O symbols carry one more leading underscore than ELF's."""
    plain = [n[1:] if fmt == "macho" and n.startswith("_") else n for n in names]
    out = run([cxxfilt, "-n"], stdin="\n".join(plain) + "\n").splitlines()
    if len(out) != len(plain):
        fail(f"{cxxfilt} returned {len(out)} names for {len(plain)}")
    return out


def strip_qt_namespace(s):
    # C++ names carry the namespace (QT::); C names get it as a suffix (QT_MANGLE_NAMESPACE: qt_version_tag_QT)
    if "::" not in s and "(" not in s and s.endswith("_QT"):
        return s[: -len("_QT")]
    return s.replace("QT::", "")


def assign_qt(args, unresolved):
    libs = dict(q.split("=", 1) for q in args.qt)
    owner = {}  # demangled official name -> (library, is_data)
    for lib_name, path in libs.items():
        syms = exports(args.nm, path, args.format)
        names = sorted(syms)
        for n, d in zip(names, demangle(args.cxxfilt, names, args.format)):
            owner.setdefault(d, (lib_name, syms[n]))
    wanted = sorted(unresolved)
    by_lib = {lib_name: [] for lib_name in libs}
    unknown = []
    for n, d in zip(wanted, demangle(args.cxxfilt, wanted, args.format)):
        hit = owner.get(strip_qt_namespace(d))
        if hit:
            by_lib[hit[0]].append((n, hit[1]))
        else:
            unknown.append(d)
    if unknown:
        fail("the IDA plugin uses symbols that neither the IDA SDK nor the official Qt libraries given (--qt) "
             "export, so IDA would not have them either:\n  "
             + "\n  ".join(unknown[:40]) + ("\n  ..." if len(unknown) > 40 else ""))
    return by_lib


# ---- writing the stand-ins ----

def is_data_itanium(raw, fmt):
    n = raw[1:] if fmt == "macho" and raw.startswith("_") else raw
    if n.startswith(("_ZTV", "_ZTI", "_ZTS", "_ZTT")):  # vtable, typeinfo, typeinfo name, VTT
        return True
    # a nested name with nothing after its closing E is a variable (a function is followed by its parameter types)
    return n.startswith("_ZN") and n.endswith("E")


def write_c(path, lib, symbols):
    lines = [f"/* Generated by cmake/host_stubs.py: a stand-in for {lib}, linked instead of the host's own copy. */",
             "#define RK_EXPORT __attribute__((visibility(\"default\")))"]
    for i, (name, data) in enumerate(sorted(symbols)):
        label = name.replace("\\", "\\\\").replace('"', '\\"')
        if data:
            lines.append(f'RK_EXPORT char rk_d{i}[256] __asm__("{label}");')
        else:
            lines.append(f'RK_EXPORT void rk_f{i}(void) __asm__("{label}");')
            lines.append(f"void rk_f{i}(void) {{}}")
    if not symbols:
        lines.append("RK_EXPORT int rk_host_stub_empty;")
    write_if_changed(path, "\n".join(lines) + "\n")


def is_data_msvc(name):
    # after the scope's "@@" comes a digit for data (static members, globals, vftables, RTTI) and a letter for code
    if not name.startswith("?"):
        return False
    i = name.find("@@")
    return i >= 0 and i + 2 < len(name) and name[i + 2].isdigit()


def write_def(path, dll, names):
    lines = [f"LIBRARY {dll}", "EXPORTS"]
    for n in sorted(names):
        lines.append(f"    {n}{' DATA' if is_data_msvc(n) else ''}")
    write_if_changed(path, "\n".join(lines) + "\n")


def write_if_changed(path, text):
    # only a real change re-links the stand-ins and the plugin
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def harvest(args):
    if not os.path.exists(args.probe):
        fail(f"the probe {args.probe} was not linked", args.log)
    os.makedirs(args.out, exist_ok=True)
    if args.format == "elf":
        names = unresolved_from_log(args.log, ELF_PATTERNS)
    elif args.format == "pe":
        names = {n[len("__imp_"):] if n.startswith("__imp_") else n for n in unresolved_from_log(args.log, PE_PATTERNS)}
    else:
        names = unresolved_macho(args.nm, args.probe)

    if args.kind == "binja":
        if not names:
            fail("the probe left nothing unresolved: it should need Binary Ninja's UI library", args.log)
        if args.format == "pe":
            write_def(os.path.join(args.out, "binaryninjaui.def"), "binaryninjaui.dll", names)
        else:
            write_c(os.path.join(args.out, "binaryninjaui.c"), "Binary Ninja's UI library",
                    [(n, is_data_itanium(n, args.format)) for n in names])
        print(f"host_stubs: {len(names)} symbols of Binary Ninja's UI library")
        return
    if args.format == "pe":
        fail("IDA on Windows links the SDK's import libraries: no stand-ins to make")
    if not names:
        fail("the probe left nothing unresolved: it should need IDA's Qt", args.log)
    for lib_name, syms in assign_qt(args, names).items():
        write_c(os.path.join(args.out, f"Qt6{lib_name}.c"), f"IDA's Qt6{lib_name}", syms)
        print(f"host_stubs: {len(syms)} symbols of IDA's Qt6{lib_name}")


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "capture":
        sys.exit(capture(sys.argv[2], sys.argv[3:]))
    p = argparse.ArgumentParser()
    p.add_argument("command", choices=["harvest"])
    p.add_argument("--kind", required=True, choices=["binja", "ida"])
    p.add_argument("--format", required=True, choices=["elf", "macho", "pe"])
    p.add_argument("--log", required=True)
    p.add_argument("--probe", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--nm", default="nm")
    p.add_argument("--cxxfilt", default="c++filt")
    p.add_argument("--qt", action="append", default=[])
    harvest(p.parse_args())


if __name__ == "__main__":
    main()
