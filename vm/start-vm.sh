#!/bin/bash
#
# Start OpenIndiana VM with virtio-gpu for DRM driver testing.
# Serial console on stdio, SSH on localhost:2222.
#
# Usage: ./start-vm.sh          (foreground, serial console)
#        ./start-vm.sh -daemonize  (background, use SSH)
#
# SSH:   ssh -p 2222 openindiana@localhost
#

VM_DIR="$(cd "$(dirname "$0")" && pwd)"
DISK="${VM_DIR}/oi-virtgpu-test.qcow2"
CIDATA="${VM_DIR}/cidata.iso"

exec qemu-system-x86_64 \
    -enable-kvm \
    -m 8G \
    -smp 4 \
    -cpu host \
    -drive file="${DISK}",format=qcow2,if=none,id=disk0 \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=disk0,bus=ahci0.0 \
    -cdrom "${CIDATA}" \
    -vga none \
    -device virtio-gpu-pci \
    -device virtio-net-pci,netdev=net0 \
    -netdev user,id=net0,hostfwd=tcp::2222-:22 \
    -device virtio-balloon \
    -device virtio-rng-pci \
    -display none \
    -serial stdio \
    -monitor unix:/tmp/virtgpu-test-monitor.sock,server,nowait \
    "$@"
