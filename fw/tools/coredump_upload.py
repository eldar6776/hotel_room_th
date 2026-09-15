"""Preserve coredumps and initialize stale flash during serial firmware uploads.

ESP-IDF checks the size word before Arduino setup(), so repair belongs in the
uploader. Never change firmware logging or erase the whole flash for this.
"""

import hashlib
import importlib.util
import json
import re
import struct
import subprocess
import tempfile
from datetime import datetime, timezone
from pathlib import Path


def coredump_partition(table):
    """Validate the generated table before selecting an erase/write range."""
    table.verify()
    matches = [p for p in table if p.type == 1 and p.subtype == 3]
    if len(matches) != 1:
        raise ValueError("Expected exactly one coredump partition")
    part = matches[0]
    if part.encrypted:
        raise ValueError("Encrypted coredump partitions require a separate workflow")
    if part.offset < 0x9000 or part.offset % 4096 or part.size <= 0 or part.size % 4096:
        raise ValueError("Coredump partition must occupy complete flash sectors")
    return part


def invalid_size(data, expected_size):
    """Mirror IDF's size check; preserve blank and plausible crash records.

    This intentionally does not discard valid-size records based on a checksum
    or format assumption. Those records must remain available for crash analysis.
    """
    if len(data) != expected_size or expected_size < 4:
        raise ValueError("Incomplete coredump read; upload aborted")
    size = struct.unpack_from("<I", data)[0]
    return size != 0xFFFFFFFF and not 4 <= size <= expected_size


def prepare_image(data, part, backup_dir, port):
    """Save verified evidence before returning an optional blank upload image."""
    repair = invalid_size(data, part.size)
    backup_dir = Path(backup_dir)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    safe_port = re.sub(r"[^A-Za-z0-9_.-]", "_", port)
    folder = backup_dir / (stamp + "-" + safe_port)
    folder.mkdir(parents=True, exist_ok=False)
    backup = folder / "coredump-before.bin"
    backup.write_bytes(data)
    if backup.read_bytes() != data:
        raise OSError("Coredump backup verification failed; upload aborted")
    (folder / "report.json").write_text(json.dumps({
        "port": port,
        "offset": hex(part.offset),
        "size": hex(part.size),
        "header": data[:4].hex(),
        "sha256": hashlib.sha256(data).hexdigest(),
        "action": "initialize_invalid_size" if repair else "preserve",
    }, indent=2) + "\n", encoding="utf-8")
    if not repair:
        return None, folder
    image = folder / "coredump-blank.bin"
    image.write_bytes(b"\xff" * part.size)
    return image, folder


def before_upload(source, target, env):
    """Run after the build, before PlatformIO invokes its normal esptool upload."""
    if env.subst("$UPLOAD_PROTOCOL") != "esptool":
        raise ValueError("Coredump preflight currently supports serial esptool uploads only")

    framework = Path(env.PioPlatform().get_package_dir("framework-arduinoespressif32"))
    spec = importlib.util.spec_from_file_location("coredump_genpart", framework / "tools/gen_esp32part.py")
    genpart = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(genpart)
    table_path = Path(env.subst("$BUILD_DIR")) / "partitions.bin"
    table = genpart.PartitionTable.from_binary(table_path.read_bytes())
    part = coredump_partition(table)

    # Check actual upload images as well as logical partitions. Never add a
    # coredump write that could overlap an app, bootloader, or extra image.
    images = list(env.get("FLASH_EXTRA_IMAGES", []))
    images.append((env.subst("$ESP32_APP_OFFSET"), str(source[0])))
    for offset, filename in images:
        start = int(str(offset), 0)
        end = start + Path(env.subst(str(filename))).stat().st_size
        if start < part.offset + part.size and end > part.offset:
            raise ValueError("Upload image overlaps coredump partition: " + str(filename))

    env.AutodetectUploadPort()
    port = env.subst("$UPLOAD_PORT")
    backup_dir = Path(env.subst("$PROJECT_DIR")) / ".pio/coredump-backups"
    backup_dir.mkdir(parents=True, exist_ok=True)
    # A unique temporary read file avoids mixing evidence between devices/runs.
    with tempfile.TemporaryDirectory(prefix="read-", dir=backup_dir) as temp:
        output = Path(temp) / "coredump.bin"
        command = [env.subst("$PYTHONEXE"), env.subst("$UPLOADER"),
                   "--chip", env.BoardConfig().get("build.mcu"),
                   "--port", port, "--baud", env.subst("$UPLOAD_SPEED"),
                   "--before", "default_reset", "--after", "hard_reset",
                   "read_flash", hex(part.offset), hex(part.size), str(output)]
        print("[COREDUMP] Reading partition before upload...")
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if result.returncode:
            raise RuntimeError("Coredump read failed; upload aborted:\n" + result.stdout + result.stderr)
        image, folder = prepare_image(output.read_bytes(), part, backup_dir, port)
        (folder / "read-flash.log").write_text(result.stdout + result.stderr, encoding="utf-8")
        (folder / "partitions.bin").write_bytes(table_path.read_bytes())
    print("[COREDUMP] Backup: " + str(folder))
    if image is None:
        print("[COREDUMP] Blank or valid-size record; preserving partition.")
    else:
        # Add to the SAME write_flash command as the new firmware. Do not erase
        # an old app separately before we know the replacement upload can start.
        env.Append(UPLOADERFLAGS=[hex(part.offset), '"' + image.as_posix() + '"'])
        print("[COREDUMP] Invalid size; initializing only %s + %s during upload." %
              (hex(part.offset), hex(part.size)))


def register(env):
    env.AddPreAction("upload", before_upload)


# Importable by host regression tests without PlatformIO/SCons side effects.
try:
    Import("env")
except NameError:
    pass
else:
    register(env)
