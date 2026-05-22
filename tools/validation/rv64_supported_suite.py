"""Shared RV64 lightweight full-system suite derived from GEM5 L3 ISA results."""

from __future__ import annotations

import json
from collections import Counter
from pathlib import Path
from typing import Any


SUPPORTED_SUITE_ID = "supported-cross-77"
SOURCE_SUITE_ID = "supported-gem5-79"
EXPECTED_TOTAL = 77
EXPECTED_BY_SUITE = {
    "rv64ui": 52,
    "rv64um": 13,
    "rv64mi": 12,
}
KNOWN_UNSUPPORTED_ISA_TESTS = {
    "rv64ui-p-fence_i": "current GEM5 raw-cpt ISA gate does not validate fence.i / self-modifying-code coherence",
    "rv64ui-p-ma_data": "riscv-tests ma_data expects completed misaligned data accesses outside this lightweight gate contract",
    "rv64mi-p-csr": "privileged CSR side effects are outside the stable lightweight compatibility contract",
    "rv64mi-p-illegal": "privileged illegal-instruction trap flow does not naturally terminate under this runner",
    "rv64mi-p-instret_overflow": "minstret write/overflow semantics are not implemented as riscv-tests expects",
}
CROSS_TARGET_NONPASS_TESTS = {
    "rv64mi-p-scall": "CORE-V Wally direct run reaches the instruction limit before the expected tohost pass path; this ecall/trap-sensitive test is not part of the current cross-target default suite",
    "rv64mi-p-pmpaddr": "XiangShan emu/difftest reports a PMP-related state mismatch; this PMP-sensitive test is not part of the current cross-target default suite",
}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def default_gem5_results_path(root: Path | None = None) -> Path:
    base = root or repo_root()
    return base / "build" / "validation" / "l3-gem5" / "isa" / "results.json"


def default_manifest_path(root: Path | None = None) -> Path:
    base = root or repo_root()
    return base / "baremetal" / "riscv-tests" / "isa" / "build" / "manifest.json"


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def suite_from_test_id(test_id: str) -> str:
    return test_id.split("-p-", 1)[0] if "-p-" in test_id else "unknown"


def name_from_test_id(test_id: str) -> str:
    return test_id.split("-p-", 1)[1] if "-p-" in test_id else test_id


def _manifest_by_id(manifest_path: Path | None) -> dict[str, dict[str, Any]]:
    if manifest_path is None or not manifest_path.exists():
        return {}
    manifest = load_json(manifest_path)
    return {test.get("id"): test for test in manifest.get("tests", []) if test.get("id")}


def build_supported_suite(
    gem5_results_path: Path | None = None,
    manifest_path: Path | None = None,
    strict: bool = True,
) -> dict[str, Any]:
    root = repo_root()
    gem5_path = gem5_results_path or default_gem5_results_path(root)
    manifest = _manifest_by_id(manifest_path or default_manifest_path(root))
    gem5_payload = load_json(gem5_path)
    rows = gem5_payload.get("results", [])
    supported_rows = [row for row in rows if row.get("status") == "pass"]
    cross_target_excluded_ids = set(CROSS_TARGET_NONPASS_TESTS)
    selected_rows = [row for row in supported_rows if row.get("id") not in cross_target_excluded_ids]
    selected_ids = [row["id"] for row in selected_rows]
    by_suite = Counter(row.get("suite") or suite_from_test_id(row["id"]) for row in selected_rows)
    excluded = []
    for test_id, reason in KNOWN_UNSUPPORTED_ISA_TESTS.items():
        source_row = next((row for row in rows if row.get("id") == test_id), {})
        excluded.append(
            {
                "id": test_id,
                "suite": source_row.get("suite") or suite_from_test_id(test_id),
                "name": source_row.get("name") or manifest.get(test_id, {}).get("name") or name_from_test_id(test_id),
                "source_status": source_row.get("status"),
                "reason": reason,
            }
        )
    cross_target_excluded = []
    for test_id, reason in CROSS_TARGET_NONPASS_TESTS.items():
        source_row = next((row for row in rows if row.get("id") == test_id), {})
        cross_target_excluded.append(
            {
                "id": test_id,
                "suite": source_row.get("suite") or suite_from_test_id(test_id),
                "name": source_row.get("name") or manifest.get(test_id, {}).get("name") or name_from_test_id(test_id),
                "source_status": source_row.get("status"),
                "reason": reason,
            }
        )
    tests = []
    for row in selected_rows:
        test_id = row["id"]
        manifest_row = manifest.get(test_id, {})
        tests.append(
            {
                "id": test_id,
                "suite": row.get("suite") or manifest_row.get("suite") or suite_from_test_id(test_id),
                "name": row.get("name") or manifest_row.get("name") or name_from_test_id(test_id),
                "source_status": row.get("status"),
                "needs": manifest_row.get("needs", row.get("needs", [])),
            }
        )
    payload = {
        "schema_version": 1,
        "suite": SUPPORTED_SUITE_ID,
        "source_suite": SOURCE_SUITE_ID,
        "source": str(gem5_path),
        "source_status": gem5_payload.get("status"),
        "source_summary": gem5_payload.get("summary"),
        "summary": {
            "total": len(tests),
            "by_suite": {key: by_suite.get(key, 0) for key in sorted(EXPECTED_BY_SUITE)},
            "excluded_known_unsupported": len(excluded),
            "excluded_cross_target_nonpass": len(cross_target_excluded),
        },
        "excluded_known_unsupported": excluded,
        "excluded_cross_target_nonpass": cross_target_excluded,
        "ids": selected_ids,
        "tests": tests,
    }
    if strict:
        validate_supported_suite(payload)
    return payload


def validate_supported_suite(payload: dict[str, Any]) -> None:
    errors = []
    summary = payload.get("summary", {})
    if summary.get("total") != EXPECTED_TOTAL:
        errors.append(f"expected {EXPECTED_TOTAL} supported tests, got {summary.get('total')}")
    by_suite = summary.get("by_suite", {})
    for suite, expected in EXPECTED_BY_SUITE.items():
        if by_suite.get(suite) != expected:
            errors.append(f"expected {suite}={expected}, got {by_suite.get(suite)}")
    ids = set(payload.get("ids", []))
    unsupported_ids = set(KNOWN_UNSUPPORTED_ISA_TESTS)
    leaked = sorted(ids & unsupported_ids)
    if leaked:
        errors.append(f"known unsupported tests leaked into supported suite: {', '.join(leaked)}")
    cross_target_nonpass_ids = set(CROSS_TARGET_NONPASS_TESTS)
    leaked_cross_target = sorted(ids & cross_target_nonpass_ids)
    if leaked_cross_target:
        errors.append(f"cross-target non-pass tests leaked into default suite: {', '.join(leaked_cross_target)}")
    excluded = {row.get("id") for row in payload.get("excluded_known_unsupported", [])}
    missing_excluded = sorted(unsupported_ids - excluded)
    if missing_excluded:
        errors.append(f"missing known unsupported exclusions: {', '.join(missing_excluded)}")
    cross_target_excluded = {row.get("id") for row in payload.get("excluded_cross_target_nonpass", [])}
    missing_cross_target_excluded = sorted(cross_target_nonpass_ids - cross_target_excluded)
    if missing_cross_target_excluded:
        errors.append(f"missing cross-target non-pass exclusions: {', '.join(missing_cross_target_excluded)}")
    if errors:
        raise ValueError("; ".join(errors))
