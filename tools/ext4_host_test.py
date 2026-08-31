#!/usr/bin/env python3
"""Host tests for tools/ext4_image.py.

Pure parser/refusal tests run everywhere.  The end-to-end tests run
automatically when mke2fs, debugfs, and e2fsck are present; otherwise they are
reported as skipped with the package name needed by Linux CI.
"""

from __future__ import annotations

import filecmp
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock
import uuid


sys.path.insert(0, str(Path(__file__).resolve().parent))
import ext4_image as ext4  # noqa: E402


def image_difference_summary(left: Path, right: Path) -> str:
    """Render bounded byte/range evidence when reproducibility regresses."""
    left_bytes = left.read_bytes()
    right_bytes = right.read_bytes()
    first: list[str] = []
    ranges: list[list[int]] = []
    different = 0
    for offset, (left_byte, right_byte) in enumerate(zip(left_bytes, right_bytes)):
        if left_byte == right_byte:
            continue
        different += 1
        if len(first) < 64:
            first.append(f"0x{offset:x}:{left_byte:02x}->{right_byte:02x}")
        if not ranges or offset != ranges[-1][1] + 1:
            if len(ranges) < 64:
                ranges.append([offset, offset])
        elif len(ranges) <= 64:
            ranges[-1][1] = offset
    if len(left_bytes) != len(right_bytes):
        different += abs(len(left_bytes) - len(right_bytes))
    rendered_ranges = ", ".join(
        f"0x{start:x}-0x{end:x}" for start, end in ranges)
    return (
        f"different_bytes={different}; first={','.join(first)}; "
        f"ranges={rendered_ranges}")


def fake_profile_image() -> bytearray:
    data = bytearray(16 * ext4.BLOCK_BYTES)
    sb = 1024
    struct.pack_into("<I", data, sb + 0x00, 64)  # inodes
    struct.pack_into("<I", data, sb + 0x04, 16)  # blocks low
    struct.pack_into("<I", data, sb + 0x0C, 4)  # free blocks low
    struct.pack_into("<I", data, sb + 0x10, 32)  # free inodes
    struct.pack_into("<I", data, sb + 0x18, 2)  # 4096-byte blocks
    struct.pack_into("<I", data, sb + 0x20, 16)
    struct.pack_into("<I", data, sb + 0x28, 64)
    struct.pack_into("<I", data, sb + 0x30, ext4.FIXED_EPOCH)
    struct.pack_into("<H", data, sb + 0x38, 0xEF53)
    struct.pack_into("<H", data, sb + 0x58, ext4.INODE_BYTES)
    struct.pack_into("<I", data, sb + 0x5C, 0x0004 | 0x0008 | 0x0020)
    struct.pack_into("<I", data, sb + 0x60, 0x0002 | 0x0040 | 0x0080 | 0x2000)
    struct.pack_into("<I", data, sb + 0x64, 0x0001 | 0x0002 | 0x0008 | 0x0020 | 0x0040 | 0x0400)
    data[sb + 0x68 : sb + 0x78] = uuid.UUID(ext4.FILESYSTEM_UUID).bytes
    data[sb + 0x78 : sb + 0x78 + len(ext4.VOLUME_LABEL)] = ext4.VOLUME_LABEL.encode("ascii")
    data[sb + 0x88 : sb + 0x88 + len(ext4.LAST_MOUNTED)] = ext4.LAST_MOUNTED.encode("ascii")
    struct.pack_into("<I", data, sb + 0x108, ext4.FIXED_EPOCH)
    return data


class SuperblockTests(unittest.TestCase):
    def test_accepts_exact_sapote_profile(self) -> None:
        report = ext4.parse_superblock(fake_profile_image())
        self.assertEqual(report["block_size"], 4096)
        self.assertEqual(report["uuid"], ext4.FILESYSTEM_UUID)
        self.assertEqual(report["features"], sorted(ext4.FEATURES))

    def test_refuses_truncation(self) -> None:
        with self.assertRaisesRegex(ext4.Ext4ImageError, "too small"):
            ext4.parse_superblock(bytearray(1024))

    def test_refuses_unknown_incompatible_feature(self) -> None:
        data = fake_profile_image()
        struct.pack_into("<I", data, 1024 + 0x60, struct.unpack_from("<I", data, 1024 + 0x60)[0] | 0x10000)
        with self.assertRaisesRegex(ext4.Ext4ImageError, "unsupported incompatible"):
            ext4.parse_superblock(data)

    def test_refuses_missing_required_feature(self) -> None:
        data = fake_profile_image()
        struct.pack_into("<I", data, 1024 + 0x60, struct.unpack_from("<I", data, 1024 + 0x60)[0] & ~0x40)
        with self.assertRaisesRegex(ext4.Ext4ImageError, "missing required.*extents"):
            ext4.parse_superblock(data)

    def test_refuses_out_of_range_block_count(self) -> None:
        data = fake_profile_image()
        struct.pack_into("<I", data, 1024 + 0x04, 17)
        with self.assertRaisesRegex(ext4.Ext4ImageError, "exceeds image capacity"):
            ext4.parse_superblock(data)

    def test_all_structural_mutations_are_refused(self) -> None:
        kinds = (
            "magic",
            "block-size",
            "missing-extents",
            "missing-64bit",
            "missing-metadata-csum",
            "unsupported-incompat",
            "blocks-range",
        )
        for kind in kinds:
            with self.subTest(kind=kind):
                data = fake_profile_image()
                ext4._mutation(kind, data, Path("unused"), {})
                with self.assertRaises(ext4.Ext4ImageError):
                    ext4.parse_superblock(data)


class ToolDiscoveryTests(unittest.TestCase):
    def test_tool_environment_pins_mkfs_debugfs_and_e2fsck_time(self) -> None:
        environment = ext4._tool_env()
        self.assertEqual(environment["SOURCE_DATE_EPOCH"], str(ext4.FIXED_EPOCH))
        self.assertEqual(environment["E2FSPROGS_FAKE_TIME"], str(ext4.FIXED_EPOCH))
        self.assertEqual(environment["E2FSCK_TIME"], str(ext4.FIXED_EPOCH))

    def test_missing_tool_names_e2fsprogs_and_linux_install_command(self) -> None:
        with mock.patch.object(ext4.shutil, "which", return_value=None):
            with self.assertRaisesRegex(
                ext4.Ext4ImageError,
                "mke2fs.*e2fsprogs.*apt-get install e2fsprogs",
            ):
                ext4.require_tools()

    def test_available_tools_is_all_or_nothing(self) -> None:
        with mock.patch.object(ext4.shutil, "which", side_effect=lambda name: f"/bin/{name}" if name != "debugfs" else None):
            self.assertEqual(ext4.available_tools(), {})


class FixtureScriptTests(unittest.TestCase):
    def test_format_invocation_and_debugfs_script_pin_reproducibility_inputs(self) -> None:
        completed = mock.Mock(returncode=0, stdout="")
        with tempfile.TemporaryDirectory(prefix="sapote-ext4-script-test-") as raw:
            root = Path(raw)
            image = root / "fixture.img"
            with mock.patch.object(ext4, "_run", return_value=completed) as run:
                ext4._format_image(
                    image,
                    {"mke2fs": "/bin/mke2fs", "debugfs": "/bin/debugfs", "e2fsck": "/bin/e2fsck"},
                    root,
                )

            mkfs_argv = list(run.call_args_list[0].args[0])
            self.assertEqual(mkfs_argv[mkfs_argv.index("-b") + 1], "4096")
            self.assertEqual(mkfs_argv[mkfs_argv.index("-U") + 1], ext4.FILESYSTEM_UUID)
            self.assertEqual(mkfs_argv[mkfs_argv.index("-O") + 1], "none," + ",".join(ext4.FEATURES))
            self.assertIn(f"hash_seed={ext4.HASH_SEED_UUID}", mkfs_argv[mkfs_argv.index("-E") + 1])
            self.assertEqual(run.call_args_list[0].kwargs["extra_env"]["MKE2FS_CONFIG"], str(root / "mke2fs.conf"))

            script = (root / "debugfs.commands").read_text(encoding="utf-8")
            self.assertIn(f"set_current_time @{ext4.FIXED_EPOCH}", script)
            self.assertIn("set_inode_field / mode 040755", script)
            self.assertIn(f"set_super_value mkfs_time @{ext4.FIXED_EPOCH}", script)
            self.assertIn(f"ea_set /data/user/state.txt {ext4.XATTR_NAME} {ext4.XATTR_VALUE}", script)
            self.assertIn(f"set_inode_field /data/user/large-sparse.bin size {ext4.LARGE_SPARSE_BYTES}", script)
            self.assertEqual(script.count("/indexed/entry-"), 256)


@unittest.skipUnless(
    ext4.available_tools(),
    "requires mke2fs, debugfs, and e2fsck from the e2fsprogs package",
)
class E2fsprogsIntegrationTests(unittest.TestCase):
    def test_determinism_fixture_semantics_and_adversarial_refusals(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sapote-ext4-host-test-") as raw:
            root = Path(raw)
            first = root / "first.img"
            second = root / "second.img"
            first_report = ext4.build_image(first)
            second_report = ext4.build_image(second)

            if not filecmp.cmp(first, second, shallow=False):
                self.fail(
                    "two builds must be byte-identical: " +
                    image_difference_summary(first, second))
            self.assertEqual(first_report["sha256"], second_report["sha256"])
            self.assertEqual(first_report["block_size"], 4096)
            self.assertEqual(first_report["large_sparse_bytes"], ext4.LARGE_SPARSE_BYTES)
            self.assertEqual(first_report["symlink_target"], "state.txt")
            self.assertEqual(first_report["xattr"], {"name": ext4.XATTR_NAME, "value": ext4.XATTR_VALUE})

            for kind in ext4.MUTATIONS:
                with self.subTest(kind=kind):
                    malformed = root / f"malformed-{kind}.img"
                    ext4.malform_image(kind, first, malformed)
                    with self.assertRaises(ext4.Ext4ImageError):
                        ext4.inspect_image(malformed)
                    malformed.unlink()


if __name__ == "__main__":
    unittest.main(verbosity=2)
