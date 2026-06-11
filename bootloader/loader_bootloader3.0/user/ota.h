/**
 * @file    ota.h
 * @brief   OTA 空中升级模块 —— 头文件
 *
 *  功能概览:
 *    - 双缓冲 DMA 接收串口固件数据
 *    - 写入外部 Flash (W25Qxx) 4 分区轮换存储
 *    - CRC32 校验 + 版本降级保护
 *    - 按键选择跳转主 APP / 升级分区
 *
 *  EEPROM 布局 (与结构体 pack(1) 强相关，勿随意修改地址):
 *    0x10  updata_val          11 字节  分区标志 + 固件长度
 *    0x1C  device_information   3 字节  设备型号 + 版本
 *    0x20  crc_data             8 字节  新旧固件 CRC32
 *    0x30  rollback_info_t     36 字节  4 分区版本/CRC/长度
 *
 *  Flash 分区布局:
 *    PARTITION_0: 0x200000 ~ 0x27FFFF  (512KB)
 *    PARTITION_1: 0x080000 ~ 0x0FFFFF  (512KB)
 *    PARTITION_2: 0x100000 ~ 0x17FFFF  (512KB)
 *    PARTITION_3: 0x180000 ~ 0x1FFFFF  (512KB)
 *
 *  内部 Flash 布局:
 *    0x8000000 ~ 0x8007FFF  升级/Boot 区域 (32KB)
 *    0x8008000 ~ 0x806BFFF  主 APP 区域     (400KB)
 *    0x806C000 ~ 0x8073FFF  下载/备用区域   (32KB)
 */

#ifndef __OTA_H_
#define __OTA_H_

/*===========================================================================
 *                              头文件依赖
 *===========================================================================*/
#include "eeprom.h"
#include "ext_flash.h"
#include "dma.h"
#include "crc.h"
#include "esp8266.h"
#include <stdlib.h>
#include <string.h>
#include <usart.h>
/*===========================================================================
 *                          EEPROM 数据结构体
 *
 *  所有结构体使用 pack(1) 对齐，确保字段布局与 EEPROM 物理地址一一对应。
 *  修改任何一个字段都必须同步检查上方布局表，防止地址错位。
 *===========================================================================*/

/**
 * @brief  OTA 升级标志 + 固件长度信息 (存储于 EEPROM 0x10)
 */
#pragma pack(push, 1) /* 保存当前对齐方式，设置 1 字节对齐 */
typedef struct
{
    uint8_t BOOT_UPDATE_FLAG;       /* 启动升级标志，见 BOOT_xxx 宏 */
    uint8_t NEW_APP_SAVE_ADDR_FLAG; /* 下次写入的分区号 (0~3)       */
    uint8_t OLD_APP_SAVE_ADDR_FLAG; /* 上一次写入的分区号 (0~3)     */
    uint32_t NEW_DATA_LEN;          /* 新固件长度 (字节)            */
    uint32_t OLD_DATA_LEN;          /* 旧固件长度 (字节)            */
} updata_val;                       /* 总计 11 字节                 */
#pragma pack(pop)                   /* 恢复之前的对齐方式           */

/**
 * @brief  设备基本信息 (存储于 EEPROM 0x1C)
 */
#pragma pack(push, 1)
typedef struct
{
    uint8_t model;       /* 设备型号 */
    uint8_t new_version; /* 当前版本 */
    uint8_t old_version; /* 上一版本 */
} device_information;    /* 总计 3 字节 */
#pragma pack(pop)

/**
 * @brief  新旧固件 CRC32 校验值 (存储于 EEPROM 0x20)
 */
#pragma pack(push, 1)
typedef struct
{
    uint32_t OLD_APP_DATA_CRC; /* 旧固件 CRC32，用于回滚 */
    uint32_t NEW_APP_DATA_CRC; /* 新固件 CRC32，用于校验 */
} crc_data;                    /* 总计 8 字节             */
#pragma pack(pop)

/**
 * @brief  单个分区的回滚信息
 */
#pragma pack(push, 1)
typedef struct
{
    uint8_t partition_version; /* 分区固件版本号       */
    uint32_t partition_crc;    /* 分区固件 CRC32       */
    uint32_t DATA_LEN;         /* 分区固件长度 (字节)  */
} partition_info_t;            /* 总计 9 字节          */

/**
 * @brief  4 分区回滚信息表 (存储于 EEPROM 0x30)
 */
typedef struct
{
    partition_info_t part[4]; /* 4 个分区，总计 36 字节 */
} rollback_info_t;
#pragma pack(pop)

/*===========================================================================
 *                          DMA 双缓冲配置
 *===========================================================================*/

/** DMA 接收缓冲区大小 (字节)，需根据实际固件大小和 RAM 余量调整 */
#define OTA_BUF_SIZE 15000

/** 双缓冲状态枚举 */
typedef enum
{
    BUF_EMPTY = 0, /* 缓冲区空闲，可被 DMA 写入 */
    BUF_READY      /* 缓冲区数据就绪，等待写入 Flash */
} buf_state_t;

/* --- DMA 双缓冲区 (在 ota.c 中定义) --- */
extern uint8_t ota_buf0[OTA_BUF_SIZE];
extern uint8_t ota_buf1[OTA_BUF_SIZE];
extern volatile uint16_t ota_buf0_len; /* 缓冲区 0 当前数据长度 */
extern volatile uint16_t ota_buf1_len; /* 缓冲区 1 当前数据长度 */

/* 缓冲区状态标志 —— 必须 volatile：中断回调写，主循环读 */
extern volatile buf_state_t ota_buf0_sta;
extern volatile buf_state_t ota_buf1_sta;

/*===========================================================================
 *                          内部 Flash 地址映射
 *===========================================================================*/

/** 栈顶地址校验基址 (STM32F4 SRAM 起始地址) */
#define STACK_ADDR 0x20000000

/** 主 APP 分区 */
#define APP_START_ADDR 0x8008000
#define APP_END_ADDR 0x806C000

/** 下载/备用分区 */
#define DOWNLOAD_START_ADDR 0x806C000
#define DOWNLOAD_END_ADDR 0x8074000

/** 升级分区 (Bootloader 所在区域) */
#define UPDATA_START_ADDR 0x8000000
#define UPDATA_END_ADDR 0x8008000

/*===========================================================================
 *                          外部 Flash 分区地址
 *===========================================================================*/

/** 每个分区大小 512KB */
#define PARTITION_SIZE 0x80000

/** 4 个固件存储分区的起始地址 */
#define PARTITION_0_ADDR 0x200000
#define PARTITION_1_ADDR 0x080000
#define PARTITION_2_ADDR 0x100000
#define PARTITION_3_ADDR 0x180000

/*===========================================================================
 *                          EEPROM 存储地址偏移
 *
 *  与上方结构体配合使用，修改地址时必须同步更新布局表
 *===========================================================================*/

#define CHECK_UPDATE_ADDR 0x10       /* updata_val           */
#define DEVICE_INFORMATION_ADDR 0x1C /* device_information   */
#define CRC_VALUE_ADDR 0x20          /* crc_data             */
#define ROLLBACK_INFO_ADDR 0x30      /* rollback_info_t      */

/*===========================================================================
 *                          启动标志常量
 *  用于 BOOT_UPDATE_FLAG 字段，指示 Bootloader 当前应执行的动作
 *===========================================================================*/

#define BOOT_CHECK_UPDATA 0  /* 检查是否有待升级固件     */
#define BOOT_UPDATA 1        /* 执行升级：跳转到新分区   */
#define BOOT_NO_UPDATA 2     /* 无升级，直接跳主 APP     */
#define BOOT_START_UPDATA 3  /* 标记正在升级中           */
#define BOOT_RESET 4         /* 系统复位                 */
#define BOOT_UPDATA_OK 5     /* 升级完成确认             */
#define BOOT_FACTORY_RESET 6 /* 恢复出厂设置             */

/*===========================================================================
 *                          OTA 主状态机状态定义
 *  download_status 的取值，驱动 download_app_run() 流转
 *===========================================================================*/

#define CONNECT_WIFI 0        /* 正在连接 WiFi                */
#define CONNECT_TCP 1         /* 正在连接 TCP 服务器          */
#define CONNECT_TCP_FAIL 2    /* TCP 连接失败，等待用户操作   */
#define CONNECT_TCP_SUCCESS 3 /* TCP 连接成功，进入握手       */
#define DOWNLOAD_START 4      /* 开始接收固件数据             */
#define CHECK_DATA 5          /* CRC 校验中                   */
#define CHECK_DATA_OK 6       /* CRC 校验通过，等待跳转选择   */
#define CHECK_DATA_FAIL 7     /* CRC 校验失败                 */
#define DOWNLOAD_ERROR 8      /* 下载异常，跳转主 APP         */

/*===========================================================================
 *                          固件大小边界校验
 *
 *  [可选] 可用于握手阶段校验服务器下发的固件长度是否合理。
 *  当前 ota.c 中未使用，如不需要可删除。
 *===========================================================================*/

#define APP_SIZE_MIN 500     /* 最小合法固件大小 (字节) */
#define APP_SIZE_MAX 0x78000 /* 最大合法固件大小 (≈480KB) */

/*===========================================================================
 *                          全局运行时变量 (extern)
 *===========================================================================*/

/** 串口累计接收总长度 (DMA 中断中累加，主循环读取) */
extern volatile uint32_t uart_rec_full_len;

/** 最近一次 DMA 接收长度 */
extern uint16_t uart_rec_len;

/** 外部 Flash 当前写入地址 */
extern volatile uint32_t w25q_write_addr;

/** ESP8266 数据接收中断标志 */
extern uint8_t receive_data_flag;

/** 按键扫描超时计时 */
extern uint32_t scan_tick;

/*===========================================================================
 *                          对外接口函数声明
 *===========================================================================*/

/**
 * @brief  OTA 主状态机调度函数，在主循环中反复调用
 *         根据 download_status 自动流转: WiFi连接 → TCP连接 → 握手
 *         → 下载写Flash → CRC校验 → 跳转
 */
void download_app_run(void);

/**
 * @brief  与服务器握手: 上报设备信息，获取固件长度/版本/CRC
 */
void updata_ack(void);

/**
 * @brief  CRC 校验 + 版本降级保护 + 更新 EEPROM 元信息
 */
void check_data(void);

/**
 * @brief  根据分区标志选择 Flash 地址，擦除目标分区
 */
void receive_updata_init(void);

/**
 * @brief  初始化 DMA 双缓冲，开启串口空闲中断接收
 */
void usart2_receive_init(void);

/**
 * @brief  读取外部 Flash 固件，计算硬件 CRC32
 * @retval CRC32 校验值
 */
uint32_t OTA_CalcBinCRC(void);

/**
 * @brief  升级完成后统一写入 EEPROM 所有元信息
 *         (分区标志 + CRC + 版本 + 4分区回滚表)
 */
void app_bootloader_write_update(void);

/**
 * @brief  更新 4 分区回滚信息表 (被 app_bootloader_write_update 内部调用)
 */
void partition_info_update(void);

/**
 * @brief  跳转到主 APP 分区执行
 */
void jump_to_app_area(void);

/**
 * @brief  跳转到升级分区执行
 */
void jump_to_updata_area(void);

/**
 * @brief  跳转到下载/备用分区执行
 */
void jump_to_download_area(void);

void print_flash_data(void);

void ota_eeprom_init(void);

void ota_eeprom_read_and_print(void);
#endif /* __OTA_H_ */
