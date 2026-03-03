/*
 * icm20608_reg.h - ICM20608 Register Definitions
 * 
 * Description: Register map and configuration enums for ICM20608 sensor
 * Author: Alientek
 * Date: 2026-03-03
 */

#ifndef _ICM20608_REG_H_
#define _ICM20608_REG_H_

/* ICM20608 Device IDs */
#define ICM20608G_ID 0xAF /* ICM20608G ID value */
#define ICM20608D_ID 0xAE /* ICM20608D ID value */

/* ICM20608 Register Addresses */
#define WHO_AM_I       0x75  /* Device ID register */
#define PWR_MGMT_1     0x6B  /* Power management register 1 */
#define ACCEL_CONFIG   0x1C  /* Accelerometer range configuration */
#define GYRO_CONFIG    0x1B  /* Gyroscope range configuration */
#define SMLPRT_DIV     0x19  /* Sample rate divider */
#define DLPF_CFG       0x1A  /* Digital low-pass filter configuration */
#define A_DLPF_CFG     0x1D  /* Accelerometer DLPF configuration */
#define PWR_MGMT_2     0x6C  /* Power management register 2 */
#define GYRO_CYCLE     0x6D  /* Low power mode configuration */
#define FIFO_EN        0x23  /* FIFO enable register */

/* Accelerometer Registers */
#define ACCEL_XOUT_H   0x3B  /* X-axis accelerometer high byte */
#define ACCEL_XOUT_L   0x3C  /* X-axis accelerometer low byte */
#define ACCEL_YOUT_H   0x3D  /* Y-axis accelerometer high byte */
#define ACCEL_YOUT_L   0x3E  /* Y-axis accelerometer low byte */
#define ACCEL_ZOUT_H   0x3F  /* Z-axis accelerometer high byte */
#define ACCEL_ZOUT_L   0x40  /* Z-axis accelerometer low byte */

/* Temperature Registers */
#define TEMP_OUT_H     0x41  /* Temperature high byte */
#define TEMP_OUT_L     0x42  /* Temperature low byte */

/* Gyroscope Registers */
#define GYRO_XOUT_H    0x43  /* X-axis gyroscope high byte */
#define GYRO_XOUT_L    0x44  /* X-axis gyroscope low byte */
#define GYRO_YOUT_H    0x45  /* Y-axis gyroscope high byte */
#define GYRO_YOUT_L    0x46  /* Y-axis gyroscope low byte */
#define GYRO_ZOUT_H    0x47  /* Z-axis gyroscope high byte */
#define GYRO_ZOUT_L    0x48  /* Z-axis gyroscope low byte */

/* Gyroscope Range Configuration (FS_SEL[1:0] at bits 3-4) */
typedef enum {
    GFS_250DPS  = 0x00,  /* ±250 °/s */
    GFS_500DPS  = 0x08,  /* ±500 °/s */
    GFS_1000DPS = 0x10,  /* ±1000 °/s */
    GFS_2000DPS = 0x18   /* ±2000 °/s */
} Gscale;

/* Accelerometer Range Configuration (ACC_FS_SEL[1:0] at bits 3-4) */
typedef enum {
    AFS_2G  = 0x00,  /* ±2g */
    AFS_4G  = 0x08,  /* ±4g */
    AFS_8G  = 0x10,  /* ±8g */
    AFS_16G = 0x18   /* ±16g */
} Ascale;

#endif /* _ICM20608_REG_H_ */
