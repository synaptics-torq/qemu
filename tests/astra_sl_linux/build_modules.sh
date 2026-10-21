#!/bin/bash

# This script checks out the astra sl linux kernel and drivers and build virtio modules needed
# to talk with the host in the emulator.

set -e

BASE_DIR=$(dirname "$(realpath "${BASH_SOURCE[0]}")")
THIRDPARTY_PREBUILTS_SOC_KERNEL=${BASE_DIR}/kernel
SYNA_KERNEL_MODULE_BUILD_PKG=syna-kernel-artifacts.tgz

# Parse options
USE_GERRIT=0
PACKAGE_PATH="${THIRDPARTY_PREBUILTS_SOC_KERNEL}/${SYNA_KERNEL_MODULE_BUILD_PKG}"
while [[ $# -gt 0 ]]; do
  case $1 in
    --use-gerrit)
      USE_GERRIT=1
      shift
      ;;
    --package-path)
      PACKAGE_PATH="$2"
      shift 2
      ;;
    --help)
      echo "Usage: $0 [--use-gerrit] [--package-path <path>]"
      echo "  --use-gerrit: Use Synaptics Gerrit for source checkout instead of public GitHub repositories."
      echo "  --package-path <path>: Specify the path to save the packaged kernel artifacts (default: ${PACKAGE_PATH})."
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      exit 1
      ;;
  esac
done

# we reference commit ids so that we can ensure we don't need to change the reference
# when we switch from pulling from private gerrit to public github.
KERNEL_COMMIT=92a985854209a7cbdc54cca75f437850d1c255f5
DRIVER_COMMIT=16f46a4d781eff4f0f467e3813e6ad28481419bd

if [[ -n ${LINUX_KERNEL_SSH_KEY:-} ]]; then
  echo "Using provided SSH key for Linux kernel checkout."
  LINUX_KERNEL_SSH_KEY_FILE=$(mktemp)
  trap 'rm -f "${LINUX_KERNEL_SSH_KEY_FILE}"' EXIT
  chmod 600 "${LINUX_KERNEL_SSH_KEY_FILE}"
  printf '%s\n' "${LINUX_KERNEL_SSH_KEY}" > "${LINUX_KERNEL_SSH_KEY_FILE}"
  export GIT_SSH_COMMAND="ssh ${LINUX_KERNEL_SSH_OPTIONS} -i ${LINUX_KERNEL_SSH_KEY_FILE} -o IdentitiesOnly=yes"

  if [[ -n ${LINUX_KERNEL_SSH_HOST_KEY:-} ]]; then
    echo "Using provided SSH host key for Linux kernel checkout."
    LINUX_KERNEL_SSH_HOST_KEY_FILE=$(mktemp)
    chmod 600 "${LINUX_KERNEL_SSH_HOST_KEY_FILE}"
    printf '%s\n' "${LINUX_KERNEL_SSH_HOST_KEY}" > "${LINUX_KERNEL_SSH_HOST_KEY_FILE}"
    export GIT_SSH_COMMAND="${GIT_SSH_COMMAND} -o UserKnownHostsFile=${LINUX_KERNEL_SSH_HOST_KEY_FILE} -o StrictHostKeyChecking=yes"
  fi
fi

if [[ $USE_GERRIT -eq 1 ]]; then
  SYNA_GERRIT_USER=$(id -n -u)
  LINUX_REPO_URL="ssh://${SYNA_GERRIT_USER}@sc-debu-git.synaptics.com:29420/astra/linux/main"
  LINUX_DRIVER_REPO_URL="ssh://${SYNA_GERRIT_USER}@sc-debu-git.synaptics.com:29420/debu/common/linux-driver/synaptics"  
else
  LINUX_REPO_URL="${LINUX_REPO_URL:-https://github.com/synaptics-astra/linux_6_12-main}"
  LINUX_DRIVER_REPO_URL="${LINUX_DRIVER_REPO_URL:-https://github.com/synaptics-astra/linux_6_12-drivers-synaptics}"
fi

SYNA_KERNEL_CHECKOUT=${THIRDPARTY_PREBUILTS_SOC_KERNEL}/main

echo "Configuring to build syna-kernel (source: $([[ $USE_GERRIT -eq 1 ]] && echo 'Gerrit' || echo 'GitHub'))"

if [ -f "${PACKAGE_PATH}" ]; then
    echo "kernel artifacts already packed. To rebuild, remove ${PACKAGE_PATH}"
else
    mkdir -p ${THIRDPARTY_PREBUILTS_SOC_KERNEL}
    cd ${THIRDPARTY_PREBUILTS_SOC_KERNEL}
    
    if [ ! -d "main" ]; then
        
      git init main

      cd main
      git remote add origin "${LINUX_REPO_URL}"
      git fetch --depth=1 origin "${KERNEL_COMMIT}"
      git checkout FETCH_HEAD
      
      cd drivers

      git init synaptics

      cd synaptics
      git remote add origin "${LINUX_DRIVER_REPO_URL}"
      git fetch --depth=1 origin "${DRIVER_COMMIT}"
      git checkout FETCH_HEAD
    else

      set -e

      cd main      

      git reset --hard && git clean -xfd

      git fetch --depth=1 origin "${KERNEL_COMMIT}"
      git checkout FETCH_HEAD

      cd drivers/synaptics

      git reset --hard && git clean -xfd
      git fetch --depth=1 origin "${DRIVER_COMMIT}"
      git checkout FETCH_HEAD

    fi

    # configure kernel to build the kernel module against it and pack the required headers/Makefiles/scripts
    cd ${SYNA_KERNEL_CHECKOUT}
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- sl261x_defconfig
    # Enable virtio network support as loadable modules (the astra_sl QEMU
    # machine exposes virtio devices over the platform/MMIO bus, so the
    # virtio-mmio transport is used rather than virtio-pci). The menu must be
    # enabled first, otherwise its children are dropped during olddefconfig.
    ./scripts/config --file .config \
        --enable VIRTIO_MENU \
        --module VIRTIO_MMIO \
        --module VIRTIO_NET \
        --module VIRTIO_BLK
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
    export LOCALVERSION=""
    # Record the exact kernel release the modules are built against (with
    # LOCALVERSION applied). It is packaged inside the artifacts tarball;
    # prepare_astra_sl_image.py reads it from there to place the modules under
    # lib/modules/<release>/kernel/ in the initramfs.
    KERNEL_RELEASE_FILE="kernel_release.txt"
    KERNEL_RELEASE=$(make -s ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- kernelrelease)
    echo "${KERNEL_RELEASE}" > ${KERNEL_RELEASE_FILE}
    echo "Kernel release: ${KERNEL_RELEASE}"
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules_prepare
    # Build the virtio modules (net + block) as .ko files. A full vmlinux build
    # is NOT required: with KBUILD_MODPOST_WARN=1, modpost records the built-in
    # symbols (e.g. from the CONFIG_VIRTIO=y core) as load-time dependencies
    # instead of failing on them. This is safe here because CONFIG_MODVERSIONS
    # is unset, so no symbol CRCs are verified at load time.
    VIRTIO_MODULES="drivers/net/virtio_net.ko drivers/block/virtio_blk.ko drivers/virtio/virtio_mmio.ko"
    # virtio_net hard-depends on these (Kconfig selects them); not built into the SDK kernel
    VIRTIO_MODULES="${VIRTIO_MODULES} net/core/failover.ko drivers/net/net_failover.ko lib/dim/dimlib.ko"
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- KBUILD_MODPOST_WARN=1 ${VIRTIO_MODULES}
    # Package the built .ko modules plus the kernel release they were built
    # against
    tar -cvzf ${PACKAGE_PATH} ${VIRTIO_MODULES} ${KERNEL_RELEASE_FILE}
    rm -f ${KERNEL_RELEASE_FILE}
fi
