#!/bin/bash

# 定义颜色输出
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

# 获取项目根目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

echo -e "${GREEN}[1/2] 正在编译内核驱动...${NC}"
cd "$PROJECT_ROOT/driver"
make clean
make

echo -e "${GREEN}[2/2] 正在编译用户态应用...${NC}"
cd "$PROJECT_ROOT/userspace"
make clean
make

echo -e "${GREEN}编译完成！${NC}"

# 复制用户态应用
if [ -f "$PROJECT_ROOT/userspace/zerocopy_gateway" ]; then
    sudo cp "$PROJECT_ROOT/userspace/zerocopy_gateway" ~/linux/rootfs/lib/modules/4.1.15/zerocopy/
    echo -e "${GREEN}已将用户态应用复制到目标文件系统！${NC}"
else
    echo -e "${RED}错误: 用户态应用编译失败${NC}"
fi

# 复制内核模块
if [ -f "$PROJECT_ROOT/driver/spi_sdma.ko" ]; then
    sudo cp "$PROJECT_ROOT/driver/spi_sdma.ko" ~/linux/rootfs/lib/modules/4.1.15/zerocopy/
    echo -e "${GREEN}已将内核模块复制到目标文件系统！${NC}"
else
    echo -e "${RED}错误: 内核模块编译失败${NC}"
fi