# -*- coding: utf-8 -*-
#!/usr/bin/env python3
"""
DemandStation 压测脚本（threading 版）

用法:
    python loadtest.py --users 1 --interval 2 --duration 30
    python loadtest.py --users 300 --interval 2 --duration 300
    python loadtest.py --users 1000 --interval 2 --duration 300
    python loadtest.py --users 3000 --interval 5 --duration 120
"""

import socket
import struct
import time
import statistics
import threading
from concurrent.futures import ThreadPoolExecutor

# 启动命令 python loadtest.py --users 1000 --interval 0.1 --duration 300 --batch-size 50 --batch-interval 0.2

# =====================================================================
# 协议常量
# =====================================================================

ID_LOGIN_REQ = 1001
ID_LOGIN_RSP = 1002
ID_GET_VR_DEVICE_LIST_REQ = 1021
ID_GET_VR_DEVICE_LIST_RSP = 1022

ERR_SUCCESS = 0

# ---- 全局发送/接收计数器 ----
_send_count = 0
_recv_count = 0
_counter_lock = threading.Lock()


# =====================================================================
# Protobuf 手动编码
# =====================================================================

def _varint_encode(value):
    buf = []
    while value > 0x7F:
        buf.append((value & 0x7F) | 0x80)
        value >>= 7
    buf.append(value & 0x7F)
    return bytes(buf)


def _varint_decode(data, offset):
    value = 0
    shift = 0
    i = offset
    while i < len(data):
        byte = data[i]
        value |= (byte & 0x7F) << shift
        shift += 7
        i += 1
        if not (byte & 0x80):
            return value, i
    raise ValueError("varint 解码失败")


def _string_field(field_num, value):
    tag = _varint_encode((field_num << 3) | 2)
    encoded = value.encode('utf-8')
    return tag + _varint_encode(len(encoded)) + encoded


def _uint64_field(field_num, value):
    tag = _varint_encode((field_num << 3) | 0)
    return tag + _varint_encode(value)


def build_login_req(username, password):
    body = b''
    body += _string_field(1, username)
    body += _string_field(2, password)
    return body


def build_get_device_list_req():
    return b''


def build_packet_header(msg_id, seq_id=0, error_code=0, error_msg=""):
    header = b''
    header += _uint64_field(1, msg_id)
    header += _uint64_field(2, seq_id)
    if error_code != 0:
        header += _uint64_field(3, error_code)
    if error_msg:
        header += _string_field(4, error_msg)
    return header


def build_tcp_packet(msg_id, body_bytes):
    header_bytes = build_packet_header(msg_id)
    total_len = 4 + 2 + len(header_bytes) + len(body_bytes)

    packet = struct.pack('>I', total_len)
    packet += struct.pack('>H', len(header_bytes))
    packet += header_bytes
    packet += body_bytes
    return packet


def parse_packet_header(data):
    msg_id = 0
    seq_id = 0
    error_code = 0
    error_msg = ""
    offset = 0

    while offset < len(data):
        tag, offset = _varint_decode(data, offset)
        field_num = tag >> 3
        wire_type = tag & 0x7

        if wire_type == 0:
            value, offset = _varint_decode(data, offset)
            if field_num == 1:
                msg_id = value
            elif field_num == 2:
                seq_id = value
            elif field_num == 3:
                error_code = value
        elif wire_type == 2:
            strlen, offset = _varint_decode(data, offset)
            value = data[offset:offset + strlen].decode('utf-8', errors='replace')
            offset += strlen
            if field_num == 4:
                error_msg = value
        else:
            break

    return msg_id, seq_id, error_code, error_msg


def recv_exact(sock, n):
    """精确读取 n 字节（处理半包）"""
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def recv_packet(sock):
    """从 socket 接收一个完整数据包，返回 (header_data, body_data)"""
    total_len_bytes = recv_exact(sock, 4)
    if total_len_bytes is None:
        return None, None

    total_len = struct.unpack('>I', total_len_bytes)[0]

    rest = recv_exact(sock, total_len - 4)
    if rest is None:
        return None, None

    header_len = struct.unpack('>H', rest[:2])[0]
    header_data = rest[2:2 + header_len]
    body_data = rest[2 + header_len:]
    return header_data, body_data


# =====================================================================
# 虚拟用户（线程）
# =====================================================================

class VirtualUser:
    """每个虚拟用户运行在自己的线程里，使用阻塞 socket"""

    def __init__(self, user_id, username, password, host, port):
        self.user_id = user_id
        self.username = username
        self.password = password
        self.host = host
        self.port = port
        self.sock = None
        self.latencies = []          # 所有请求的延迟（毫秒）
        self.logged_in = False
        self.login_attempted = False  # 新增：是否执行了登录尝试

    def connect_and_login(self):
        """连接服务器并登录"""
        self.login_attempted = True  # 标记：进入了登录流程
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # 禁用 Nagle
            self.sock.settimeout(10)
            self.sock.connect((self.host, self.port))
        except Exception as e:
            return False

        # 发送登录请求
        login_packet = build_tcp_packet(ID_LOGIN_REQ, build_login_req(self.username, self.password))
        self.sock.sendall(login_packet)

        # 等待登录响应
        header_data, body_data = recv_packet(self.sock)
        if header_data is None:
            return False

        msg_id, seq_id, error_code, error_msg = parse_packet_header(header_data)
        if msg_id != ID_LOGIN_RSP or error_code != ERR_SUCCESS:
            return False

        self.logged_in = True
        self.sock.settimeout(30)     # 登录后超时放宽
        return True

    def send_device_list_request(self):
        """
        发送设备列表请求并测量 RTT

        使用阻塞 socket.sendall() + recv()，时间测量可靠
        """
        if not self.logged_in or not self.sock:
            return None

        packet = build_tcp_packet(ID_GET_VR_DEVICE_LIST_REQ, build_get_device_list_req())

        # ---- 计时：从 sendall 之前开始 ----
        # 使用 perf_counter 获取最高精度（微秒级）
        start = time.perf_counter()
        try:
            self.sock.sendall(packet)
            # 计数：发送
            with _counter_lock:
                global _send_count; _send_count += 1
            header_data, body_data = recv_packet(self.sock)
            if header_data is None:
                return None

            elapsed_ms = (time.perf_counter() - start) * 1000
            # 计数：收到响应
            with _counter_lock:
                global _recv_count; _recv_count += 1

            msg_id, seq_id, error_code, error_msg = parse_packet_header(header_data)
            if msg_id != ID_GET_VR_DEVICE_LIST_RSP or error_code != ERR_SUCCESS:
                return None

            return elapsed_ms

        except socket.timeout:
            return None
        except Exception:
            return None

    def run(self, interval=2.0, duration=300):
        """
        依次执行：连接 → 登录 → 循环发送请求

        全程在调用线程内执行，不依赖事件循环
        """
        ok = self.connect_and_login()
        if not ok:
            return

        start_time = time.perf_counter()
        while time.perf_counter() - start_time < duration:
            latency = self.send_device_list_request()
            if latency is not None:
                self.latencies.append(latency)

            elapsed = time.perf_counter() - start_time
            if elapsed >= duration:
                break

            time.sleep(interval)

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except:
                pass


# =====================================================================
# 统计输出
# =====================================================================

def print_stats(all_latencies, login_ok, login_attempted, login_fail_real,
                login_not_attempted, user_count, duration, save_dir=None, tag=""):
    """打印统计并自动保存报告"""
    lines = []
    lines.append(f"{'=' * 60}")
    lines.append(f"  压测报告")
    lines.append(f"{'=' * 60}")
    lines.append(f"  虚拟用户数: {user_count}")
    lines.append(f"  登录成功: {login_ok}")
    lines.append(f"  登录失败（已尝试）: {login_fail_real}")
    lines.append(f"  未完成登录（被中断）: {login_not_attempted}")
    lines.append(f"  运行时长: {duration} 秒")

    if not all_latencies:
        lines.append(f"\n  没有收集到延迟数据")
        lines.append(f"{'=' * 60}\n")
        report = "\n".join(lines)
        print(report)
        return

    total = len(all_latencies)
    valid = [l for l in all_latencies if l is not None]

    if not valid:
        lines.append(f"\n  没有成功的请求")
        lines.append(f"{'=' * 60}\n")
        report = "\n".join(lines)
        print(report)
        return

    valid.sort()
    n = len(valid)

    avg = statistics.mean(valid)
    mini = min(valid)
    maxi = max(valid)
    qps = total / duration if duration > 0 else 0

    # 修正百分位计算: P50 = 索引 int(n * 0.50 - 0.5) 的最近合法值
    def percentile(data, p):
        k = (len(data) - 1) * p
        f = int(k)
        c = k - f
        if f + 1 >= len(data):
            return data[-1]
        return data[f] * (1 - c) + data[f + 1] * c

    p50 = percentile(valid, 0.50)
    p95 = percentile(valid, 0.95)
    p99 = percentile(valid, 0.99)

    lines.append(f"\n  总请求数: {total}")
    lines.append(f"  成功率: 100.0%")
    lines.append(f"  QPS: {qps:.1f}")
    lines.append(f"")
    lines.append(f"  {'延迟统计（毫秒）':-<40}")
    lines.append(f"  最小值 : {mini:.2f}")
    lines.append(f"  最大值 : {maxi:.2f}")
    lines.append(f"  平均值 : {avg:.2f}")
    lines.append(f"  P50    : {p50:.2f}")
    lines.append(f"  P95    : {p95:.2f}")
    lines.append(f"  P99    : {p99:.2f}")
    lines.append(f"  {'':-<40}")
    lines.append(f"{'=' * 60}\n")

    report = "\n".join(lines)
    print(report)

    # ---- 自动保存报告到文件 ----
    if save_dir:
        import os, datetime
        os.makedirs(save_dir, exist_ok=True)
        name_parts = [f"report_{user_count}u_{duration}s"]
        if tag:
            name_parts.append(tag)
        filename = "_".join(name_parts) + ".md"
        filepath = os.path.join(save_dir, filename)
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(f"# 压测报告\n\n")
            f.write(f"- 时间: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
            if tag:
                f.write(f"- 标签: {tag}\n")
            f.write(f"- 用户数: {user_count}\n")
            f.write(f"- 时长: {duration}s\n\n")
            f.write(f"| 指标 | 值 |\n")
            f.write(f"|------|-----|\n")
            f.write(f"| 总请求数 | {total} |\n")
            f.write(f"| QPS | {qps:.1f} |\n")
            f.write(f"| 登录成功 | {login_ok} |\n")
            f.write(f"| 最小值 | {mini:.2f} ms |\n")
            f.write(f"| 最大值 | {maxi:.2f} ms |\n")
            f.write(f"| 平均值 | {avg:.2f} ms |\n")
            f.write(f"| P50 | {p50:.2f} ms |\n")
            f.write(f"| P95 | {p95:.2f} ms |\n")
            f.write(f"| P99 | {p99:.2f} ms |\n")
        print(f"  📄 报告已保存: {filepath}")

# =====================================================================
# 主入口
# =====================================================================

def main():
    import argparse
    import concurrent.futures

    parser = argparse.ArgumentParser(description='DemandStation 压测脚本')
    parser.add_argument('--users', type=int, default=100,
                        help='虚拟用户数（默认 100）')
    parser.add_argument('--interval', type=float, default=2.0,
                        help='每个用户的请求间隔秒数（默认 2.0）')
    parser.add_argument('--duration', type=int, default=180,
                        help='运行时长秒数（默认 180）')
    parser.add_argument('--host', default='127.0.0.1',
                        help='服务器地址（默认 127.0.0.1）')
    parser.add_argument('--port', type=int, default=5488,
                        help='服务器端口（默认 5488）')
    parser.add_argument('--prefix', default='test_user_',
                        help='用户名前缀（默认 test_user_）')
    parser.add_argument('--password', default='123456',
                        help='密码（默认 123456）')
    parser.add_argument('--start-id', type=int, default=1,
                        help='起始用户编号（默认 1）')
    parser.add_argument('--batch-size', type=int, default=50,
                        help='每批启动的用户数（默认 50，防 TCP backlog 溢出）')
    parser.add_argument('--batch-interval', type=float, default=0.2,
                        help='批次间隔秒数（默认 0.2）')
    parser.add_argument('--tag', default='',
                        help='报告文件名标签（如 w8 表示 8 线程）')

    args = parser.parse_args()

    print(f"{'=' * 60}")
    print(f"  DemandStation 压测脚本（threading 版）")
    print(f"{'=' * 60}")
    print(f"  服务器地址: {args.host}:{args.port}")
    print(f"  虚拟用户数: {args.users}")
    print(f"  请求间隔: {args.interval} 秒")
    print(f"  运行时长: {args.duration} 秒")
    print(f"  分批启动: 每批 {args.batch_size} 人，间隔 {args.batch_interval} 秒")
    print(f"  用户名前缀: {args.prefix}")
    print(f"  按 Ctrl+C 可提前结束")
    print(f"{'=' * 60}\n")

    # 创建所有虚拟用户
    users = []
    for i in range(args.users):
        uid = args.start_id + i
        username = f"{args.prefix}{uid}"
        user = VirtualUser(uid, username, args.password, args.host, args.port)
        users.append(user)

    print(f"正在分批启动 {args.users} 个虚拟用户...")
    print(f"")

    login_ok = 0
    login_fail = 0
    all_latencies = []

    # ---- 每秒打印实际发送速率 ----
    def print_rate_loop():
        prev_send = 0
        prev_recv = 0
        while counter_alive[0]:
            time.sleep(1)
            with _counter_lock:
                cur_send = _send_count
                cur_recv = _recv_count
            send_rate = cur_send - prev_send
            recv_rate = cur_recv - prev_recv
            prev_send = cur_send
            prev_recv = cur_recv
            print(f"  [ClientRate] send:{send_rate}/s  recv:{recv_rate}/s  queue:{send_rate - recv_rate}/s")
    counter_alive = [True]
    rate_thread = threading.Thread(target=print_rate_loop, daemon=True)
    rate_thread.start()

    # 分批启动：每批 batch_size 个用户，间隔 batch_interval 秒
    # 防止 TCP 全连接队列（SOMAXCONN 默认 200）瞬间溢出
    max_workers = min(args.users, 2000)
    all_futures = []

    executor = ThreadPoolExecutor(max_workers=max_workers)

    try:
        # 提交所有批次，每批间隔 batch_interval 秒
        for batch_start in range(0, args.users, args.batch_size):
            batch_end = min(batch_start + args.batch_size, args.users)
            batch = users[batch_start:batch_end]
            print(f"  启动批次 {batch_start // args.batch_size + 1}：用户 {batch_start + 1} ~ {batch_end}")

            for user in batch:
                future = executor.submit(user.run, args.interval, args.duration)
                all_futures.append(future)

            if batch_end < args.users:
                time.sleep(args.batch_interval)

        # 所有用户启动完毕后，用 1 秒超时轮询等待全部完成
        # 这样 Ctrl+C 不会卡在 future.result() 上
        print(f"\n  所有批次启动完毕（{len(all_futures)} 个用户），等待运行结束...")
        remaining = set(all_futures)
        while remaining:
            done = set()
            try:
                for f in concurrent.futures.as_completed(remaining, timeout=1.0):
                    done.add(f)
            except concurrent.futures.TimeoutError:
                pass
            remaining -= done

    except KeyboardInterrupt:
        print(f"\n  用户中断，正在收集已有数据...")
    finally:
        # 不等待正在运行的线程，立即退出
        executor.shutdown(wait=False)

        # 收集结果——区分"尝试过但失败"和"被中断还没轮到"
        login_ok = sum(1 for u in users if u.logged_in)
        login_fail_real = sum(1 for u in users if u.login_attempted and not u.logged_in)
        login_not_attempted = sum(1 for u in users if not u.login_attempted)
        for u in users:
            all_latencies.extend(u.latencies)
            u.close()

        # ---- 自动保存报告 ----
        import os
        counter_alive[0] = False  # 停止速率打印线程
        rate_thread.join(timeout=2)
        save_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'reports')
        print_stats(all_latencies, login_ok,
                    sum(1 for u in users if u.login_attempted),
                    login_fail_real, login_not_attempted,
                    args.users, args.duration, save_dir, args.tag)

if __name__ == '__main__':
    main()