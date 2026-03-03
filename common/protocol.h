/*
 * protocol.h - 内核驱动与用户态应用共享的协议头文件
 *
 * 作用：定义 Netlink 通信号码、数据结构、内存映射大小等。
 * 这样无论是在 driver 还是在 userspace，修改一处即可全局生效。
 */

#ifndef _ICM20608_PROTOCOL_H_
#define _ICM20608_PROTOCOL_H_

/* 1. Netlink 协议号 (必须一致) 
 * 31 是内核保留给用户的最大自定义协议号
 */
#define NETLINK_ICM_NOTIFY 31

/* 2. 内存映射相关 */
#define ICM_DMA_BUF_SIZE 4096  /* 映射的内存大小 (1个PAGE_SIZE) */
#define ICM_DEV_PATH "/dev/alientek_icm20608" /* 设备节点路径 */

/* 3. 传感器数据结构 (与内核态严格对齐) */
struct icm_sensor_data {
    signed int gyro_x;
    signed int gyro_y;
    signed int gyro_z;
    signed int accel_x;
    signed int accel_y;
    signed int accel_z;
    signed int temp;
};

/* 4. 定义应用层发给内核的命令字 (用于注册 PID) */
#define ICM_CMD_REGISTER_PID "REG_PID"

#endif /* _ICM20608_PROTOCOL_H_ */