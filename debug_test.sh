#!/bin/bash

echo "=== MTP Daemon Debug Test Script ==="
echo "This script will help debug the MTP daemon issues"
echo

# 编译调试版本
echo "1. Compiling debug version..."
gcc -o mtp_daemon_debug_verbose mtp_daemon_debug_verbose.c -lpthread
if [ $? -ne 0 ]; then
    echo "ERROR: Compilation failed!"
    exit 1
fi
echo "   Compilation successful"

# 检查系统状态
echo
echo "2. Checking system status..."
echo "   Watch directory: $(ls -ld /mnt/extsd 2>/dev/null || echo 'NOT FOUND')"
echo "   MTP FIFO: $(ls -l /tmp/.mtp_fifo 2>/dev/null || echo 'NOT FOUND')"
echo "   Current user: $(whoami)"
echo "   Current directory: $(pwd)"

# 检查MTP相关进程
echo
echo "3. Checking MTP processes..."
ps aux | grep -i mtp | grep -v grep || echo "   No MTP processes found"

# 检查FIFO状态
echo
echo "4. Checking FIFO details..."
if [ -e /tmp/.mtp_fifo ]; then
    echo "   FIFO exists:"
    ls -l /tmp/.mtp_fifo
    file /tmp/.mtp_fifo
    echo "   FIFO permissions: $(stat -c '%a' /tmp/.mtp_fifo)"
else
    echo "   FIFO does not exist"
fi

# 创建测试目录结构
echo
echo "5. Creating test directory structure..."
TEST_DIR="/mnt/extsd/debug_test"
if [ -d "$TEST_DIR" ]; then
    echo "   Cleaning existing test directory..."
    rm -rf "$TEST_DIR"
fi

echo "   Creating nested test structure..."
mkdir -p "$TEST_DIR/level1/level2/level3"
echo "test file" > "$TEST_DIR/level1/level2/level3/test.txt"
echo "   Test structure created: $TEST_DIR/level1/level2/level3/test.txt"

echo
echo "6. Starting MTP daemon in debug mode..."
echo "   The daemon will print detailed logs."
echo "   After it starts, please:"
echo "   1. Wait for 'MTP DAEMON READY' message"
echo "   2. In another terminal, run: rm -rf $TEST_DIR"
echo "   3. Watch the logs carefully"
echo "   4. Check if PC side updates within 2-3 seconds"
echo "   5. Press Ctrl+C to stop the daemon"
echo
echo "=== STARTING DAEMON (Press Ctrl+C to stop) ==="
echo

# 启动守护进程
./mtp_daemon_debug_verbose --no-daemon