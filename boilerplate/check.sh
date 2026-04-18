#!/bin/bash
echo "=== Building user-space ==="
make engine workloads

echo ""
echo "=== Building kernel module ==="
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

echo ""
echo "=== Checking for monitor.ko ==="
if [ -f "monitor.ko" ]; then
    echo "✓ monitor.ko built successfully"
    echo ""
    echo "=== Loading module ==="
    sudo insmod monitor.ko
    echo "✓ Module loaded"
    echo ""
    echo "=== Verification ==="
    lsmod | grep monitor
    ls -la /dev/container_monitor
    dmesg | tail -3
else
    echo "✗ monitor.ko not found - build failed"
    echo "Make sure linux-headers-$(uname -r) is installed"
fi
