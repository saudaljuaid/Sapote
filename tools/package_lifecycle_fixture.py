#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the deterministic signed repository used by the guest sap proof."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


PACKAGE = load("sapote_package_lifecycle", ROOT / "tools" / "sapote-package.py")
REPOSITORY = load(
    "sapote_repository_lifecycle", ROOT / "tools" / "sapote-repository.py"
)

ROOT_SEED = bytes(range(32))
PUBLISHER_SEED = bytes(range(32, 64))
GENERATED = 1_800_000_000
EXPIRES = GENERATED + 86_400


def build(output: Path, executable: Path) -> dict[str, object]:
    payload = executable.read_bytes()
    package_spec = {
        "format": 3,
        "architecture": "x86_64",
        "abi_min": 1,
        "abi_max": 1,
        "identifier": "org.sapote.proof",
        "name": "Sapote Installed Proof",
        "version": "1.0.0",
        "publisher": "Sapote Development Publisher",
        "capabilities": ["console"],
        "dependencies": [],
        "conflicts": [],
    }
    package = PACKAGE.build_package_v3(
        package_spec,
        ({
            "path": "bin/proof.app",
            "kind": "executable",
            "mode": 0o555,
            "payload": payload,
        },),
        PUBLISHER_SEED,
    )
    publisher_public = PACKAGE._ed25519_public_bytes_from_private(
        PUBLISHER_SEED
    )
    publisher_key_id = hashlib.sha256(publisher_public).hexdigest()
    download_path = "packages/org.sapote.proof/1.0.0.spk"
    repository_spec = {
        "format": 1,
        "repository": "org.sapote.main",
        "repository_version": 42,
        "generated_at": GENERATED,
        "expires_at": EXPIRES,
        "architecture": "x86_64",
        "abi_min": 1,
        "abi_max": 1,
        "packages": [{
            "identifier": "org.sapote.proof",
            "version": "1.0.0",
            "download_path": download_path,
            "bytes": len(package),
            "sha256": hashlib.sha256(package).hexdigest(),
            "publisher_key_id": publisher_key_id,
            "dependencies": [],
            "conflicts": [],
            "provides": [],
        }],
    }
    repository = REPOSITORY.build_repository(repository_spec, ROOT_SEED)
    root_public = PACKAGE._ed25519_public_bytes_from_private(ROOT_SEED)
    REPOSITORY.parse_repository(
        repository,
        trusted_root_keys={hashlib.sha256(root_public).hexdigest(): root_public},
        now=GENERATED + 60,
        minimum_repository_version=42,
    )
    PACKAGE.parse_package(
        package,
        trusted_keys={publisher_key_id: publisher_public},
    )
    package_path = output / download_path
    package_path.parent.mkdir(parents=True, exist_ok=True)
    PACKAGE.atomic_write(package_path, package)
    PACKAGE.atomic_write(output / "repository.sri", repository)
    return {
        "output": str(output),
        "repository_bytes": len(repository),
        "repository_sha256": hashlib.sha256(repository).hexdigest().upper(),
        "package_bytes": len(package),
        "package_sha256": hashlib.sha256(package).hexdigest().upper(),
        "payload_sha256": hashlib.sha256(payload).hexdigest().upper(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--executable", type=Path, required=True)
    args = parser.parse_args()
    if not PACKAGE.ed25519_available():
        raise RuntimeError("Python Ed25519 support is required")
    print(json.dumps(build(args.output, args.executable), sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
