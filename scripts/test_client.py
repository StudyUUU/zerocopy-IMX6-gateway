# -*- coding: utf-8 -*-
import socket
import json
import matplotlib
import matplotlib.pyplot as plt
import matplotlib.animation as animation
from collections import deque
import threading
import sys
import os

# ==============================================================================
# 配置参数
# ==============================================================================
UDP_IP = "0.0.0.0"       # 监听所有本地网卡
UDP_PORT = 8888          # 必须与 gateway 广播端口一致
MAX_POINTS = 200         # X轴显示的点数
BUFFER_SIZE = 1024

# ==============================================================================
# 数据队列初始化 (使用 0 预填充，实现示波器平滑滚动效果)
# ==============================================================================
data_dict = {
    'accel_x': deque([0.0]*MAX_POINTS, maxlen=MAX_POINTS),
    'accel_y': deque([0.0]*MAX_POINTS, maxlen=MAX_POINTS),
    'accel_z': deque([0.0]*MAX_POINTS, maxlen=MAX_POINTS),
    'gyro_x': deque([0.0]*MAX_POINTS, maxlen=MAX_POINTS),
    'gyro_y': deque([0.0]*MAX_POINTS, maxlen=MAX_POINTS),
    'gyro_z': deque([0.0]*MAX_POINTS, maxlen=MAX_POINTS),
    'temp': 0.0  # 单独保存最新温度
}

running = True

# ==============================================================================
# UDP 接收线程
# ==============================================================================
def udp_receiver():
    global running
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        sock.bind((UDP_IP, UDP_PORT))
        print("[*] UDP 接收线程已启动，监听端口: %d" % UDP_PORT)
    except Exception as e:
        print("[!] 绑定端口失败: %s" % e)
        running = False
        return

    while running:
        try:
            sock.settimeout(1.0)
            data, addr = sock.recvfrom(BUFFER_SIZE)
            payload = data.decode('utf-8')
            parsed_data = json.loads(payload)
            
            accel = parsed_data.get('accel', [0.0, 0.0, 0.0])
            gyro = parsed_data.get('gyro', [0.0, 0.0, 0.0])
            temp = parsed_data.get('temp', 0.0)
            
            data_dict['accel_x'].append(accel[0])
            data_dict['accel_y'].append(accel[1])
            data_dict['accel_z'].append(accel[2])
            
            data_dict['gyro_x'].append(gyro[0])
            data_dict['gyro_y'].append(gyro[1])
            data_dict['gyro_z'].append(gyro[2])
            
            data_dict['temp'] = temp
            
        except socket.timeout:
            continue
        except json.JSONDecodeError:
            pass # 忽略解析错误
        except Exception:
            continue

    sock.close()
    print("[*] UDP 接收线程已退出")

# ==============================================================================
# 绘图更新
# ==============================================================================
def update_plot(frame, lines, text_info):
    # 生成固定的 X 轴序列
    x_data = list(range(MAX_POINTS))
    
    # 批量更新线条数据
    lines['ax'].set_data(x_data, list(data_dict['accel_x']))
    lines['ay'].set_data(x_data, list(data_dict['accel_y']))
    lines['az'].set_data(x_data, list(data_dict['accel_z']))
    
    lines['gx'].set_data(x_data, list(data_dict['gyro_x']))
    lines['gy'].set_data(x_data, list(data_dict['gyro_y']))
    lines['gz'].set_data(x_data, list(data_dict['gyro_z']))
    
    # 提取最新数值用于面板展示
    ax, ay, az = data_dict['accel_x'][-1], data_dict['accel_y'][-1], data_dict['accel_z'][-1]
    gx, gy, gz = data_dict['gyro_x'][-1], data_dict['gyro_y'][-1], data_dict['gyro_z'][-1]
    tc = data_dict['temp']
    
    # 更新底部文本面板
    info_str = (f"Real-time Data | "
                f"Accel: X={ax:5.2f}g, Y={ay:5.2f}g, Z={az:5.2f}g | "
                f"Gyro: X={gx:6.1f}°, Y={gy:6.1f}°, Z={gz:6.1f}° | "
                f"Temp: {tc:4.1f}°C")
    text_info.set_text(info_str)
    
    # 返回所有需要刷新的对象
    arts = list(lines.values())
    arts.append(text_info)
    return tuple(arts)

def main():
    global running
    
    # 检查是否有显示设备 (针对纯终端环境的容错)
    if os.environ.get('DISPLAY') is None and matplotlib.get_backend() != 'agg':
        print("[!] 错误: 检测到未开启图形显示设备 (DISPLAY 环境变量为空)。")
        print("    如果你是通过 SSH 连接，请使用 'ssh -X' 登录，或者在有屏幕的 PC 上运行此脚本。")
        return

    recv_thread = threading.Thread(target=udp_receiver)
    recv_thread.daemon = True
    recv_thread.start()

    try:
        plt.style.use('dark_background')
        # 创建画布，适当增加高度以容纳底部面板
        fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8))
        fig.canvas.manager.set_window_title('Zero-Copy Gateway Realtime Monitor')
        
        # 配置加速度计图表
        ax1.set_title("Accelerometer (g)", fontsize=14, pad=10)
        ax1.set_ylim(-3.0, 3.0) # 放宽一点量程
        ax1.set_xlim(0, MAX_POINTS)
        ax1.set_ylabel("g", fontsize=12)
        ax1.grid(True, linestyle=':', alpha=0.5)
        
        # 配置陀螺仪图表
        ax2.set_title("Gyroscope (dps)", fontsize=14, pad=10)
        ax2.set_ylim(-500, 500)
        ax2.set_xlim(0, MAX_POINTS)
        ax2.set_ylabel("degree/s", fontsize=12)
        ax2.grid(True, linestyle=':', alpha=0.5)

        # 初始化线条 (存入字典方便管理)
        lines = {
            'ax': ax1.plot([], [], color='#FF5555', linewidth=1.5, label='Accel X')[0],
            'ay': ax1.plot([], [], color='#55FF55', linewidth=1.5, label='Accel Y')[0],
            'az': ax1.plot([], [], color='#5555FF', linewidth=1.5, label='Accel Z')[0],
            'gx': ax2.plot([], [], color='#FFFF55', linewidth=1.5, label='Gyro X')[0],
            'gy': ax2.plot([], [], color='#55FFFF', linewidth=1.5, label='Gyro Y')[0],
            'gz': ax2.plot([], [], color='#FF55FF', linewidth=1.5, label='Gyro Z')[0]
        }
        
        # 图例位置
        ax1.legend(loc='upper right', ncol=3)
        ax2.legend(loc='upper right', ncol=3)

        # 调整图表边距，给底部的数值显示留出空间
        plt.subplots_adjust(bottom=0.15, top=0.9, hspace=0.3)
        
        # 在图表底部中心添加文本面板
        # [修改点]: 将 fig.text 改为 ax2.text，并使用 transform=fig.transFigure
        # 这样它既有父 Axes (解决 blit=True 报错)，又能在全局窗口的底部居中显示
        text_info = ax2.text(0.5, 0.05, "Waiting for data...", 
                             transform=fig.transFigure,
                             ha='center', va='center', fontsize=14,
                             color='#EEEEEE', bbox=dict(facecolor='#222222', edgecolor='#555555', 
                                                        boxstyle='round,pad=0.5'))

        # 动画核心
        ani = animation.FuncAnimation(fig, update_plot, fargs=(lines, text_info), 
                                      interval=30, blit=True, cache_frame_data=False)

        print("[*] 正在打开图形窗口... 若无响应请检查 X11 或 GUI 环境。")
        plt.show()
        
    except Exception as e:
        print("[!] 绘图发生异常: %s" % e)
    finally:
        running = False
        print("[*] 脚本运行结束。")

if __name__ == "__main__":
    main()