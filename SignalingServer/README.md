# SignalingServer — 公网信令服务器

基于 C++17 + Boost.Asio 的设备发现与 NAT 穿透信令服务器。

## 功能

- **设备注册/心跳/发现**: HTTP REST API + SQLite
- **STUN 服务器**: RFC 5389 UDP :3478, 公网地址反射
- **WebSocket 推送**: 设备上线/下线实时通知
- **打洞信令**: Offer/Answer/ICE 交换，P2P 连接协调

## 技术栈

| 层 | 选型 |
|---|------|
| 构建 | CMake + vcpkg |
| 网络 | Boost.Asio (One Loop Per Thread) |
| HTTP | cpp-httplib (header-only) |
| WebSocket | Boost.Beast |
| 数据库 | SQLite3 |
| JSON | nlohmann/nlohmann (隐式) + boost::json |
| 日志 | Boost.Log + fmt |

## 编译

### Windows (VS 2022 + vcpkg)

```powershell
# 安装依赖
vcpkg install boost-json boost-log boost-thread unofficially-sqlite3 --triplet x64-windows

# 编译
cmake --preset default -B build/release
cmake --build build/release
```

### Linux (Ubuntu 22.04)

```bash
# 安装依赖
sudo apt install build-essential cmake ninja-build pkg-config
sudo apt install libboost-all-dev libfmt-dev libsqlite3-dev

# 编译
cmake --preset linux-release -B build/linux
cmake --build build/linux
```

## 运行

```bash
cd Services/signaling
../../build/release/Services/signaling/SignalingServer

# 启动后可用接口:
# HTTP: http://localhost:8080
# STUN: UDP localhost:3478
# WS:   ws://localhost:8081/ws
```

## API

| 方法 | 路径 | 说明 |
|------|------|------|
| POST | /api/register | 设备注册 |
| POST | /api/heartbeat | 心跳保活 |
| GET  | /api/devices | 在线设备列表 |
| POST | /api/offer | 转发 Offer 信令 |
| POST | /api/answer | 转发 Answer 信令 |
| POST | /api/ice | 转发 ICE 候选 |
| GET  | /api/metrics | Prometheus 风格指标 |

## WebSocket 协议

```
客户端 → 服务器: {"type":"hello","device_id":"pc1"}
服务器 → 客户端: {"type":"device_online/offline","id":"xx"}
服务器 → 客户端: {"type":"offer/answer/ice","from":"xx","to":"yy",...}
```

## 测试

```bash
# STUN
python Tests/stun/test_stun_client.py

# WebSocket
python Tests/ws/test_ws_client.py pc1
python Tests/ws/test_ws_client.py pc2 192.168.1.100  # 远程
```

## 部署 (Linux 云服务器)

```bash
# 编译 + 上传
scp build/linux/Services/signaling/SignalingServer root@1.2.3.4:/opt/signaling/
scp Services/signaling/Config/server_config.json root@1.2.3.4:/opt/signaling/Config/

# 运行
ssh root@1.2.3.4 "cd /opt/signaling && ./SignalingServer"

# Docker 可选
docker build -t signaling-server .
docker run -d -p 8080:8080 -p 8081:8081 -p 3478:3478/udp signaling-server
```

## 目录结构

```
SignalingServer/
├── CMakeLists.txt
├── CMakePresets.json
├── Libs/           # 复用 DemandStation Boost 基础设施
├── Services/signaling/  # 信令服务
│   ├── Config/
│   ├── SignalingApp.cpp
│   ├── StunServer.cpp
│   ├── WsSession.cpp
│   └── WsSessionManager.cpp
├── Tests/
│   ├── stun/test_stun_client.py
│   └── ws/test_ws_client.py
└── Notes/
```
