#!/bin/bash

# 🚀 优化版MTP Daemon安装脚本

echo "🚀 Installing optimized MTP inotify daemon..."

# 编译daemon
echo "📦 Compiling daemon..."
gcc -o mtp_daemon_optimized_final mtp_daemon_optimized_final.c
if [ $? -ne 0 ]; then
    echo "❌ Compilation failed!"
    exit 1
fi

echo "✅ Compilation successful"

# 停止可能运行的旧版本
echo "🛑 Stopping any existing daemons..."
pkill -f mtp_daemon
pkill -f mtp-monitor

# 复制到系统目录
echo "📁 Installing to /usr/sbin/..."
sudo cp mtp_daemon_optimized_final /usr/sbin/mtp-monitor-optimized
sudo chmod +x /usr/sbin/mtp-monitor-optimized

# 创建启动脚本
echo "📝 Creating startup script..."
cat > /tmp/mtp-monitor-optimized << 'EOF'
#!/bin/bash
### BEGIN INIT INFO
# Provides:          mtp-monitor-optimized
# Required-Start:    $local_fs $network
# Required-Stop:     $local_fs $network
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Optimized MTP file monitor daemon
# Description:       Monitors file changes and notifies MTP daemon efficiently
### END INIT INFO

DAEMON="/usr/sbin/mtp-monitor-optimized"
PIDFILE="/var/run/mtp-monitor-optimized.pid"

case "$1" in
    start)
        echo "Starting optimized MTP monitor daemon..."
        if [ -f $PIDFILE ]; then
            echo "Daemon already running (PID: $(cat $PIDFILE))"
            exit 1
        fi
        $DAEMON &
        echo $! > $PIDFILE
        echo "Daemon started with PID: $(cat $PIDFILE)"
        ;;
    stop)
        echo "Stopping optimized MTP monitor daemon..."
        if [ -f $PIDFILE ]; then
            kill $(cat $PIDFILE)
            rm -f $PIDFILE
            echo "Daemon stopped"
        else
            echo "Daemon not running"
        fi
        ;;
    restart)
        $0 stop
        sleep 1
        $0 start
        ;;
    status)
        if [ -f $PIDFILE ]; then
            PID=$(cat $PIDFILE)
            if ps -p $PID > /dev/null 2>&1; then
                echo "Daemon is running (PID: $PID)"
            else
                echo "PID file exists but daemon not running"
                rm -f $PIDFILE
            fi
        else
            echo "Daemon not running"
        fi
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac
EOF

# 安装启动脚本
sudo mv /tmp/mtp-monitor-optimized /etc/init.d/
sudo chmod +x /etc/init.d/mtp-monitor-optimized

# 启动daemon
echo "🚀 Starting optimized daemon..."
sudo /etc/init.d/mtp-monitor-optimized start

echo ""
echo "✅ Installation completed!"
echo ""
echo "📋 Usage:"
echo "  sudo /etc/init.d/mtp-monitor-optimized start   # 启动daemon"
echo "  sudo /etc/init.d/mtp-monitor-optimized stop    # 停止daemon"
echo "  sudo /etc/init.d/mtp-monitor-optimized status  # 查看状态"
echo "  sudo /etc/init.d/mtp-monitor-optimized restart # 重启daemon"
echo ""
echo "📊 Monitor logs:"
echo "  ps aux | grep mtp-monitor-optimized  # 查看进程"
echo "  tail -f /var/log/syslog | grep mtp   # 查看系统日志"
echo ""
echo "🎯 The daemon will:"
echo "  - Monitor /mnt/extsd for file changes"
echo "  - Wait 3 seconds after activity stops"
echo "  - Send UPDATE command to MtpDaemon (like MtpTools)"
echo "  - Minimum 5 seconds between commands"