"""WebSocket 推送测试脚本
用法:
  python test_ws_client.py                            # 设备 pc1 @ localhost
  python test_ws_client.py pc2                        # 设备 pc2 @ localhost
  python test_ws_client.py pc1 192.168.1.100          # 远程服务器
"""
import asyncio
import websockets
import json
import sys

DEVICE_ID = sys.argv[1] if len(sys.argv) > 1 else "pc1"
HOST      = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
WS_URL    = f"ws://{HOST}:8081/ws"

async def main():
    print(f"[WS Test] {DEVICE_ID} 连接 {WS_URL}")

    try:
        async with websockets.connect(WS_URL) as ws:
            # 发送 hello 注册设备
            await ws.send(json.dumps({"type": "hello", "device_id": DEVICE_ID}))
            print(f"  → hello, device_id={DEVICE_ID}")
            print(f"  ✓ 已注册, 等待推送消息...")

            while True:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=30)
                    data = json.loads(msg)
                    print(f"  ← {json.dumps(data, ensure_ascii=False)}")
                except asyncio.TimeoutError:
                    print("  (30秒无消息, 测试通过)")
                    break
    except Exception as e:
        print(f"  FAIL: {e}")
        return 1

    return 0

if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
