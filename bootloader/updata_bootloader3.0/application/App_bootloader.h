#ifndef __APP_BOOTLOADER_H
#define __APP_BOOTLOADER_H

#include "eeprom.h"
#include "ext_flash.h"
#include "crc.h"
#include <string.h>

/*===========================================================================
 *                   数据结构体 (EEPROM 存储用)
 *===========================================================================*/

/**
 * @brief  升级控制标志 (存储于 EEPROM @ CHECK_UPDATE_ADDR)
 *
 *  占 9 字节，1 字节对齐
 */
#pragma pack(1)
typedef struct
{
  uint8_t  BOOT_UPDATE_FLAG;        /* 0=无需升级 1=待升级 2=有新固件待下载 */
  uint8_t  NEW_APP_SAVE_ADDR_FLAG;  /* 新固件所在分区号 (0-3) */
  uint8_t  OLD_APP_SAVE_ADDR_FLAG;  /* 旧固件所在分区号 (0-3) */
  uint32_t NEW_DATA_LEN;            /* 新固件长度 (字节) */
  uint32_t OLD_DATA_LEN;            /* 旧固件长度 (字节) */
} updata_val;
#pragma pack()

/**
 * @brief  CRC 校验值 (存储于 EEPROM @ CRC_VALUE_ADDR)
 *
 *  占 8 字节，4 字节对齐
 */
typedef struct
{
  uint32_t OLD_APP_DATA_CRC;  /* 旧固件 CRC32 */
  uint32_t NEW_APP_DATA_CRC;  /* 新固件 CRC32 */
} crc_data;

/*===========================================================================
 *                   EEPROM 地址分配
 *===========================================================================*/

#define CHECK_UPDATE_ADDR      0x10   /* updata_val       (9 字节) */
#define DEVICE_INFORMATION_ADDR 0x1C  /* device_information (3 字节) */
#define CRC_VALUE_ADDR         0x20   /* crc_data          (8 字节) */
#define ROLLBACK_INFO_ADDR     0x30   /* rollback_info_t  (36 字节) */

/*===========================================================================
 *                   内部 Flash 分区布局
 *
 *  STM32 Flash 起始: 0x08000000
 *
 *  ┌──────────────┬────────────┬────────────┐
 *  │  区域         │  起始       │  结束       │
 *  ├──────────────┼────────────┼────────────┤
 *  │  UPDATA      │  0x08000000│  0x08008000│  32KB
 *  │  APP         │  0x08008000│  0x0806C000│  400KB
 *  │  DOWNLOAD    │  0x0806C000│  0x08074000│  32KB
 *  │  FACTORY     │  0x08074000│  0x0807C000│  32KB
 *  └──────────────┴────────────┴────────────┘
 *===========================================================================*/

#define STACK_ADDR           0x20000000  /* RAM 起始地址，用于校验栈指针合法性 */

#define UPDATA_START_ADDR    0x08000000
#define UPDATA_END_ADDR      0x08008000

#define APP_START_ADDR       0x08008000
#define APP_END_ADDR         0x0806C000

#define DOWNLOAD_START_ADDR  0x0806C000
#define DOWNLOAD_END_ADDR    0x08074000

#define FACTORY_START_ADDR   0x08074000
#define FACTORY_END_ADDR     0x0807C000

/*===========================================================================
 *                   状态机状态定义
 *===========================================================================*/

#define BOOT_CHECK_UPDATA    0  /* 启动: 读 EEPROM，检查是否有升级需求 */
#define BOOT_UPDATA          1  /* 校验外部 Flash 固件 (CRC/长度/地址) */
#define BOOT_NO_UPDATA       2  /* 无升级: 直接跳转 APP */
#define BOOT_HAVE_NEW_DATA   3  /* 有新固件未下载: 跳转下载区 */
#define BOOT_START_UPDATA    4  /* 校验通过: 将固件写入内部 Flash */
#define BOOT_RESET           5  /* 校验失败: 跳转 APP (放弃本次升级) */
#define BOOT_UPDATA_OK       6  /* 写入+校验成功: 跳转 APP */
#define BOOT_FACTORY_RESET   7  /* 多次失败: 跳转工厂区恢复 */
#define BOOT_UPDATA_FAIL     8  /* 写入失败: 触发重试/回滚处理 */

/*===========================================================================
 *                   外部 Flash 分区布局 (W25Q)
 *
 *  4 个分区轮换存储固件，每分区 512KB
 *
 *  ┌────────┬────────────┬──────────┐
 *  │  分区   │  起始地址    │  大小     │
 *  ├────────┼────────────┼──────────┤
 *  │  0      │  0x200000  │  0x80000 │
 *  │  1      │  0x080000  │  0x80000 │
 *  │  2      │  0x100000  │  0x80000 │
 *  │  3      │  0x180000  │  0x80000 │
 *  └────────┴────────────┴──────────┘
 *===========================================================================*/

#define PARTITION_0_ADDR     0x200000
#define PARTITION_0_SIZE     0x80000

#define PARTITION_1_ADDR     0x080000
#define PARTITION_1_SIZE     0x80000

#define PARTITION_2_ADDR     0x100000
#define PARTITION_2_SIZE     0x80000

#define PARTITION_3_ADDR     0x180000
#define PARTITION_3_SIZE     0x80000

#define PARTITION_SIZE       0x80000  /* 统一分区大小 512KB */

/*===========================================================================
 *                   固件合法性校验范围
 *===========================================================================*/

#define APP_SIZE_MIN         500       /* 固件最小长度 (字节)，过小视为无效 */
#define APP_SIZE_MAX         300000    /* 固件最大长度 (字节)，超过分区则拒绝 */

/*===========================================================================
 *                   函数声明
 *===========================================================================*/

void app_bootloader_check_update(void);   /* 启动检查: 读 EEPROM，按键选择 */
void app_bootloader_check_data(void);     /* 校验固件: 长度/栈指针/复位向量/CRC */
void app_bootloader_run(void);            /* 主状态机调度 */
void write_data_to_flash(void);           /* 外部 Flash → 内部 Flash 搬运写入 */
uint32_t wirte_to_flash_data_check(void); /* 校验内部 Flash APP 区 CRC32 */
void jump_to_app_area(void);              /* 跳转主 APP */
void jump_to_download_area(void);         /* 跳转下载区 */
void start_delay(void);                   /* 上电延时 5s */

#endif /* __APP_BOOTLOADER_H */
