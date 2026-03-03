# ================================================================
# Kernel module Makefile for SPI SDMA Driver (i.MX6ULL)
# ================================================================

# ----------------------------------------------------------------
# 1. 内核路径配置
# ----------------------------------------------------------------
KERN_DIR ?= /home/alientek/linux/IMX6ULL/kernel/nxp_kernel

# 设置交叉编译链
export ARCH ?= arm
export CROSS_COMPILE ?= arm-linux-gnueabihf-

# ----------------------------------------------------------------
# 2. 模块配置
# ----------------------------------------------------------------
# Module name
MODULE_NAME := spi_sdma

# Object files - spi_sdma.ko 由以下 .o 文件组成
obj-m := $(MODULE_NAME).o
$(MODULE_NAME)-objs := spi_sdma_driver.o

# Compiler flags
ccflags-y := -Wall -Werror

# ----------------------------------------------------------------
# 3. 目录变量
# ----------------------------------------------------------------
PWD := $(shell pwd)
ROOTFS_DIR ?= /home/alientek/linux/rootfs/lib/modules/4.1.15/$(MODULE_NAME)

# ----------------------------------------------------------------
# 4. 编译目标
# ----------------------------------------------------------------

# 默认目标：编译驱动模块
all: modules

# 编译驱动模块
modules:
	$(MAKE) -C $(KERN_DIR) M=$(PWD) modules

# 清理规则
clean:
	$(MAKE) -C $(KERN_DIR) M=$(PWD) clean
	rm -rf modules.order Module.symvers

# ----------------------------------------------------------------
# 5. 安装和部署目标
# ----------------------------------------------------------------

# 安装到当前系统（一般用于开发板上）
install:
	$(MAKE) -C $(KERN_DIR) M=$(PWD) modules_install
	depmod -a

# 拷贝到 rootfs（用于交叉编译后部署）
copy:
	@echo "========================================"
	@echo "拷贝驱动模块到 rootfs"
	@echo "========================================"
	@echo "目标目录: $(ROOTFS_DIR)"
	@sudo mkdir -p $(ROOTFS_DIR)
	@if [ -f $(MODULE_NAME).ko ]; then \
		echo "拷贝 $(MODULE_NAME).ko -> $(ROOTFS_DIR)/"; \
		sudo cp $(MODULE_NAME).ko $(ROOTFS_DIR)/; \
		echo "拷贝完成！"; \
	else \
		echo "错误: $(MODULE_NAME).ko 不存在，请先编译！"; \
		exit 1; \
	fi
	@echo "========================================"

# ----------------------------------------------------------------
# 6. 帮助信息
# ----------------------------------------------------------------

help:
	@echo "================================================================"
	@echo "SPI SDMA 驱动 Makefile (i.MX6ULL)"
	@echo "================================================================"
	@echo ""
	@echo "编译目标:"
	@echo "  all     - 编译内核模块 (默认)"
	@echo "  modules - 编译内核模块"
	@echo "  clean   - 清理编译文件"
	@echo ""
	@echo "部署目标:"
	@echo "  install - 安装模块到当前系统"
	@echo "  copy    - 拷贝模块到 rootfs"
	@echo ""
	@echo "配置变量:"
	@echo "  KERN_DIR     - 内核源码路径"
	@echo "                 当前: $(KERN_DIR)"
	@echo "  ROOTFS_DIR   - rootfs 目标路径"
	@echo "                 当前: $(ROOTFS_DIR)"
	@echo "  ARCH         - 目标架构 (当前: $(ARCH))"
	@echo "  CROSS_COMPILE - 交叉编译前缀 (当前: $(CROSS_COMPILE))"
	@echo ""
	@echo "使用示例:"
	@echo "  make                          # 编译模块"
	@echo "  make clean                    # 清理"
	@echo "  make copy                     # 拷贝到 rootfs"
	@echo "  make KERN_DIR=/path/to/kernel # 指定内核路径编译"
	@echo ""
	@echo "================================================================"

.PHONY: all modules clean install copy help
