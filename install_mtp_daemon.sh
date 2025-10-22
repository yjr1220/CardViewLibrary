#!/bin/bash

echo "=== MTP File System Monitor Daemon Installation ==="
echo

# 编译最终版本
echo "1. Compiling MTP daemon..."
gcc -o mtp_daemon_final mtp_daemon_final.c -lpthread
if [ $? -ne 0 ]; then
    echo "ERROR: Compilation failed!"
    exit 1
fi
echo "   ✓ Compilation successful"

# 检查权限
echo
echo "2. Checking permissions..."
if [ "$(whoami)" != "root" ]; then
    echo "WARNING: Not running as root. Some operations may fail."
else
    echo "   ✓ Running as root"
fi

# 停止旧的守护进程
echo
echo "3. Stopping existing daemon..."
if [ -f /var/run/mtp-daemon.pid ]; then
    OLD_PID=$(cat /var/run/mtp-daemon.pid)
    if kill -0 $OLD_PID 2>/dev/null; then
        echo "   Stopping old daemon (PID: $OLD_PID)..."
        kill $OLD_PID
        sleep 2
    fi
    rm -f /var/run/mtp-daemon.pid
fi
echo "   ✓ Old daemon stopped"

# 安装新的守护进程
echo
echo "4. Installing new daemon..."
cp mtp_daemon_final /usr/sbin/mtp-monitor
chmod +x /usr/sbin/mtp-monitor
echo "   ✓ Daemon installed to /usr/sbin/mtp-monitor"

# 创建启动脚本
echo
echo "5. Creating startup script..."
cat > /etc/init.d/mtp-monitor << 'EOF'
#!/bin/sh

DAEMON=/usr/sbin/mtp-monitor
PIDFILE=/var/run/mtp-daemon.pid

case "$1" in
    start)
        echo "Starting MTP monitor daemon..."
        if [ -f $PIDFILE ]; then
            PID=$(cat $PIDFILE)
            if kill -0 $PID 2>/dev/null; then
                echo "Daemon already running (PID: $PID)"
                exit 1
            fi
        fi
        $DAEMON
        echo "MTP monitor daemon started"
        ;;
    stop)
        echo "Stopping MTP monitor daemon..."
        if [ -f $PIDFILE ]; then
            PID=$(cat $PIDFILE)
            if kill -0 $PID 2>/dev/null; then
                kill $PID
                rm -f $PIDFILE
                echo "MTP monitor daemon stopped"
            else
                echo "Daemon not running"
                rm -f $PIDFILE
            fi
        else
            echo "Daemon not running"
        fi
        ;;
    restart)
        $0 stop
        sleep 2
        $0 start
        ;;
    status)
        if [ -f $PIDFILE ]; then
            PID=$(cat $PIDFILE)
            if kill -0 $PID 2>/dev/null; then
                echo "MTP monitor daemon is running (PID: $PID)"
            else
                echo "MTP monitor daemon is not running (stale PID file)"
            fi
        else
            echo "MTP monitor daemon is not running"
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac

exit 0
EOF

chmod +x /etc/init.d/mtp-monitor
echo "   ✓ Startup script created"

# 启动守护进程
echo
echo "6. Starting MTP monitor daemon..."
/etc/init.d/mtp-monitor start

echo
echo "=== Installation Complete ==="
echo
echo "Usage:"
echo "  Start:   /etc/init.d/mtp-monitor start"
echo "  Stop:    /etc/init.d/mtp-monitor stop"
echo "  Restart: /etc/init.d/mtp-monitor restart"
echo "  Status:  /etc/init.d/mtp-monitor status"
echo
echo "Debug mode (foreground with logs):"
echo "  /usr/sbin/mtp-monitor --no-daemon"
echo
echo "Features:"
echo "  ✓ Monitors /mnt/extsd for file system changes"
echo "  ✓ Sends burst MTP update commands for immediate response"
echo "  ✓ Filters temporary files automatically"
echo "  ✓ 2-second quiet time for operation completion"
echo "  ✓ 200ms check interval for fast response"
echo "  ✓ Automatic recovery from errors"
echo
echo "The daemon is now running and monitoring file system changes."
echo "File operations should now be reflected on PC within 2-3 seconds!"