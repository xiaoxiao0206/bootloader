# STM32 OTA Bootloader
基于 STM32 + W25Q 外部 Flash + ESP8266 WiFi 的 OTA 固件升级系统。

## 架构概览
┌─────────────────────────────────────────────────────────┐
│                    服务器 (TCP)                          │
│         推送固件 + 元数据 (长度/版本/CRC32)               │
└──────────────────────┬──────────────────────────────────┘
│ WiFi (ESP8266)
│
┌──────────────────────▼──────────────────────────────────┐
│                   OTA 程序 (Download 区)                 │
│                                                          │
│  DMA 循环接收 ──→ 主循环轮询 NDTR ──→ 写入外部 Flash      │
│  校验通过后写入 EEPROM 元数据                              │
└──────────────────────┬──────────────────────────────────┘
│
┌──────────────▼──────────────┐
│      外部 Flash (W25Q)       │
│  ┌────────┬────────┬──────┐ │
│  │ 分区 0  │ 分区 1 │ ...  │ │    4 分区轮换存储
│  │ 512KB   │ 512KB  │      │ │
│  └────────┴────────┴──────┘ │
└──────────────┬──────────────┘
│
┌──────────────────────▼──────────────────────────────────┐
│              Bootloader 程序 (Bootloader 区)             │
│                                                          │
│  读取 EEPROM → 校验外部 Flash → 写入内部 Flash APP 区     │
│  写入后再次 CRC 校验 → 通过则跳转 APP                     │
└──────────────────────┬──────────────────────────────────┘
│
┌──────────────▼──────────────┐
│       内部 Flash (APP 区)     │
│       用户应用程序            │
└─────────────────────────────┘

## 内部 Flash 分区布局
地址范围                 区域          大小     用途
─────────────────────────────────────────────────────
0x08000000 - 0x08008000  UPDATA        32KB    Bootloader 程序
0x08008000 - 0x0806C000  APP           400KB   用户应用程序
0x0806C000 - 0x08074000  DOWNLOAD      32KB    OTA 下载程序
0x08074000 - 0x0807C000  FACTORY       32KB    工厂恢复程序

## 外部 Flash (W25Q) 分区布局
地址范围                 分区     大小     用途
─────────────────────────────────────────────────────
0x00080000 - 0x00100000  分区 1   512KB   固件存储 (轮换)
0x00100000 - 0x00180000  分区 2   512KB   固件存储 (轮换)
0x00180000 - 0x00200000  分区 3   512KB   固件存储 (轮换)
0x00200000 - 0x00280000  分区 0   512KB   固件存储 (轮换)
4 分区轮换写入，OTA 每次下载到下一个分区，Bootloader 校验后写入内部 Flash。

## EEPROM 数据存储
地址      结构体                大小     用途
─────────────────────────────────────────────────────
0x10      updata_val            9B      升级标志、分区号、固件长度
0x1C      device_information    3B      设备型号、版本号
0x20      crc_data              8B      新旧固件 CRC32
0x30      rollback_info_t       36B     4 分区回滚信息表

## OTA 流程
### 1. 下载阶段 (OTA 程序)
WiFi 连接 → TCP 连接 → 握手获取元数据 → DMA 循环接收固件 → CRC 校验
| 步骤 | 说明 |
|------|------|
| TCP 握手 | 发送设备型号和版本，获取固件长度/版本/CRC32 |
| DMA 循环接收 | DMA 永不暂停，主循环读 NDTR 寄存器追写外部 Flash |
| CRC 校验 | 从外部 Flash 回读固件，硬件 CRC32 与服务端比对 |
| 版本校验 | 防止版本回退 |
| 写入 EEPROM | 校验通过后写入升级标志和元数据 |

### 2. 升级阶段 (Bootloader)
上电 → 读 EEPROM → 按键选择 → 校验固件 → 写入内部 Flash → 跳转 APP
| 步骤 | 说明 |
|------|------|
| 外部 Flash 校验 | 长度、栈指针、复位向量、CRC32 |
| 内部 Flash 写入 | 逐页擦除，半字写入 |
| 写入后校验 | 从内部 Flash 回读计算 CRC32，与期望值比对 |
| 失败处理 | 支持重试新版本或回滚旧版本，超 3 次进工厂模式 |

### 完整状态机
OTA 程序状态机:
CONNECT_TCP → CONNECT_TCP_SUCCESS → DOWNLOAD_START → CHECK_DATA → CHECK_DATA_OK
↓                                    ↓               ↓
CONNECT_TCP_FAIL → 重试/退出       CHECK_DATA_FAIL ← CRC/版本失败

Bootloader 状态机:
BOOT_CHECK_UPDATA → BOOT_UPDATA → BOOT_START_UPDATA → BOOT_UPDATA_OK → 跳APP
↓                    ↓               ↓
BOOT_NO_UPDATA    BOOT_RESET       BOOT_UPDATA_FAIL → 重试/回滚/工厂恢复

## CRC32 计算
STM32 硬件 CRC 单元处理字节序与标准 CRC-32 不同，所有 CRC 计算处均需对每个字做字节反转 (`__REV`)：
uint32_t word = ((uint32_t *)buf)[i];
word = __REV(word);  // 0x01020304 → 0x04030201
HAL_CRC_Accumulate(&hcrc, &word, 1);
涉及函数：
OTA_CalcBinCRC() — OTA 下载后校验外部 Flash
ext_flash_calc_crc() — Bootloader 校验外部 Flash
 internal_flash_calc_crc() — Bootloader 校验内部 Flash
 
关键设计决策
DMA 循环接收
问题： 双缓冲方案在高波特率下频繁停启 DMA 导致溢出丢数据。
方案： DMA 循环模式永不暂停，主循环读 NDTR 寄存器获取 DMA 写入位置，追着写外部 Flash。
DMA 写入位置 (NDTR) ──→ get_dma_write_pos()
                            │
flash_read_pos ─────────────┤
                            │
                            ▼
                    flush_dma_to_flash()
                            │
                            ▼
                    W25Q 外部 Flash
自动处理环形缓冲区回绕，主循环处理速度需大于数据到达速率。

服务器协议
ESP8266 工作在命令模式（非透传），\r\n 可能被剥离。receive_tcp_data 使用超时判断帧结束，不依赖换行符：
第一字节: 超时 10 秒 (等服务器响应)
后续字节: 超时 200ms (两次字节间隔 > 200ms 视为帧结束)

文件结构
├── ota.c / ota.h              OTA 下载模块 (DMA 循环接收 + 外部 Flash 写入)
├── App_bootloader.c / .h      Bootloader 模块 (固件校验 + 内部 Flash 写入 + 跳转)
├── ext_flash.c / .h           W25Q 外部 Flash 驱动
├── eeprom.c / .h              EEPROM 读写驱动
├── crc.c / .h                 硬件 CRC 配置
└── esp8266.c / .h             ESP8266 WiFi + TCP 驱动

硬件要求
组件	型号/规格
MCU	（≥ 512KB Flash)（≥32KB）
外部 Flash	W25Q 系列 SPI Flash (≥ 2MB)
WiFi 模块	ESP8266
按键	GPIO_PIN_13 (确认) / GPIO_PIN_14 (跳过/重试)
EEPROM	I2C 接口
串口	USART1 (调试) / USART2 (ESP8266, DMA 循环接收)
