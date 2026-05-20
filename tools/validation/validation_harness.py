#!/usr/bin/env python3
"""Four-layer validation harness for module deletion/degradation experiments."""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import shlex
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


STATUS_ENUM = (
    "pass",
    "fail",
    "timeout",
    "unsupported",
    "blocked_missing_artifact",
    "blocked_env",
)

LAYER_NAMES = {
    "l1": "L1 compile/build",
    "l2": "L2 module simulation",
    "l25": "L2.5 CORE-V Wally lightweight integration",
    "l3": "L3 full-system staged simulation",
}


@dataclass(frozen=True)
class Gate:
    layer: str
    target: str
    workload: str
    host: str
    cwd: str
    command: str
    timeout: int
    requires_artifact: str | None = None
    parser: str = "return-code"


@dataclass
class Result:
    layer: str
    target: str
    workload: str
    host: str
    cwd: str
    command: str
    return_code: int | None
    status: str
    log_path: str
    stats_path: str | None
    exit_reason: str
    difftest: str | None
    simInsts: int | None
    instrCnt: int | None
    cycles: int | None
    IPC: float | None
    host_time: float | None


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def default_manifest(root: Path) -> Path:
    return root / "baremetal" / "riscv-tests" / "isa" / "build" / "manifest.json"


def load_manifest(path: Path) -> dict:
    if not path.exists():
        return {
            "summary": {"total": 0, "by_status": {}, "by_suite": {}},
            "tests": [],
            "missing": str(path),
        }
    return json.loads(path.read_text(encoding="utf-8"))


def shell_join(command: Iterable[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def default_linux_root(root: Path) -> Path:
    root_s = str(root)
    if root_s.startswith("/Users/zhuyanbo/"):
        return Path("/mnt/hgfs/zhuyanbo") / root_s.removeprefix("/Users/zhuyanbo/")
    return root


def default_eda_root() -> Path:
    return Path("/nfs_global/I/qimeng2/zhuyanbo/module-deletion-test")


def build_gates(root: Path, manifest: dict, linux_root: Path, eda_root: Path) -> list[Gate]:
    linux_root_s = str(linux_root)
    eda_root_s = str(eda_root)
    manifest_path = str(linux_root / "baremetal" / "riscv-tests" / "isa" / "build" / "manifest.json")
    total = manifest.get("summary", {}).get("total", 0)
    gem5_bin = "GEM5/build/RISCV/gem5.opt"
    l3_isa_cmd = f"tools/validation/run-l3-gem5 --stage isa --manifest {shlex.quote(manifest_path)} --output build/validation/l3-gem5/isa/results.json"
    return [
        Gate(
            layer="l1",
            target="riscv-tests-build",
            workload=f"rv64ui/rv64um/rv64mi manifest ({total} tests)",
            host="linux",
            cwd=linux_root_s,
            command="./baremetal/build-riscv-tests-manifest.sh --timeout 60",
            timeout=300,
        ),
        Gate(
            layer="l1",
            target="gem5-entry",
            workload=gem5_bin,
            host="linux",
            cwd=linux_root_s,
            command=f"test -x {shlex.quote(gem5_bin)}",
            timeout=30,
            requires_artifact=gem5_bin,
        ),
        Gate(
            layer="l1",
            target="xiangshan-linux-compile",
            workload="xiangshan.compile",
            host="linux",
            cwd=f"{linux_root_s}/XiangShan",
            command="../tools/validation/run-xiangshan-linux-checks --mode compile --output ../build/validation/xiangshan-linux-compile/results.json",
            timeout=900,
        ),
        Gate(
            layer="l1",
            target="xiangshan-linux-bpu-scalatest",
            workload="SaturateCounterTest/SignedSaturateCounterTest",
            host="linux",
            cwd=f"{linux_root_s}/XiangShan",
            command="../tools/validation/run-xiangshan-linux-checks --mode bpu-scalatest --output ../build/validation/xiangshan-linux-bpu-scalatest/results.json",
            timeout=900,
        ),
        Gate(
            layer="l1",
            target="wally-prepare",
            workload="CORE-V Wally checkout/build preflight",
            host="linux",
            cwd=linux_root_s,
            command="tools/validation/probe-wally --prepare --output build/validation/wally-prepare/results.json",
            timeout=1800,
        ),
        Gate(
            layer="l2",
            target="gem5-bpu-unit",
            workload="tage.test/mgsc.test/btb_tage.test",
            host="linux",
            cwd=linux_root_s,
            command="tools/validation/run-gem5-bpu-tests --output build/validation/gem5-bpu/results.json",
            timeout=1800,
        ),
        Gate(
            layer="l2",
            target="xiangshan-bpu-schema",
            workload="predict/resolve/redirect/flush/train/commit schema",
            host="linux",
            cwd=linux_root_s,
            command="tools/validation/check-bpu-trace-schema --sample --output build/validation/bpu-trace-schema/results.json",
            timeout=60,
        ),
        Gate(
            layer="l25",
            target="wally-isa-matrix",
            workload=f"rv64ui/rv64um/rv64mi manifest ({total} tests)",
            host="linux",
            cwd=linux_root_s,
            command=f"tools/validation/probe-wally --manifest {shlex.quote(manifest_path)} --output build/validation/wally-isa-matrix/results.json",
            timeout=3600,
        ),
        Gate(
            layer="l25",
            target="wally-benchmark",
            workload="CoreMark or Dhrystone smoke",
            host="linux",
            cwd=linux_root_s,
            command="tools/validation/probe-wally --benchmark coremark --output build/validation/wally-benchmark/results.json",
            timeout=1800,
        ),
        Gate(
            layer="l3",
            target="gem5-isa-first-layer",
            workload=f"complete rv64ui/rv64um/rv64mi manifest ({total} tests)",
            host="linux",
            cwd=linux_root_s,
            command=l3_isa_cmd,
            timeout=7200,
        ),
        Gate(
            layer="l3",
            target="gem5-medium",
            workload="CoreMark/Dhrystone/microbench",
            host="linux",
            cwd=linux_root_s,
            command="tools/validation/run-l3-gem5 --stage medium --output build/validation/l3-gem5/medium/results.json",
            timeout=7200,
        ),
        Gate(
            layer="l3",
            target="xiangshan-emu-isa",
            workload="complete rv64ui/rv64um/rv64mi manifest",
            host="eda-00",
            cwd=f"{eda_root_s}/XiangShan",
            command="make emu && tools/validation/run-l3-xiangshan --stage isa",
            timeout=14400,
        ),
        Gate(
            layer="l3",
            target="spec2006-checkpoint",
            workload="SPEC2006 checkpoint manifest",
            host="linux/eda-00",
            cwd=linux_root_s,
            command="tools/validation/run-l3 --stage spec2006 --checkpoint-manifest <provided-by-user>",
            timeout=28800,
            requires_artifact="SPEC2006 checkpoint manifest",
        ),
    ]


def selected_gates(gates: list[Gate], layer: str, target: str) -> list[Gate]:
    if layer != "all":
        gates = [gate for gate in gates if gate.layer == layer]
    if target != "all":
        gates = [gate for gate in gates if gate.target == target or gate.target.startswith(target)]
    return gates


def log_file(output_dir: Path, gate: Gate) -> Path:
    safe = f"{gate.layer}-{gate.target}-{gate.workload}".replace("/", "_").replace(" ", "_")
    safe = "".join(ch for ch in safe if ch.isalnum() or ch in "._-")[:180]
    return output_dir / "logs" / f"{safe}.log"


def remote_command(gate: Gate) -> str:
    inner = f"cd {shlex.quote(gate.cwd)} && {gate.command}"
    if gate.host == "local":
        return inner
    if gate.host == "linux":
        return "ssh linux " + shlex.quote(f"bash -lc {shlex.quote(inner)}")
    if gate.host == "eda-00":
        eda_inner = "ssh eda-00 " + shlex.quote("bash -lc " + shlex.quote(inner))
        return "ssh q2 " + shlex.quote(f"bash -lc {shlex.quote(eda_inner)}")
    return gate.command


def dry_result(gate: Gate, log_path: Path) -> Result:
    return Result(
        layer=gate.layer,
        target=gate.target,
        workload=gate.workload,
        host=gate.host,
        cwd=gate.cwd,
        command=remote_command(gate),
        return_code=None,
        status="unsupported",
        log_path=str(log_path),
        stats_path=None,
        exit_reason="dry-run only; command not executed",
        difftest=None,
        simInsts=None,
        instrCnt=None,
        cycles=None,
        IPC=None,
        host_time=None,
    )


def execute_gate(gate: Gate, log_path: Path) -> Result:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    command = remote_command(gate)
    with log_path.open("w", encoding="utf-8") as log:
        log.write("$ " + command + "\n\n")
        try:
            completed = subprocess.run(
                command,
                shell=True,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=gate.timeout,
                check=False,
                executable="/bin/bash",
            )
            rc = completed.returncode
            status = "pass" if rc == 0 else "fail"
            exit_reason = "return-code"
        except subprocess.TimeoutExpired:
            rc = None
            status = "timeout"
            exit_reason = f"timeout after {gate.timeout}s"
            log.write(f"\n{exit_reason}\n")
    marker_status, marker_reason = parse_validation_marker(log_path)
    if marker_status:
        status = marker_status
        exit_reason = marker_reason or exit_reason
    return Result(
        layer=gate.layer,
        target=gate.target,
        workload=gate.workload,
        host=gate.host,
        cwd=gate.cwd,
        command=command,
        return_code=rc,
        status=status,
        log_path=str(log_path),
        stats_path=None,
        exit_reason=exit_reason,
        difftest=None,
        simInsts=None,
        instrCnt=None,
        cycles=None,
        IPC=None,
        host_time=round(time.monotonic() - start, 3),
    )


def parse_validation_marker(log_path: Path) -> tuple[str | None, str | None]:
    if not log_path.exists():
        return None, None
    status = None
    reason = None
    for line in log_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("VALIDATION_STATUS="):
            candidate = line.split("=", 1)[1].strip()
            if candidate in STATUS_ENUM:
                status = candidate
        elif line.startswith("VALIDATION_REASON="):
            reason = line.split("=", 1)[1].strip()
    return status, reason


def write_outputs(output_dir: Path, results: list[Result], manifest: dict, dry_run: bool) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    payload = {
        "schema_version": 1,
        "generated_at": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "dry_run": dry_run,
        "status_enum": STATUS_ENUM,
        "layers": LAYER_NAMES,
        "manifest_summary": manifest.get("summary", {}),
        "results": [asdict(result) for result in results],
    }
    (output_dir / "results.json").write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    lines = [
        "# Validation Harness Results",
        "",
        f"- dry_run: `{str(dry_run).lower()}`",
        f"- results: `{len(results)}`",
        f"- manifest_total: `{manifest.get('summary', {}).get('total', 0)}`",
        "",
        "| layer | target | workload | host | status | exit_reason |",
        "|---|---|---|---|---|---|",
    ]
    for result in results:
        lines.append(
            f"| {result.layer} | {result.target} | {result.workload} | {result.host} | {result.status} | {result.exit_reason} |"
        )
    (output_dir / "summary.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    root = repo_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--layer", choices=("all", *LAYER_NAMES.keys()), default="all")
    parser.add_argument("--target", default="all")
    parser.add_argument("--manifest", type=Path, default=default_manifest(root))
    parser.add_argument("--linux-root", type=Path, default=Path(os.environ.get("VALIDATION_LINUX_ROOT", default_linux_root(root))))
    parser.add_argument("--eda-root", type=Path, default=Path(os.environ.get("VALIDATION_EDA_ROOT", default_eda_root())))
    parser.add_argument("--output-dir", type=Path, default=None)
    parser.add_argument("--dry-run", dest="dry_run", action="store_true", default=True)
    parser.add_argument("--no-dry-run", dest="dry_run", action="store_false")
    parser.add_argument("--list", action="store_true", help="print selected gates as JSON and exit")
    args = parser.parse_args(argv)

    manifest = load_manifest(args.manifest)
    gates = selected_gates(build_gates(root, manifest, args.linux_root, args.eda_root), args.layer, args.target)
    if args.list:
        print(json.dumps([asdict(gate) for gate in gates], indent=2, sort_keys=True))
        return 0

    run_id = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    output_dir = args.output_dir or (root / "build" / "validation" / run_id)
    results: list[Result] = []
    for gate in gates:
        log_path = log_file(output_dir, gate)
        results.append(dry_result(gate, log_path) if args.dry_run else execute_gate(gate, log_path))
    write_outputs(output_dir, results, manifest, args.dry_run)
    print(f"results={output_dir / 'results.json'}")
    print(f"summary={output_dir / 'summary.md'}")
    if args.dry_run:
        for gate in gates:
            print(f"[dry-run] {gate.layer} {gate.target} host={gate.host} command={remote_command(gate)}")
    return 0 if args.dry_run or all(result.status == "pass" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
