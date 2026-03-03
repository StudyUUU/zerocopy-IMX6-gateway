#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

/* 根据驱动 read 接口定义的返回数据结构 */
typedef struct {
    int gyro_x;
    int gyro_y;
    int gyro_z;
    int accel_x;
    int accel_y;
    int accel_z;
    int temp;
} icm20608_data_t;

static int stop = 0;

/* 信号处理函数，用于优雅退出 */
void handle_sigint(int sig) {
    stop = 1;
}

int main(int argc, char *argv[]) {
    int fd;
    int ret;
    icm20608_data_t sensor_data;
    char *filename;

    /* 检查输入参数 */
    if (argc != 2) {
        printf("Usage: %s <device_path>\n", argv[0]);
        printf("Example: %s /dev/icm206080\n", argv[0]);
        return -1;
    }

    filename = argv[1];

    /* 打开设备 */
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("Failed to open device");
        return -1;
    }

    /* 注册信号处理 */
    signal(SIGINT, handle_sigint);

    printf("Starting to read ICM20608 data... (Press Ctrl+C to stop)\n");
    printf("-----------------------------------------------------------\n");

    while (!stop) {
        /* 读取 7 个 signed int */
        ret = read(fd, &sensor_data, sizeof(sensor_data));
        if (ret < 0) {
            perror("Read failed");
            break;
        }

        /* 打印原始数据 (ADC 原始值) */
        /* 根据驱动代码：data[0-2]是陀螺仪，data[3-5]是加速度，data[6]是温度 */
        printf("\rGYR: [%6d, %6d, %6d] | ACC: [%6d, %6d, %6d] | TMP: %6d",
               sensor_data.gyro_x, sensor_data.gyro_y, sensor_data.gyro_z,
               sensor_data.accel_x, sensor_data.accel_y, sensor_data.accel_z,
               sensor_data.temp);
        
        /* 强制刷新标准输出，保证 \r 覆盖效果 */
        fflush(stdout);

        /* 延时 200ms (5Hz) */
        usleep(200000);
    }

    printf("\n-----------------------------------------------------------\n");
    printf("Exiting... \n");

    close(fd);
    return 0;
}