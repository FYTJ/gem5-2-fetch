"""Detect SPEC2006 GCPT checkpoint layout from the checkpoint memory image."""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path


INSPECT_BYTES = 0xF1000
LEGACY_MAGIC_OFFSET = 0xF00
LEGACY_GPR_OFFSET = 0x1000
LEGACY_PC_OFFSET = 0x1200
LEGACY_CSR_OFFSET = 0x1300
EMBEDDED_CAFF_OFFSET = 0x888
EMBEDDED_BEEF_OFFSET = 0xECDB0
EMBEDDED_HSTATUS_OFFSET = 0xF0FF0


def _u16_le(data: bytes, offset: int) -> int | None:
    if offset + 2 > len(data):
        return None
    return int.from_bytes(data[offset : offset + 2], "little")


def _has_nonzero(data: bytes, start: int, size: int) -> bool:
    if start >= len(data):
        return False
    return any(data[start : min(start + size, len(data))])


def _read_zstd_prefix(path: Path, size: int) -> tuple[bytes, str | None]:
    zstd = shutil.which("zstd")
    if zstd is None:
        return b"", "zstd command is required to inspect compressed checkpoint layout"
    process = subprocess.Popen(
        [zstd, "-dc", str(path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    assert process.stdout is not None
    chunks: list[bytes] = []
    remaining = size
    try:
        while remaining > 0:
            chunk = process.stdout.read(min(65536, remaining))
            if not chunk:
                break
            chunks.append(chunk)
            remaining -= len(chunk)
    finally:
        process.stdout.close()
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
    return b"".join(chunks), None


def read_checkpoint_prefix(path: Path, size: int = INSPECT_BYTES) -> tuple[bytes, str | None]:
    if path.suffix == ".zstd":
        return _read_zstd_prefix(path, size)
    try:
        return path.read_bytes()[:size], None
    except OSError as exc:
        return b"", str(exc)


def detect_gcpt_layout(path: Path) -> dict:
    data, error = read_checkpoint_prefix(path)
    legacy_magic = _u16_le(data, LEGACY_MAGIC_OFFSET) == 0xBEEF
    legacy_payload = (
        _has_nonzero(data, LEGACY_GPR_OFFSET, 0x100)
        and _has_nonzero(data, LEGACY_PC_OFFSET, 0x20)
        and _has_nonzero(data, LEGACY_CSR_OFFSET, 0x100)
    )
    embedded_caff = _u16_le(data, EMBEDDED_CAFF_OFFSET) == 0xCAFF
    embedded_beef = _u16_le(data, EMBEDDED_BEEF_OFFSET) == 0xBEEF
    hstatus_payload = _has_nonzero(data, EMBEDDED_HSTATUS_OFFSET, 0x8)

    if embedded_caff and embedded_beef:
        layout = "embedded-rvh" if hstatus_payload else "embedded-gcpt"
    elif legacy_magic and legacy_payload:
        layout = "legacy-fixed"
    else:
        layout = "unknown"

    return {
        "layout": layout,
        "error": error,
        "bytes_inspected": len(data),
        "legacy_fixed": {
            "magic_beef_at_0xf00": legacy_magic,
            "payload_present": legacy_payload,
        },
        "embedded": {
            "magic_caff_at_0x888": embedded_caff,
            "magic_beef_at_0xecdb0": embedded_beef,
            "hstatus_payload_nonzero": hstatus_payload,
        },
        "requires_external_restorer": layout == "legacy-fixed",
        "requires_rvh": layout == "embedded-rvh",
        "requires_h_aware_ref": layout == "embedded-rvh",
    }
