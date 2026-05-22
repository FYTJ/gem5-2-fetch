#!/usr/bin/env python3
"""Build a unified RV64 riscv-tests manifest for lightweight validation gates."""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


STATUSES = {
    "pass",
    "fail",
    "timeout",
    "unsupported",
    "blocked_env",
    "blocked_missing_artifact",
}
DEFAULT_SUITES = ("rv64ui", "rv64um", "rv64mi")
DEFAULT_BACKENDS = ("xiangshan", "gem5", "wally")


def repo_paths() -> tuple[Path, Path, Path]:
    root = Path(__file__).resolve().parent
    isa = root / "riscv-tests" / "isa"
    return root, isa, isa / "build"


def find_tool(env_name: str, candidates: tuple[str, ...]) -> str | None:
    if os.environ.get(env_name):
        return os.environ[env_name]
    for candidate in candidates:
        found = shutil.which(candidate)
        if found:
            return found
    return None


def suite_arch(suite: str, default_arch: str) -> str:
    return os.environ.get(f"{suite.upper()}_ARCH", default_arch)


def classify_needs(suite: str, name: str) -> list[str]:
    needs: set[str] = set()
    if suite == "rv64ui":
        needs.add("rv64_user_integer")
        if name in {"beq", "bne", "bge", "bgeu", "blt", "bltu", "jal", "jalr"}:
            needs.add("branch_or_jump")
        if name == "fence_i":
            needs.add("zifencei")
        if name == "ma_data":
            needs.add("misaligned_data_behavior")
    elif suite == "rv64um":
        needs.update({"rv64_m_extension", "mul_div"})
    elif suite == "rv64mi":
        needs.update({"machine_mode", "zicsr", "trap_or_csr_sensitive"})
        if "misaligned" in name or name in {"ma_addr", "ma_fetch"}:
            needs.update({"exception", "misaligned"})
        if name in {"csr", "mcsr"}:
            needs.add("csr")
        if name in {"breakpoint", "sbreak"}:
            needs.update({"exception", "breakpoint"})
        if name == "scall":
            needs.update({"exception", "ecall"})
        if name in {"illegal", "ma_fetch"}:
            needs.add("illegal_or_fetch_exception")
        if name in {"zicntr", "instret_overflow"}:
            needs.update({"counter", "zicntr"})
        if name == "pmpaddr":
            needs.add("pmp")
    return sorted(needs)


def exit_protocol(backend: str) -> dict:
    if backend == "gem5":
        return {
            "backend": backend,
            "pass": "m5_exit pseudo instruction",
            "fail": "m5_fail pseudo instruction with code=1",
            "note": "gem5-only protocol; not intended for XiangShan/NEMU",
        }
    if backend == "xiangshan":
        return {
            "backend": backend,
            "pass": "a0=0 then ebreak",
            "fail": "a0=1 then ebreak",
            "note": "GOOD TRAP compatible protocol for XiangShan/NEMU style runners",
        }
    if backend == "wally":
        return {
            "backend": backend,
            "pass": "store word to symbol tohost, which Wally testbench treats as TestComplete",
            "fail": "a0=1 then ebreak; runner classifies lack of Wally pass marker as non-pass",
            "note": "Wally single-ELF Verilator testbench terminates on tohost store and does not signature-check single ELF tests",
        }
    raise ValueError(f"unknown exit backend: {backend}")


def backend_define(backend: str) -> str:
    return f"-DRVTEST_EXIT_BACKEND_{backend.upper()}"


def enumerate_sources(isa_dir: Path, suites: list[str], only: set[str]) -> list[dict]:
    tests: list[dict] = []
    for suite in suites:
        suite_dir = isa_dir / suite
        if not suite_dir.is_dir():
            tests.append(
                {
                    "id": f"{suite}-missing-suite",
                    "suite": suite,
                    "name": "missing-suite",
                    "source": str(suite_dir),
                    "status": "blocked_missing_artifact",
                    "reason": f"missing suite directory: {suite_dir}",
                    "needs": [],
                }
            )
            continue
        for src in sorted(suite_dir.glob("*.S")):
            name = src.stem
            test_ref = f"{suite}/{name}"
            if only and test_ref not in only and name not in only:
                continue
            target = f"{suite}-p-{name}"
            tests.append(
                {
                    "id": target,
                    "suite": suite,
                    "name": name,
                    "source": str(src),
                    "status": "unsupported",
                    "reason": "enumerated but not built yet",
                    "needs": classify_needs(suite, name),
                    "exit_protocol": exit_protocol("xiangshan"),
                    "exit_protocols": {backend: exit_protocol(backend) for backend in DEFAULT_BACKENDS},
                    "compatible_backends": list(DEFAULT_BACKENDS),
                }
            )
    return tests


def run_command(command: list[str], log_path: Path, timeout: int) -> tuple[str, int | None]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8") as log:
        log.write("$ " + " ".join(command) + "\n\n")
        try:
            completed = subprocess.run(
                command,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            log.write(f"\nTIMEOUT after {timeout}s\n")
            return "timeout", None
    return ("pass", completed.returncode) if completed.returncode == 0 else ("fail", completed.returncode)


def build_one(
    test: dict,
    root: Path,
    isa_dir: Path,
    build_root: Path,
    tools: dict[str, str],
    arch: str,
    abi: str,
    timeout: int,
    backend: str,
) -> None:
    target = test["id"]
    out_dir = build_root / backend / test["suite"] / target
    elf = out_dir / target
    bin_path = out_dir / f"{target}.bin"
    dump = out_dir / f"{target}.dump"
    build_log = out_dir / f"{target}.build.log"
    command = [
        tools["gcc"],
        f"-march={arch}",
        f"-mabi={abi}",
        "-mcmodel=medany",
        "-O2",
        "-g",
        "-ffreestanding",
        "-fno-pic",
        "-fno-pie",
        backend_define(backend),
        "-static",
        "-nostdlib",
        "-nostartfiles",
        "-Wl,--gc-sections",
        "-Wl,--check-sections",
        "-T",
        str(root / "link.lds"),
        "-I",
        str(isa_dir),
        "-I",
        str(isa_dir / "macros" / "scalar"),
        str(root / "start.S"),
        str(Path(test["source"])),
        "-o",
        str(elf),
    ]
    status, rc = run_command(command, build_log, timeout)
    test.setdefault("builds", {})[backend] = {
        "command": command,
        "return_code": rc,
        "status": status,
        "log_path": str(build_log),
    }
    test["status"] = status if status != "pass" else test.get("status", "pass")
    test["reason"] = "compiled" if status == "pass" else f"{backend} build {status}"
    test["arch"] = arch
    test["abi"] = abi
    artifacts = {
        "elf": str(elf),
        "bin": str(bin_path),
        "dump": str(dump),
        "build_log": str(build_log),
    }
    test.setdefault("artifacts_by_backend", {})[backend] = artifacts
    if backend == "xiangshan" or "artifacts" not in test:
        test["artifacts"] = artifacts
        test["build"] = test["builds"][backend]
        test["exit_protocol"] = exit_protocol(backend)
    if status != "pass":
        return
    objcopy_status, objcopy_rc = run_command([tools["objcopy"], "-O", "binary", str(elf), str(bin_path)], build_log.with_suffix(".objcopy.log"), timeout)
    objdump_status, objdump_rc = run_command([tools["objdump"], "-alDS", "-M", "no-aliases", str(elf)], dump, timeout)
    if objcopy_status != "pass" or objdump_status != "pass" or not bin_path.exists() or bin_path.stat().st_size == 0:
        test["status"] = "fail"
        test["reason"] = f"{backend} post-link artifact generation failed"
        test["builds"][backend]["post"] = {
            "objcopy_status": objcopy_status,
            "objcopy_return_code": objcopy_rc,
            "objdump_status": objdump_status,
            "objdump_return_code": objdump_rc,
        }


def summarize(tests: list[dict]) -> dict:
    by_status = {status: 0 for status in sorted(STATUSES)}
    by_suite: dict[str, dict[str, int]] = {}
    for test in tests:
        status = test.get("status", "unsupported")
        by_status[status] = by_status.get(status, 0) + 1
        suite = test["suite"]
        by_suite.setdefault(suite, {s: 0 for s in sorted(STATUSES)})
        by_suite[suite][status] = by_suite[suite].get(status, 0) + 1
    return {"total": len(tests), "by_status": by_status, "by_suite": by_suite}


def main(argv: list[str]) -> int:
    root, isa_dir, default_build = repo_paths()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", action="append", choices=DEFAULT_SUITES, help="suite to include; defaults to all RV64 gate suites")
    parser.add_argument("--test", action="append", default=[], help="test filter: either name or suite/name")
    parser.add_argument("--build-dir", type=Path, default=default_build)
    parser.add_argument("--manifest", type=Path, default=None)
    parser.add_argument("--manifest-only", action="store_true")
    parser.add_argument("--timeout", type=int, default=int(os.environ.get("RISCV_TEST_BUILD_TIMEOUT", "60")))
    parser.add_argument("--arch", default=os.environ.get("RISCV_ARCH", "rv64ima_zicsr_zifencei"))
    parser.add_argument("--abi", default=os.environ.get("RISCV_ABI", "lp64"))
    parser.add_argument("--backend", action="append", choices=DEFAULT_BACKENDS, help="exit backend to build; defaults to xiangshan and gem5")
    args = parser.parse_args(argv)

    suites = args.suite or list(DEFAULT_SUITES)
    backends = args.backend or list(DEFAULT_BACKENDS)
    only = set(args.test)
    build_root = args.build_dir.resolve()
    manifest_path = args.manifest or build_root / "manifest.json"
    tests = enumerate_sources(isa_dir, suites, only)
    for test in tests:
        if test.get("status") != "blocked_missing_artifact":
            test["exit_protocols"] = {backend: exit_protocol(backend) for backend in backends}
            test["compatible_backends"] = list(backends)

    gcc = find_tool("RISCV_GCC", ("riscv64-unknown-elf-gcc", "riscv64-linux-gnu-gcc"))
    objcopy = find_tool("RISCV_OBJCOPY", ("riscv64-unknown-elf-objcopy", "riscv64-linux-gnu-objcopy"))
    objdump = find_tool("RISCV_OBJDUMP", ("riscv64-unknown-elf-objdump", "riscv64-linux-gnu-objdump"))
    tools = {"gcc": gcc, "objcopy": objcopy, "objdump": objdump}

    if args.manifest_only:
        for test in tests:
            test["status"] = "unsupported"
            test["reason"] = "manifest-only mode"
    elif not all(tools.values()):
        missing = ", ".join(name for name, value in tools.items() if not value)
        for test in tests:
            test["status"] = "blocked_env"
            test["reason"] = f"missing RISC-V toolchain component(s): {missing}"
    else:
        for test in tests:
            if test.get("status") == "blocked_missing_artifact":
                continue
            test["status"] = "pass"
            test["reason"] = "compiled"
            for backend in backends:
                build_one(
                    test,
                    root,
                    isa_dir,
                    build_root,
                    {k: str(v) for k, v in tools.items() if v},
                    suite_arch(test["suite"], args.arch),
                    args.abi,
                    args.timeout,
                    backend,
                )
                if test["status"] != "pass":
                    break

    manifest = {
        "schema_version": 1,
        "generated_at": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "generator": str(Path(__file__).resolve()),
        "isa_dir": str(isa_dir),
        "build_dir": str(build_root),
        "exit_backends": backends,
        "toolchain": tools,
        "status_enum": sorted(STATUSES),
        "suites": suites,
        "summary": summarize(tests),
        "tests": tests,
    }
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(json.dumps(manifest["summary"], indent=2, sort_keys=True))
    print(f"manifest={manifest_path}")
    return 0 if manifest["summary"]["by_status"].get("fail", 0) == 0 and manifest["summary"]["by_status"].get("blocked_env", 0) == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
