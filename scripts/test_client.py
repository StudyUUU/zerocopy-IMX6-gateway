#!/usr/bin/env python3
"""
简单的TCP客户端测试程序
用于连接到 zerocopy_gateway 并接收数据
"""

import socket
import struct
import sys
import time

# TCP协议头结构
# struct tcp_data_header {
#     uint32_t magic;          /* 0x53444D41 "SDMA" */
#     uint32_t seq_num;
#     uint32_t data_size;
#     uint64_t timestamp;
#     uint32_t checksum;
# }
HEADER_FORMAT = '=IIIQI'  # Little-endian, 4+4+4+8+4 = 24 bytes
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)
MAGIC = 0x53444D41

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <host> [port]")
        sys.exit(1)
    
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8888
    
    print(f"连接到 {host}:{port}...")
    
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print("连接成功！等待数据...\n")
        
        packet_count = 0
        total_bytes = 0
        start_time = time.time()
        
        while True:
            # 接收头部
            header_data = recv_exact(sock, HEADER_SIZE)
            if not header_data:
                print("连接关闭")
                break
            
            # 解析头部
            magic, seq_num, data_size, timestamp, checksum = struct.unpack(
                HEADER_FORMAT, header_data
            )
            
            # 验证魔数
            if magic != MAGIC:
                print(f"错误: 无效的魔数 0x{magic:08X}")
                break
            
            # 接收数据
            data = recv_exact(sock, data_size)
            if not data:
                print("数据接收不完整")
                break
            
            # 验证校验和
            calc_checksum = sum(data) & 0xFFFFFFFF
            if calc_checksum != checksum:
                print(f"警告: 校验和不匹配 (期望 {checksum}, 实际 {calc_checksum})")
            
            packet_count += 1
            total_bytes += data_size
            
            # 打印统计信息
            elapsed = time.time() - start_time
            rate = total_bytes / elapsed / 1024 / 1024 if elapsed > 0 else 0
            
            print(f"[#{packet_count}] seq={seq_num}, size={data_size}, "
                  f"checksum=0x{checksum:08X}, "
                  f"rate={rate:.2f} MB/s")
            
            # 每10个包打印一次详细信息
            if packet_count % 10 == 0:
                avg_size = total_bytes / packet_count
                print(f"\n=== 统计 ===")
                print(f"  总包数: {packet_count}")
                print(f"  总字节: {total_bytes}")
                print(f"  平均包大小: {avg_size:.0f} bytes")
                print(f"  平均速率: {rate:.2f} MB/s")
                print(f"  运行时间: {elapsed:.1f}s\n")
    
    except KeyboardInterrupt:
        print("\n\n用户中断")
    except Exception as e:
        print(f"错误: {e}")
    finally:
        sock.close()
        print("连接已关闭")

def recv_exact(sock, size):
    """接收精确的字节数"""
    data = b''
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            return None
        data += chunk
    return data

if __name__ == '__main__':
    main()
