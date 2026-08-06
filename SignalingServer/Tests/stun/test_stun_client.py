"""RFC 5389 STUN Binding Request 测试脚本
用法: python test_stun_client.py [host] [port]
默认: python test_stun_client.py 127.0.0.1 3478
"""
import socket
import struct
import sys

HOST = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 3478

def build_binding_request():
    """构造 20 字节 STUN Binding Request"""
    msg_type        = 0x0001                # Binding Request
    msg_length      = 0                     # 无属性
    magic_cookie    = 0x2112A442            # RFC 5389 magic
    transaction_id  = b'\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C'
    return struct.pack('!HHI12s', msg_type, msg_length, magic_cookie, transaction_id)

def decode_response(data):
    """解析 STUN Binding Response，提取 MAPPED-ADDRESS"""
    if len(data) < 20:
        return None, "响应太短"

    msg_type = struct.unpack('!H', data[0:2])[0]
    if msg_type != 0x0101:
        return None, f"非 Binding Response: 0x{msg_type:04X}"

    msg_len = struct.unpack('!H', data[2:4])[0]
    pos = 20  # 跳过 20 字节 STUN 头

    results = {}
    while pos < 20 + msg_len and pos + 4 <= len(data):
        attr_type = struct.unpack('!H', data[pos:pos+2])[0]
        attr_len  = struct.unpack('!H', data[pos+2:pos+4])[0]
        pos += 4

        if attr_type == 0x0020 and attr_len >= 8:       # XOR-MAPPED-ADDRESS
            family = data[pos + 1]
            if family == 0x01:                           # IPv4
                xport = struct.unpack('!H', data[pos+2:pos+4])[0]
                xaddr = struct.unpack('!I', data[pos+4:pos+8])[0]
                port = xport ^ 0x2112
                addr = socket.inet_ntoa(struct.pack('!I', xaddr ^ 0x2112A42F))
                results['xor_mapped'] = f"{addr}:{port}"
        elif attr_type == 0x0001 and attr_len >= 8:     # MAPPED-ADDRESS
            port = struct.unpack('!H', data[pos+2:pos+4])[0]
            addr = socket.inet_ntoa(data[pos+4:pos+8])
            results['mapped'] = f"{addr}:{port}"

        pos += attr_len

    return results, None

def main():
    print(f"[STUN Test] → {HOST}:{PORT}")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(3)

    req = build_binding_request()
    sock.sendto(req, (HOST, PORT))

    try:
        data, addr = sock.recvfrom(64)
        results, err = decode_response(data)

        if err:
            print(f"  FAIL: {err}")
            return 1

        print(f"  Response length: {len(data)} bytes")
        for k, v in results.items():
            print(f"  {k}: {v}")

        print("  PASS")
    except socket.timeout:
        print("  FAIL: timeout (STUN 服务器未响应)")
        return 1
    finally:
        sock.close()

    return 0

if __name__ == "__main__":
    sys.exit(main())
