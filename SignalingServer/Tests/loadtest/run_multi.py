#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
多进程压测启动器：启动 N 个 loadtest.py 子进程，绕过单进程 GIL 限制

用法:
    python run_multi.py --processes 10 --users 100 --interval 0.1 --duration 30

    → 启动 10 个子进程，每进程 100 用户，共 1000 用户
    → 子进程 1: --users 100 --start-id 1
    → 子进程 2: --users 100 --start-id 101
    → ...
"""

import subprocess
import sys
import time
import argparse
import os


def main():
    parser = argparse.ArgumentParser(description='DemandStation 多进程压测')
    parser.add_argument('--processes', type=int, default=10, help='子进程数')
    parser.add_argument('--users', type=int, default=100, help='每进程用户数')
    parser.add_argument('--interval', type=float, default=0.1, help='请求间隔(秒)')
    parser.add_argument('--duration', type=int, default=30, help='运行时长(秒)')
    parser.add_argument('--batch-size', type=int, default=50, help='每批启动用户数')
    parser.add_argument('--batch-interval', type=float, default=0.2, help='批次间隔')
    parser.add_argument('--tag', default='', help='报告标签后缀')
    parser.add_argument('--host', default='127.0.0.1', help='服务器地址')
    parser.add_argument('--port', type=int, default=5488, help='服务器端口')
    parser.add_argument('--start-id', type=int, default=1, help='起始用户编号（多机压测时错开）')
    args = parser.parse_args()

    total_users = args.processes * args.users
    script_dir = os.path.dirname(os.path.abspath(__file__))
    loadtest_path = os.path.join(script_dir, 'loadtest.py')

    print(f"{'=' * 60}")
    print(f"  DemandStation 多进程压测")
    print(f"  子进程数: {args.processes}")
    print(f"  每进程用户: {args.users}")
    print(f"  总用户数: {total_users}")
    print(f"  请求间隔: {args.interval}s")
    print(f"  运行时长: {args.duration}s")
    print(f"{'=' * 60}\n")

    processes = []
    for i in range(args.processes):
        start_id = args.start_id + i * args.users
        tag = f"{args.tag}_p{i + 1}" if args.tag else f"p{i + 1}"

        cmd = [
            sys.executable,
            loadtest_path,
            '--users', str(args.users),
            '--interval', str(args.interval),
            '--duration', str(args.duration),
            '--batch-size', str(args.batch_size),
            '--batch-interval', str(args.batch_interval),
            '--start-id', str(start_id),
            '--tag', tag,
            '--host', args.host,
            '--port', str(args.port),
        ]

        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1)
        processes.append((i, p))
        print(f"  [进程 {i + 1}/{args.processes}] 启动 PID:{p.pid} 起始用户:{start_id}")
        time.sleep(0.3)  # 错峰

    print(f"\n  共 {len(processes)} 个进程已启动，等待全部完成...\n")

    # 汇总各进程的最终报告
    total_reqs = 0
    total_qps = 0
    all_latency_lines = []

    for idx, p in processes:
        output = p.communicate()[0]
        # 提取 QPS 和总请求数
        for line in output.split('\n'):
            if '总请求数' in line:
                try:
                    total_reqs += int(line.split(':')[1].strip())
                except:
                    pass
            if 'QPS' in line:
                try:
                    total_qps += float(line.split(':')[1].strip())
                except:
                    pass

    print(f"\n{'=' * 60}")
    print(f"  汇总报告 ({args.processes} 进程, {total_users} 用户)")
    print(f"{'=' * 60}")
    print(f"  总请求数: {total_reqs}")
    print(f"  汇总 QPS: {total_qps:.1f}")
    print(f"{'=' * 60}")


if __name__ == '__main__':
    main()
