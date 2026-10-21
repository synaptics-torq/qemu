#!/usr/bin/env python3

"""
Downloads an Astra SL image, extracts the rootfs, kernel image (including the baked in initrd) 
and kernel command line. It also build an overlay for the initrd with the kernel virtio drivers.
"""

import gzip
import os
import shutil
import struct
import subprocess
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

DOWNLOAD_URL = (
    "https://github.com/synaptics-astra/sdk/releases/download/"
    "scarthgap_6.12_v2.6.1/"
    "sl2619_coralboard_scarthgap_6.12_v2.6.1.zip"
)

SCRIPT_DIR = Path(__file__).resolve().parent
IMAGE_DIR = SCRIPT_DIR / "astra_sl_image"
ZIP_PATH = IMAGE_DIR / "image.zip"
EMMC_DIR = IMAGE_DIR / "eMMCimg"
OUTPUT_KERNEL = SCRIPT_DIR / "linux"
OUTPUT_CMDLINE = SCRIPT_DIR / "kernel_cmdline.txt"
OUTPUT_INITRAMFS = SCRIPT_DIR / "initramfs.cpio.gz"
KERNEL_ARTIFACTS = SCRIPT_DIR / "kernel" / "syna-kernel-artifacts.tgz"
# Member inside the artifacts tarball written by build_kos.sh: the exact
# kernel release the modules were built against (make kernelrelease with
# LOCALVERSION applied).
KERNEL_RELEASE_MEMBER = "kernel_release.txt"


def title(message):
    print(f"\033[1m{message}\033[0m")


def download_image():
    IMAGE_DIR.mkdir(parents=True, exist_ok=True)
    if ZIP_PATH.is_file() and ZIP_PATH.stat().st_size:
        print("Zip already present")
        return

    title("Downloading zip...")
    temporary_path = ZIP_PATH.with_suffix(".zip.tmp")
    try:
        with urllib.request.urlopen(DOWNLOAD_URL) as response:
            with temporary_path.open("wb") as output:
                shutil.copyfileobj(response, output)
        temporary_path.replace(ZIP_PATH)
    except Exception:
        temporary_path.unlink(missing_ok=True)
        raise


def extract_image():
    partitions = ("boot", "rootfs")
    if EMMC_DIR.is_dir() and all(
        (EMMC_DIR / f"{name}.subimg.gz").is_file()
        or (EMMC_DIR / f"{name}.subimg").is_file()
        for name in partitions
    ):
        print("Zip already uncompressed")
        return

    title("Uncompressing zip...")
    with zipfile.ZipFile(ZIP_PATH) as archive:
        archive.extractall(IMAGE_DIR)


def decompress_partition(name):
    source = EMMC_DIR / f"{name}.subimg.gz"
    destination = EMMC_DIR / f"{name}.subimg"
    if destination.is_file() and destination.stat().st_size:
        print(f"{name} partition already uncompressed")
        return
    if not source.is_file():
        raise FileNotFoundError(f"Partition archive not found: {source}")

    title(f"Uncompressing {name} partition...")
    temporary_path = destination.with_name(destination.name + ".tmp")
    try:
        with gzip.open(source, "rb") as compressed:
            with temporary_path.open("wb") as output:
                shutil.copyfileobj(compressed, output)
        temporary_path.replace(destination)
    except Exception:
        temporary_path.unlink(missing_ok=True)
        raise


def desparsify_boot_partition():
    boot_image = EMMC_DIR / "boot.subimg"
    raw_image = EMMC_DIR / "boot.raw"
    if raw_image.is_file() and raw_image.stat().st_size:
        print("Kernel partition already desparsified")
        return
    
    with boot_image.open("rb") as image:
        sparse_magic = image.read(4)
    if sparse_magic == b"\x3a\xff\x26\xed":
        title("Desparsifying kernel partition...")
        simg2img = shutil.which("simg2img")
        if simg2img is None:
            raise RuntimeError("simg2img is required to convert the sparse boot partition")
        subprocess.run([simg2img, str(boot_image), str(raw_image)], check=True)
    else:
        shutil.copyfile(boot_image, raw_image)


def extract_kernel_image(boot_image=None, output_kernel=None, output_cmdline=None):
    boot_image = Path(boot_image or EMMC_DIR / "boot.raw")
    output_kernel = Path(output_kernel or OUTPUT_KERNEL)
    output_cmdline = Path(output_cmdline or OUTPUT_CMDLINE)
    data = boot_image.read_bytes()

    android_offset = data.find(b"ANDROID!")
    if android_offset < 0 or android_offset + 40 > len(data):
        raise ValueError("Could not find a valid Android boot-image header")

    kernel_size = struct.unpack_from("<I", data, android_offset + 8)[0]
    page_size = struct.unpack_from("<I", data, android_offset + 36)[0]
    header_version = struct.unpack_from("<I", data, android_offset + 40)[0]
    if kernel_size == 0 or page_size == 0 or page_size & (page_size - 1):
        raise ValueError("Invalid Android kernel size or page size")
    if header_version > 2:
        raise ValueError(f"Unsupported Android boot header version: {header_version}")

    header_end = android_offset + 1632
    if header_end > len(data):
        raise ValueError("Android boot header is truncated before its command line")
    cmdline = " ".join(
        part.decode("ascii", "replace").strip()
        for part in (
            data[android_offset + 64 : android_offset + 64 + 512].split(b"\0", 1)[0],
            data[android_offset + 608 : android_offset + 608 + 1024].split(b"\0", 1)[0],
        )
        if part.strip()
    )
    output_cmdline.write_text(cmdline + "\n", encoding="utf-8")
    print(f"Kernel command line: {cmdline}")
    print(f"Kernel command line written to {output_cmdline}")

    genimg_offset = android_offset + page_size
    kernel_end = genimg_offset + kernel_size
    if genimg_offset + 64 > kernel_end or kernel_end > len(data):
        raise ValueError("Android kernel region extends past the boot image")

    magic, header_size = struct.unpack_from("<II", data, genimg_offset)
    if magic != 0x482A4D49:
        raise ValueError("Android kernel region does not contain a genimg header")
    chunk_count = struct.unpack_from("<I", data, genimg_offset + 60)[0]
    if header_size < 64 + chunk_count * 32 or header_size > kernel_size:
        raise ValueError("Invalid genimg header size or chunk table")

    matches = []
    for index in range(chunk_count):
        chunk_id, chunk_offset, chunk_size = struct.unpack_from(
            "<4sII", data, genimg_offset + 64 + index * 32
        )
        if chunk_id == b"LNXB":
            if (
                chunk_size == 0
                or chunk_offset < header_size
                or chunk_offset + chunk_size > kernel_size
            ):
                raise ValueError("LNXB payload lies outside the genimg kernel region")
            matches.append((genimg_offset + chunk_offset, chunk_size))

    if len(matches) != 1:
        raise ValueError(f"Expected one LNXB chunk, found {len(matches)}")

    lz4 = shutil.which("lz4")
    if lz4 is None:
        raise RuntimeError("lz4 is required to decompress the kernel payload")

    payload_offset, payload_size = matches[0]
    title(f"Extracting LNXB kernel payload ({payload_size} bytes)...")
    output_kernel.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=output_kernel.parent) as directory:
        temporary_directory = Path(directory)
        compressed_kernel = temporary_directory / "kernel.lz4"
        extracted_kernel = temporary_directory / "linux"
        compressed_kernel.write_bytes(data[payload_offset : payload_offset + payload_size])
        subprocess.run(
            [lz4, "-d", "-f", str(compressed_kernel), str(extracted_kernel)],
            check=True,
        )
        if not extracted_kernel.is_file() or not extracted_kernel.stat().st_size:
            raise RuntimeError("LZ4 decompression produced an empty kernel image")
        extracted_kernel.replace(output_kernel)
    print(f"Kernel image written to {output_kernel}")


def _kernel_release(artifacts=KERNEL_ARTIFACTS,
                    member=KERNEL_RELEASE_MEMBER):
    """Read the kernel release string (e.g. 6.12.62) recorded by build_kos.sh."""
    with tarfile.open(artifacts, "r:gz") as tar:
        try:
            release = tar.extractfile(member).read().decode("utf-8").strip()
        except KeyError:
            raise RuntimeError(
                f"'{member}' not found in {artifacts}. "
                "Run build_kos.sh first.")
    if not release:
        raise RuntimeError(f"Kernel release is empty in {artifacts}")
    return release


def _build_initramfs_overlay(build_dir, artifacts=KERNEL_ARTIFACTS):
    """Add the built virtio .ko modules (net + block) to the initramfs tree.

    The artifact tarball contains the modules with kernel-source-relative
    paths (e.g. drivers/net/virtio_net.ko, drivers/virtio/virtio_mmio.ko),
    which mirror the layout under lib/modules/<release>/kernel/, so they are
    extracted there directly.
    """

    os.makedirs(build_dir, exist_ok=True)

    artifacts = Path(artifacts)
    if not artifacts.is_file():
        raise RuntimeError(
            f"Kernel artifacts package not found at {artifacts}. "
            "Run build_kos.sh first.")

    os.makedirs(Path(build_dir) / "init.d", exist_ok=True)

    scripts_dir = os.path.dirname(__file__) + "/scripts/"

    for file in os.listdir(scripts_dir):
        shutil.copyfile(os.path.join(scripts_dir, file),
                        os.path.join(build_dir, "init.d", file))

    release = _kernel_release()
    mod_dir = Path(build_dir) / "lib" / "modules" / release / "kernel"

    extracted = []
    with tarfile.open(artifacts, "r:gz") as tar:
        for member in tar:
            if not member.isfile() or not member.name.endswith(".ko"):
                continue
            # Refuse to let tar member paths escape the initramfs tree.
            target = (mod_dir / member.name).resolve()
            if not target.is_relative_to(mod_dir.resolve()):
                raise RuntimeError(f"Refusing to extract unsafe path: {member.name}")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(tar.extractfile(member).read())
            extracted.append(target)

    if not extracted:
        raise RuntimeError(f"No .ko modules found in {artifacts}")

    for path in extracted:
        print(f"Added kernel module to initramfs: {path.relative_to(build_dir)}")


def _make_cpio_newc(root_dir, output_path):
    """Create a newc-format cpio archive from root_dir, gzipped to output_path."""
    cpio = shutil.which("cpio")
    if cpio is None:
        raise RuntimeError("cpio is required to package the initramfs")    

    output_path = Path(output_path)
    with tempfile.NamedTemporaryFile(suffix=".cpio", delete=False) as tmp_cpio:
        tmp_cpio_path = tmp_cpio.name
    try:
        # find . | cpio -o -H newc > tmp_cpio
        find = subprocess.Popen(
            ["find", "."], cwd=root_dir, stdout=subprocess.PIPE
        )
        with open(tmp_cpio_path, "wb") as cpio_out:
            # --owner=0:0 is required: the kernel's populate_initrd() strips
            # the execute bit from any initramfs file not owned by root, which
            # would make init fail with EACCES (error -13).
            subprocess.run(
                [cpio, "-o", "-H", "newc", "--reproducible", "--owner=0:0"],
                stdin=find.stdout, stdout=cpio_out, check=True,
                cwd=root_dir
            )
        find.wait()

        # gzip the cpio archive
        with open(tmp_cpio_path, "rb") as src, gzip.open(output_path, "wb") as gz:
            shutil.copyfileobj(src, gz)
    finally:
        os.unlink(tmp_cpio_path)

    print(f"Initramfs written to {output_path} ({output_path.stat().st_size} bytes)")


def extract_initramfs(rootfs_image=None, output_initramfs=None):
    """Extract a minimal shell initramfs from the rootfs image."""
    rootfs_image = Path(rootfs_image or EMMC_DIR / "rootfs.subimg")
    output_initramfs = Path(output_initramfs or OUTPUT_INITRAMFS)

    if output_initramfs.is_file() and output_initramfs.stat().st_size:
        # Rebuild if the kernel artifact modules are newer than the existing
        # initramfs, so re-running build_kos.sh picks the modules up.
        if not (
            KERNEL_ARTIFACTS.is_file()
            and KERNEL_ARTIFACTS.stat().st_mtime > output_initramfs.stat().st_mtime
        ):
            print("Initramfs already present")
            return
        print("Kernel artifacts newer than initramfs; rebuilding to include modules")

    debugfs = shutil.which("debugfs")
    if debugfs is None:
        raise RuntimeError("debugfs is required to extract files from the ext4 rootfs")

    title("Extracting minimal initramfs from rootfs...")
    with tempfile.TemporaryDirectory() as tmp:
        tmp_dir = Path(tmp)
                        
        build_dir = tmp_dir / "initramfs"
        _build_initramfs_overlay(str(build_dir))

        # Package as cpio.gz
        _make_cpio_newc(str(build_dir), str(output_initramfs))


def main():
    download_image()
    extract_image()
    decompress_partition("rootfs")
    decompress_partition("boot")
    desparsify_boot_partition()
    extract_kernel_image()
    extract_initramfs()


if __name__ == "__main__":
    main()