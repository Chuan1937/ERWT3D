#!/bin/bash

# Drop filesystem cache
# Requires root privileges

if [ "$(id -u)" -ne 0 ]; then
    echo "This script requires root privileges. Run with sudo."
    exit 1
fi

echo "Dropping filesystem cache..."

# Sync filesystem
sync

# Drop caches
echo 3 > /proc/sys/vm/drop_caches

echo "Cache dropped successfully."