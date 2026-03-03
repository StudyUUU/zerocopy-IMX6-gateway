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
        // 1. 获取原始值 (从映射内存直接读取)
        int raw_x = sensor_data[3];
        int raw_y = sensor_data[4];
        int raw_z = sensor_data[5];

        // 2. 换算为物理单位 (g)
        float ax = (float)raw_x / 2048.0f;
        float ay = (float)raw_y / 2048.0f;
        float az = (float)raw_z / 2048.0f;

        // 3. 打印输出
        // \r 回到行首，\033[K 清除从光标到行尾的内容
        std::cout << "\r\033[K" 
                  << "Accel(g) - X: " << std::fixed << std::setprecision(3) << std::setw(6) << ax 
                  << " | Y: " << std::setw(6) << ay 
                  << " | Z: " << std::setw(6) << az 
                  << " [Raw Z: " << raw_z << "]" << std::flush;

        usleep(40000); // 25Hz 刷新频率即可，不用太快
    }

    munmap(sensor_data, 4096);
    close(fd);
    return 0;
}