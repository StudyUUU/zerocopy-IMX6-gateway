#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstdio> // 添加此头文件以支持 perror
#include <iomanip> // 用于格式化输出

int main() {
    int fd = open("/dev/alientek_icm20608", O_RDWR); // 注意确认你的设备名
    if (fd < 0) {
        std::cerr << "Failed to open device!" << std::endl;
        return -1;
    }

    int *sensor_data = (int *)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sensor_data == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return -1;
    }

    // 先清屏一次
    std::cout << "\033[2J\033[H" << "--- Zero-Copy Real-time Gateway ---" << std::endl;

    while (true) {
        // 1. 从映射内存获取原始值
        // 数据布局: [0]gyro_x, [1]gyro_y, [2]gyro_z, [3]accel_x, [4]accel_y, [5]accel_z, [6]temp
        int gyro_x_raw = sensor_data[0];
        int gyro_y_raw = sensor_data[1];
        int gyro_z_raw = sensor_data[2];
        int accel_x_raw = sensor_data[3];
        int accel_y_raw = sensor_data[4];
        int accel_z_raw = sensor_data[5];
        int temp_raw = sensor_data[6];

        // 2. 换算为物理单位
        // 加速度: ±16g 量程，灵敏度 2048 counts/g
        float ax = (float)accel_x_raw / 2048.0f;
        float ay = (float)accel_y_raw / 2048.0f;
        float az = (float)accel_z_raw / 2048.0f;

        // 角速度: ±2000 °/s 量程，灵敏度 16.4 counts/°/s
        float gx = (float)gyro_x_raw / 16.4f;
        float gy = (float)gyro_y_raw / 16.4f;
        float gz = (float)gyro_z_raw / 16.4f;

        // 温度: T(°C) = Temp_adc / 326.8 + 25
        float temp_c = (float)temp_raw / 326.8f + 25.0f;

        // 3. 打印输出
        // \r 回到行首，\033[K 清除从光标到行尾的内容
        std::cout << "\r\033[K" 
                  << "Accel(g) - X: " << std::fixed << std::setprecision(3) << std::setw(7) << ax 
                  << " | Y: " << std::setw(7) << ay 
                  << " | Z: " << std::setw(7) << az 
                  << "  |  Gyro(°/s) - X: " << std::setw(7) << gx
                  << " | Y: " << std::setw(7) << gy
                  << " | Z: " << std::setw(7) << gz
                  << "  |  Temp(°C): " << std::setw(6) << temp_c
                  << std::flush;

        usleep(40000); // 25Hz 刷新频率即可，不用太快
    }

    munmap(sensor_data, 4096);
    close(fd);
    return 0;
}