#!/bin/bash
# ============================================================
# DemandStation Linux 一键环境配置脚本
# 适用：Ubuntu 22.04 LTS
# 用法：chmod +x setup_ubuntu.sh && ./setup_ubuntu.sh
# ============================================================
set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${GREEN}============================================================${NC}"
echo -e "${GREEN}  DemandStation Ubuntu 22.04 环境初始化${NC}"
echo -e "${GREEN}============================================================${NC}"

# ---- 1. 基础构建工具 ----
echo -e "${YELLOW}[1/8] 安装构建工具...${NC}"
sudo apt update
sudo apt install -y build-essential cmake g++ ninja-build pkg-config wget

# ---- 2. Boost 1.83 ----
echo -e "${YELLOW}[2/8] 安装 Boost 1.83（PPA）...${NC}"
if ! dpkg -l | grep -q libboost1.83-dev; then
    sudo add-apt-repository -y ppa:mhier/libboost-latest
    sudo apt update
fi
sudo apt install -y libboost1.83-all-dev

# ---- 3. hiredis / protobuf / fmt / openssl / zlib ----
echo -e "${YELLOW}[3/8] 安装 hiredis/protobuf/fmt/openssl...${NC}"
sudo apt install -y libhiredis-dev protobuf-compiler libprotobuf-dev \
    libfmt-dev libssl-dev zlib1g-dev

# ---- 4. MySQL Connector/C++ 9.2.0 ----
echo -e "${YELLOW}[4/8] 安装 MySQL Connector/C++ 9.2.0...${NC}"
if ! ldconfig -p | grep -q libmysqlcppconnx; then
    MYSQL_URL="https://dev.mysql.com/get/Downloads/Connector-C++/mysql-connector-c++-9.2.0-linux-glibc2.28-x86-64bit.tar.gz"
    cd /tmp
    wget -q "$MYSQL_URL" -O mysql-connector.tar.gz
    tar xzf mysql-connector.tar.gz
    sudo cp mysql-connector-c++-*/lib64/* /usr/local/lib/
    sudo cp -r mysql-connector-c++-*/include/* /usr/local/include/
    sudo ldconfig
    rm -rf mysql-connector*
    echo -e "${GREEN}  MySQL Connector 9.2.0 安装完成${NC}"
else
    echo -e "${GREEN}  MySQL Connector 已安装，跳过${NC}"
fi

# ---- 5. MySQL Server + Redis ----
echo -e "${YELLOW}[5/8] 安装 MySQL Server + Redis...${NC}"
sudo apt install -y mysql-server redis-server

# ---- 6. 启动服务 + 配置 Redis ----
echo -e "${YELLOW}[6/8] 启动服务并配置 Redis...${NC}"
sudo systemctl start mysql redis-server

# Redis 端口改为 6380，密码设为 123456
if ! grep -q "^port 6380" /etc/redis/redis.conf; then
    sudo sed -i 's/^port 6379/port 6380/' /etc/redis/redis.conf
fi
if ! grep -q "^requirepass 123456" /etc/redis/redis.conf; then
    sudo sed -i 's/^# requirepass.*/requirepass 123456/' /etc/redis/redis.conf
fi
sudo systemctl restart redis-server

# 验证 Redis
redis-cli -p 6380 -a 123456 ping 2>/dev/null || echo -e "${RED} Redis 验证失败，请检查${NC}"

# ---- 7. MySQL X Plugin + 建库 ----
echo -e "${YELLOW}[7/8] 启用 MySQL X Plugin 并创建数据库...${NC}"
sudo mysql -e "INSTALL PLUGIN mysqlx SONAME 'mysqlx.so';" 2>/dev/null || true
sudo mysql -e "CREATE DATABASE IF NOT EXISTS demandstation;" 2>/dev/null || true
sudo mysql -e "ALTER USER 'root'@'localhost' IDENTIFIED WITH mysql_native_password BY '123456'; FLUSH PRIVILEGES;" 2>/dev/null || true

# ---- 8. 导入数据 ----
echo -e "${YELLOW}[8/8] 导入 demandstation 数据...${NC}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
if [ -f "$SCRIPT_DIR/demandstation.sql" ]; then
    sudo mysql -u root -p123456 demandstation < "$SCRIPT_DIR/demandstation.sql" 2>/dev/null && \
        echo -e "${GREEN}  数据导入完成${NC}" || \
        echo -e "${RED}  数据导入失败（可能已存在），请手动处理${NC}"
else
    echo -e "${RED}  未找到 demandstation.sql，请从 Windows 导出后放到此目录${NC}"
fi

# ---- 完成 ----
echo ""
echo -e "${GREEN}============================================================${NC}"
echo -e "${GREEN}  环境初始化完成！${NC}"
echo -e "${GREEN}============================================================${NC}"
echo ""
echo "  已验证的服务："
echo "  - MySQL:   root / 123456 (port 3306 classic, 33060 X Protocol)"
echo "  - Redis:   127.0.0.1:6380 密码 123456"
echo "  - Boost:   1.83.0"
echo ""
echo "  下一步："
echo "    1. VS 中选 'WSL (x64-Release)' → 生成"
echo "    2. 在输出目录 Services/gateway/ 启动 ./ds_gateway"
echo "    3. Windows 端压测：python run_multi.py --host <WSL_IP>"
echo ""
