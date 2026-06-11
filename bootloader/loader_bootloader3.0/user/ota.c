/**
 * @file    ota.c
 * @brief   OTA 固件下载模块 —— DMA 循环接收 + 主循环轮询写 Flash
 *
 * 工作模式:
 *   1. DMA 始终处于循环接收状态，不会因回调而暂停，避免高波特率丢数据
 *   2. 主循环读 NDTR 寄存器获取 DMA 当前写入位置，追着写外部 Flash
 *   3. 自动处理环形缓冲区回绕
 *   4. 4 分区轮换存储，支持回滚
 *
 * 假设: 主循环处理速度 > 数据到达速率，DMA 不会追尾覆盖未读数据
 */

#include "ota.h"
#include <string.h>
#include <stdlib.h>

/*===========================================================================
 *                            配置宏
 *===========================================================================*/

#define DOWNLOAD_TIMEOUT_MS    15000U   /* 单次数据接收超时 (ms)              */
#define KEY_SELECT_TIMEOUT_MS  10000U   /* 按键选择超时 (ms)                  */
#define TCP_RETRY_TIMEOUT_MS   15000U   /* TCP 重连等待超时 (ms)              */
#define PROGRESS_INTERVAL_MS   2000U   /* 进度打印间隔 (ms)                  */
#define DMA_SETTLE_MS          50U     /* DMA 启动后等待稳定 (ms)            */
#define CRC_READ_BUF_SIZE      1024U   /* CRC 计算时 Flash 读缓冲区大小      */
#define TCP_LINE_BUF_SIZE      100U    /* TCP 单行接收缓冲区大小             */

/** DMA 循环缓冲区大小 —— 建议 ≥ OTA_BUF_SIZE 的 2 倍 */
#define DMA_CIRCULAR_SIZE      (OTA_BUF_SIZE * 2)

/*===========================================================================
 *                            全局变量
 *===========================================================================*/

volatile uint32_t uart_rec_full_len = 0;   /* 已写入 Flash 的累计字节数       */
volatile uint32_t w25q_write_addr   = 0;   /* 外部 Flash 当前写地址           */
volatile uint32_t w25q_read_addr    = 0;   /* 外部 Flash 当前读地址           */

uint32_t updata_real_len = 0;   /* 本次固件期望长度                          */
uint32_t crc_real_value  = 0;   /* 服务端下发的 CRC32                       */
uint8_t  version         = 0;   /* 本次固件版本号                            */

uint8_t  download_status  = CONNECT_TCP;
uint32_t scan_tick        = 0;
uint8_t  receive_data_flag = 0; /* 1=DMA 接收中，0=空闲                      */

/* DMA 循环缓冲区 —— DMA 永远不会停 */
static uint8_t dma_circular_buf[DMA_CIRCULAR_SIZE];

/* 主循环追踪的 Flash 已写位置 (环形读指针) */
static volatile uint32_t flash_read_pos = 0;

static uint32_t print_tick    = 0;
static uint32_t last_rec_tick = 0;

/*===========================================================================
 *                            分区地址表
 *===========================================================================*/

/** 4 分区起始地址，用于轮换写入 */
static const uint32_t partition_addrs[4] = {
    PARTITION_0_ADDR,
    PARTITION_1_ADDR,
    PARTITION_2_ADDR,
    PARTITION_3_ADDR
};

/*===========================================================================
 *                        工具函数
 *===========================================================================*/

/**
 * @brief  下载前反初始化所有相关外设，为跳转做准备
 */
static void peripherals_deinit(void)
{
    NVIC_DisableIRQ(USART1_IRQn);
    HAL_UART_DMAStop(&huart2);
    HAL_UART_DeInit(&huart2);
    HAL_DMA_Abort(&hdma_usart2_rx);
    NVIC_DisableIRQ(USART2_IRQn);
    NVIC_DisableIRQ(DMA1_Channel6_IRQn);

    /* 停 SysTick，防止跳转后意外中断 */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
}

/**
 * @brief  简易按键扫描（消抖 + 等待松开）
 * @param  GPIO_Pin  目标引脚
 * @return 1=检测到按下，0=无动作
 */
static uint8_t Key_Scan(uint16_t GPIO_Pin)
{
    if (HAL_GPIO_ReadPin(GPIOF, GPIO_Pin) != 0)
        return 0;

    HAL_Delay(20);
    if (HAL_GPIO_ReadPin(GPIOF, GPIO_Pin) != 0)
        return 0;

    /* 等待松开 */
    while (HAL_GPIO_ReadPin(GPIOF, GPIO_Pin) == 0)
        HAL_Delay(20);

    return 1;
}

/**
 * @brief  将 8 字节十六进制 ASCII 字符串转换为 uint32
 *         例如 "A1B2C3D4" → 0xA1B2C3D4
 * @param  str 指向 8 字节十六进制字符串
 * @return 转换结果，非法字符返回 0
 */
static uint32_t hex_str_to_u32(const uint8_t *str)
{
    uint32_t val = 0;
    for (uint8_t i = 0; i < 8; i++)
    {
        uint8_t c = str[i];
        val <<= 4;
        if      (c >= '0' && c <= '9') val |= (uint32_t)(c - '0');
        else if (c >= 'A' && c <= 'F') val |= (uint32_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') val |= (uint32_t)(c - 'a' + 10);
        else return 0;  /* 非法字符 */
    }
    return val;
}

/*===========================================================================
 *                            跳转功能
 *===========================================================================*/

/**
 * @brief  校验栈指针和复位向量，然后跳转到指定区域
 * @param  sp         栈指针 (从目标地址读取)
 * @param  pc         复位向量 (从目标地址 +4 读取)
 * @param  area_start 区域起始地址
 * @param  area_end   区域结束地址
 * @param  name       区域名 (用于调试打印)
 * @return 仅在校验失败时返回 -1，正常跳转不会返回
 */
static int jump_to_area(uint32_t sp, uint32_t pc,
                        uint32_t area_start, uint32_t area_end,
                        const char *name)
{
    if ((sp & 0xFFFF0000) != STACK_ADDR)
    {
        printf("%s stack addr error: 0x%08lX\r\n", name, (unsigned long)sp);
        return -1;
    }
    if (pc < area_start || pc > area_end)
    {
        printf("%s reset handler error: 0x%08lX\r\n", name, (unsigned long)pc);
        return -1;
    }

    peripherals_deinit();
    __disable_irq();
    __set_MSP(sp);
    SCB->VTOR = area_start;
    ((void (*)(void))pc)();

    /* 理论上不会执行到这里 */
    return -1;
}

void jump_to_app_area(void)
{
    printf("jump to APP\r\n");
    uint32_t sp = *(volatile uint32_t *)(APP_START_ADDR);
    uint32_t pc = *(volatile uint32_t *)(APP_START_ADDR + 4);
    jump_to_area(sp, pc, APP_START_ADDR, APP_END_ADDR, "APP");
}

void jump_to_updata_area(void)
{
    printf("jump to UPDATA\r\n");
    uint32_t sp = *(volatile uint32_t *)(UPDATA_START_ADDR);
    uint32_t pc = *(volatile uint32_t *)(UPDATA_START_ADDR + 4);
    jump_to_area(sp, pc, UPDATA_START_ADDR, UPDATA_END_ADDR, "UPDATA");
}

void jump_to_download_area(void)
{
    uint32_t sp = *(volatile uint32_t *)(DOWNLOAD_START_ADDR);
    uint32_t pc = *(volatile uint32_t *)(DOWNLOAD_START_ADDR + 4);
    jump_to_area(sp, pc, DOWNLOAD_START_ADDR, DOWNLOAD_END_ADDR, "DOWNLOAD");
}

/*===========================================================================
 *                        EEPROM 更新
 *===========================================================================*/

/**
 * @brief  更新分区标志和启动标志 (轮换写入地址 + 标记需要升级)
 */
static void flag_write_update(void)
{
    updata_val rd, wr;
    eeprom_read_bytes(CHECK_UPDATE_ADDR, (uint8_t *)&rd, sizeof(rd));

    wr.NEW_APP_SAVE_ADDR_FLAG = (rd.NEW_APP_SAVE_ADDR_FLAG + 1) & 0x03;
    wr.OLD_APP_SAVE_ADDR_FLAG = rd.NEW_APP_SAVE_ADDR_FLAG;
    wr.BOOT_UPDATE_FLAG       = BOOT_UPDATA;
    wr.OLD_DATA_LEN           = rd.NEW_DATA_LEN;
    wr.NEW_DATA_LEN           = updata_real_len;

    eeprom_write_bytes(CHECK_UPDATE_ADDR, (uint8_t *)&wr, sizeof(wr));
}

/**
 * @brief  更新 CRC 校验值
 */
static void crc_val_write_update(void)
{
    crc_data rd, wr;
    eeprom_read_bytes(CRC_VALUE_ADDR, (uint8_t *)&rd, sizeof(rd));

    wr.OLD_APP_DATA_CRC = rd.NEW_APP_DATA_CRC;
    wr.NEW_APP_DATA_CRC = crc_real_value;

    eeprom_write_bytes(CRC_VALUE_ADDR, (uint8_t *)&wr, sizeof(wr));
}

/**
 * @brief  更新设备信息 (版本号等)
 */
static void device_information_update(void)
{
    device_information rd, wr;
    eeprom_read_bytes(DEVICE_INFORMATION_ADDR, (uint8_t *)&rd, sizeof(rd));

    wr.old_version = rd.new_version;
    wr.new_version = version;
    wr.model       = rd.model;

    eeprom_write_bytes(DEVICE_INFORMATION_ADDR, (uint8_t *)&wr, sizeof(wr));
}

/**
 * @brief  更新 4 分区回滚信息表中对应分区的元数据
 */
void partition_info_update(void)
{
    rollback_info_t rollback;
    updata_val      part_flag;

    eeprom_read_bytes(ROLLBACK_INFO_ADDR,  (uint8_t *)&rollback,  sizeof(rollback));
    eeprom_read_bytes(CHECK_UPDATE_ADDR,   (uint8_t *)&part_flag, sizeof(part_flag));

    uint8_t idx = part_flag.NEW_APP_SAVE_ADDR_FLAG;
    if (idx > 3) return;

    rollback.part[idx].partition_version = version;
    rollback.part[idx].partition_crc     = crc_real_value;
    rollback.part[idx].DATA_LEN          = updata_real_len;

    eeprom_write_bytes(ROLLBACK_INFO_ADDR, (uint8_t *)&rollback, sizeof(rollback));
}

/**
 * @brief  依次写入所有 EEPROM 元数据（分区标志、CRC、版本、回滚表）
 */
void app_bootloader_write_update(void)
{
    flag_write_update();           HAL_Delay(10);
    crc_val_write_update();        HAL_Delay(10);
    device_information_update();   HAL_Delay(10);
    partition_info_update();       HAL_Delay(10);
}

/*===========================================================================
 *                  DMA 循环接收 (核心模块)
 *
 *  原方案: 双缓冲，每次回调停/启 DMA → 高波特率溢出
 *  新方案: DMA 循环模式永不暂停，主循环读 NDTR 获取写入位置
 *===========================================================================*/

/**
 * @brief  启动 DMA 循环接收
 *
 *  DMA 处于循环模式，持续写入 dma_circular_buf。
 *  主循环通过 get_dma_write_pos() 追踪写入进度。
 */
void usart2_receive_init(void)
{
    receive_data_flag = 1;
    memset(dma_circular_buf, 0, DMA_CIRCULAR_SIZE);

    uart_rec_full_len = 0;
    flash_read_pos    = 0;

    HAL_UART_Receive_DMA(&huart2, dma_circular_buf, DMA_CIRCULAR_SIZE);

    /* 关闭半传输中断 —— 主循环轮询 NDTR 即可，无需中断通知 */
    __HAL_DMA_DISABLE_IT(&hdma_usart2_rx, DMA_IT_HT);
}

/**
 * @brief  HAL Rx 事件回调 (DMA 循环模式下由 IDLE/HT/TC 触发)
 *
 *  本模块采用主循环轮询 NDTR 的策略，此处不处理数据。
 *  保留空回调以满足 HAL 框架要求。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    (void)huart;
    (void)Size;
}

/**
 * @brief  获取 DMA 当前写入位置 (字节偏移)
 *
 *  NDTR 是倒计数器: 已写字节数 = 缓冲区总大小 - 剩余计数
 *  内存屏障确保读到最新值
 */
static inline uint32_t get_dma_write_pos(void)
{
    __DSB();  /* 数据同步屏障，确保 NDTR 读取为最新值 */
    return DMA_CIRCULAR_SIZE - __HAL_DMA_GET_COUNTER(&hdma_usart2_rx);
}

/**
 * @brief  从环形缓冲区取出新数据写入外部 Flash
 *
 *  处理两种情况:
 *    - 线性: [flash_read_pos, dma_write_pos)
 *    - 回绕: 先写尾部 [flash_read_pos, 缓冲区末尾)，再写头部 [0, dma_write_pos)
 *
 *  安全假设: 主循环处理速度 > 数据到达速率，DMA 不会追尾覆盖未读数据。
 *  若出现追尾 (overrun)，数据将损坏，需通过 CRC 校验发现。
 *
 * @return 本次写入 Flash 的字节数
 */
static uint32_t flush_dma_to_flash(void)
{
    uint32_t wpos = get_dma_write_pos();
    uint32_t rpos = flash_read_pos;

    if (wpos == rpos)
        return 0;  /* 无新数据 */

    uint32_t written = 0;

    if (wpos > rpos)
    {
        /* 线性区间 [rpos, wpos)，无回绕 */
        uint32_t len = wpos - rpos;
        ext_flash_write_data(w25q_write_addr, &dma_circular_buf[rpos], len);
        w25q_write_addr += len;
        written = len;
    }
    else
    {
        /* 回绕: 先写 [rpos → 缓冲区末尾] */
        uint32_t tail_len = DMA_CIRCULAR_SIZE - rpos;
        ext_flash_write_data(w25q_write_addr, &dma_circular_buf[rpos], tail_len);
        w25q_write_addr += tail_len;
        written = tail_len;

        /* 再写 [0 → wpos] */
        if (wpos > 0)
        {
            ext_flash_write_data(w25q_write_addr, &dma_circular_buf[0], wpos);
            w25q_write_addr += wpos;
            written += wpos;
        }
    }

    flash_read_pos = wpos;
    uart_rec_full_len += written;
    return written;
}

/*===========================================================================
 *                        OTA 初始化 / Flash 操作
 *===========================================================================*/

/**
 * @brief  初始化 OTA 写入: 选择下一个分区并擦除
 *
 *  分区轮换规则: 当前标记 → 下一个分区 (0→1→2→3→0)
 */
void receive_updata_init(void)
{
    updata_val rd;
    eeprom_read_bytes(CHECK_UPDATE_ADDR, (uint8_t *)&rd, sizeof(rd));

    /* 选择下一个分区 (轮换) */
    uint8_t next = (rd.NEW_APP_SAVE_ADDR_FLAG + 1) & 0x03;
    w25q_write_addr = partition_addrs[next];
    w25q_read_addr  = partition_addrs[next];
    printf("use partition %u: 0x%08X\r\n", next, w25q_write_addr);

    /* 写入测试数据验证 Flash 可访问，然后擦除整分区 */
    ext_flash_write_data(w25q_write_addr, (uint8_t *)"hello", 5);
    HAL_Delay(50);
    ext_flash_erase_area(w25q_write_addr, PARTITION_SIZE);
    HAL_Delay(DMA_SETTLE_MS);
}

/*===========================================================================
 *                        CRC 计算 / 握手
 *===========================================================================*/

/**
 * @brief  从外部 Flash 读取已下载固件，计算 CRC32
 * @return CRC32 校验值
 */
uint32_t OTA_CalcBinCRC(void)
{
    uint32_t remain = updata_real_len;
    uint32_t addr   = w25q_read_addr;
    uint8_t  buf[CRC_READ_BUF_SIZE];

    __HAL_CRC_DR_RESET(&hcrc);

    while (remain > 0)
    {
        uint16_t chunk = (remain > CRC_READ_BUF_SIZE)
                         ? (uint16_t)CRC_READ_BUF_SIZE
                         : (uint16_t)remain;
        ext_flash_read_data(addr, buf, chunk);

        uint32_t word_cnt = chunk / 4;
        for (uint32_t i = 0; i < word_cnt; i++)
        {
            uint32_t word = ((uint32_t *)buf)[i];
            word = __REV(word);                    
            HAL_CRC_Accumulate(&hcrc, &word, 1);
        }

        uint8_t tail = chunk % 4;
        if (tail > 0)
        {
            uint32_t last_word = 0;
            memcpy(&last_word, buf + chunk - tail, tail);
            last_word = __REV(last_word);          
            HAL_CRC_Accumulate(&hcrc, &last_word, 1);
        }

        addr   += chunk;
        remain -= chunk;
    }

    return hcrc.Instance->DR;
}


/**
 * @brief  与服务端握手: 交换设备信息 → 获取固件元数据 → 启动下载
 *
 *  协议流程:
 *    → 发送设备型号和版本
 *    ← 服务端确认 "ok"
 *    → 请求 data_len
 *    ← 返回固件长度
 *    → 请求 version
 *    ← 返回固件版本
 *    → 请求 crc_value
 *    ← 返回 CRC32 (8 字节十六进制)
 *    → 发送 "start" 通知服务端开始推送
 */
void updata_ack(void)
{
	  __HAL_UART_DISABLE_IT(&huart2, UART_IT_RXNE);
	
    device_information dev_info;
    eeprom_read_bytes(DEVICE_INFORMATION_ADDR, (uint8_t *)&dev_info, sizeof(dev_info));

    /* 交换设备信息 */
    TCP_Send_Format("model:%d",   dev_info.model);
    TCP_Send_Format("version:%d", dev_info.new_version);

    uint8_t buf[TCP_LINE_BUF_SIZE];
    receive_tcp_data(buf);
    buf[TCP_LINE_BUF_SIZE - 1] = '\0';

    if (strstr((char *)buf, "ok") == NULL && strstr((char *)buf, "OK") == NULL)
    {
        printf("device information rejected\r\n");
        download_status = DOWNLOAD_ERROR;
        return;
    }
    printf("device information OK\r\n");

    /* 获取固件长度 */
    TCP_Send_Format("data_len");
    receive_tcp_data(buf);
    buf[TCP_LINE_BUF_SIZE - 1] = '\0';
    updata_real_len = (uint32_t)atoi((char *)buf);
    if (updata_real_len == 0 || updata_real_len > PARTITION_SIZE)
    {
        printf("Invalid firmware length: %lu\r\n", (unsigned long)updata_real_len);
        download_status = DOWNLOAD_ERROR;
        return;
    }
    printf("firmware length: %lu\r\n", (unsigned long)updata_real_len);

    /* 获取固件版本 */
    TCP_Send_Format("version");
    receive_tcp_data(buf);
    buf[TCP_LINE_BUF_SIZE - 1] = '\0';
    version = (uint8_t)atoi((char *)buf);
    printf("firmware version: %u\r\n", version);

    /* 获取 CRC 校验值 */
    TCP_Send_Format("crc_value");
    receive_tcp_data(buf);
    buf[TCP_LINE_BUF_SIZE - 1] = '\0';
    crc_real_value = hex_str_to_u32(buf);
    printf("firmware CRC: 0x%08lX\r\n", (unsigned long)crc_real_value);

    /* 初始化接收环境并通知服务端开始推送 */
    usart2_receive_init();
    receive_updata_init();
    HAL_Delay(20);

    TCP_Send_Format("start");
    download_status = DOWNLOAD_START;
}

/*===========================================================================
 *                  固件下载主循环 (核心模块)
 *
 *  主循环持续轮询 DMA 写入位置，将新数据从环形缓冲区搬运到外部 Flash，
 *  直到接收完全部数据或超时。
 *===========================================================================*/

void receive_data_write_to_flash(void)
{
    last_rec_tick = HAL_GetTick();
    print_tick    = HAL_GetTick();

    printf("Download started, DMA circular buffer: %u bytes\r\n",
           DMA_CIRCULAR_SIZE);

    while (1)
    {
        /* 主循环核心: 追着 DMA 写入位置，将新数据刷到 Flash */
        uint32_t written = flush_dma_to_flash();
        if (written > 0)
            last_rec_tick = HAL_GetTick();

        /* 定时打印进度 */
        if (HAL_GetTick() - print_tick >= PROGRESS_INTERVAL_MS)
        {
            print_tick = HAL_GetTick();
            printf("progress: %lu / %lu bytes\r\n",
                   (unsigned long)uart_rec_full_len,
                   (unsigned long)updata_real_len);
        }

        /* 正常结束: 收齐所有数据 */
        if (uart_rec_full_len >= updata_real_len)
        {
            flush_dma_to_flash();  /* 尾部数据再次刷新确保写入 */
            printf("\r\n===== Download complete (length matched) =====\r\n");
            break;
        }

        /* 超时: 长时间无新数据到达 */
        if (HAL_GetTick() - last_rec_tick > DOWNLOAD_TIMEOUT_MS)
        {
            printf("\r\n===== TIMEOUT! got %lu / %lu =====\r\n",
                   (unsigned long)uart_rec_full_len,
                   (unsigned long)updata_real_len);
            HAL_UART_DMAStop(&huart2);
            receive_data_flag  = 0;
            download_status    = CHECK_DATA_FAIL;
            return;
        }
    }

    /* 下载结束，停止 DMA */
    HAL_UART_DMAStop(&huart2);
    receive_data_flag = 0;

    printf("Total written to Flash: %lu bytes\r\n",
           (unsigned long)uart_rec_full_len);
    
    
    download_status = CHECK_DATA;
}

/*===========================================================================
 *                            CRC 校验
 *===========================================================================*/

/**
 * @brief  校验已下载固件: CRC32 + 版本号
 *
 *  CRC 不匹配 → CHECK_DATA_FAIL (触发重传)
 *  版本回退 → CHECK_DATA_FAIL
 *  全部通过 → 写入 EEPROM 元数据 → CHECK_DATA_OK
 */
void check_data(void)
{
    printf("Flash read addr: 0x%08lX\r\n", (unsigned long)w25q_read_addr);

    uint32_t crc_val = OTA_CalcBinCRC();
    printf("calc CRC: 0x%08lX  expect: 0x%08lX\r\n",
           (unsigned long)crc_val, (unsigned long)crc_real_value);

    if (crc_real_value != crc_val)
    {
        printf("CRC check FAILED\r\n");
        download_status = CHECK_DATA_FAIL;
        return;
    }

    /* 防止版本回退 */
    device_information dev_info;
    eeprom_read_bytes(DEVICE_INFORMATION_ADDR, (uint8_t *)&dev_info, sizeof(dev_info));
    if (version < dev_info.new_version)
    {
        printf("Version downgrade rejected: current=%u, new=%u\r\n",
               dev_info.new_version, version);
        download_status = CHECK_DATA_FAIL;
        return;
    }

    printf("CRC + Version check PASS\r\n");
    app_bootloader_write_update();
    download_status = CHECK_DATA_OK;
}

/*===========================================================================
 *                        状态处理函数
 *===========================================================================*/

static uint8_t init_status = CONNECT_WIFI;

/**
 * @brief  WiFi 连接 → TCP 连接 两步初始化
 */
static void connect_tcp_init(void)
{
    switch (init_status)
    {
    case CONNECT_WIFI:
        if (esp8266_init() != 0)
        {
            printf("WIFI CONNECT FAIL\r\n");
            download_status = CONNECT_TCP_FAIL;
        }
        else
        {
            printf("WIFI CONNECT SUCCESS\r\n");
            init_status = CONNECT_TCP;
        }
        break;

    case CONNECT_TCP:
        if (esp8266_tcp_connect_init() != 0)
        {
            printf("TCP CONNECT FAIL\r\n");
            download_status = CONNECT_TCP_FAIL;
        }
        else
        {
            printf("TCP CONNECT SUCCESS\r\n");
            download_status = CONNECT_TCP_SUCCESS;
            init_status     = CONNECT_WIFI;  /* 下次进入时重新从 WiFi 开始 */
        }
        break;
    }
}

/**
 * @brief  TCP 连接失败处理: 等待用户按键选择重试或退出
 *
 *  GPIO_PIN_13 → 退出，跳转 APP
 *  GPIO_PIN_14 → 重试
 *  超时       → 自动跳转 APP
 */
static void tcp_connect_fail_handle(void)
{
    scan_tick = HAL_GetTick();
    while (1)
    {
        if (Key_Scan(GPIO_PIN_13) == 1)
        {
            download_status = DOWNLOAD_ERROR;
            break;
        }
        if (Key_Scan(GPIO_PIN_14) == 1)
        {
            init_status     = CONNECT_WIFI;
            download_status = CONNECT_TCP;
            break;
        }
        if (HAL_GetTick() - scan_tick >= TCP_RETRY_TIMEOUT_MS)
        {
            jump_to_app_area();
            break;
        }
    }
}

/**
 * @brief  数据校验失败处理: 等待用户按键选择重传或退出
 *
 *  GPIO_PIN_13 → 退出
 *  GPIO_PIN_14 → 请求服务端重传
 *  超时       → 退出
 */
static void check_data_error_handle(void)
{
    scan_tick = HAL_GetTick();
    while (1)
    {
        if (Key_Scan(GPIO_PIN_13) == 1)
        {
            download_status = DOWNLOAD_ERROR;
            break;
        }
        if (Key_Scan(GPIO_PIN_14) == 1)
        {
            TCP_Send_Format("data error, please resend");
            uint8_t buf[TCP_LINE_BUF_SIZE];
            receive_tcp_data(buf);
            buf[TCP_LINE_BUF_SIZE - 1] = '\0';
            if (strstr((char *)buf, "ok") != NULL ||
                strstr((char *)buf, "OK") != NULL)
            {
                download_status = CONNECT_TCP_SUCCESS;
                break;
            }
        }
        if (HAL_GetTick() - scan_tick >= KEY_SELECT_TIMEOUT_MS)
        {
            download_status = DOWNLOAD_ERROR;
            break;
        }
    }
}

/**
 * @brief  下载完成后选择跳转目标: APP 或 UPDATA 分区
 *
 *  GPIO_PIN_13 → 跳转 APP
 *  GPIO_PIN_14 → 跳转 UPDATA
 *  超时       → 默认跳转 APP
 */
static void inquiry_jump_to_which_app_area(void)
{
    scan_tick = HAL_GetTick();
    while (1)
    {
        if (Key_Scan(GPIO_PIN_13) == 1) { jump_to_app_area();    break; }
        if (Key_Scan(GPIO_PIN_14) == 1) { jump_to_updata_area(); break; }
        if (HAL_GetTick() - scan_tick >= KEY_SELECT_TIMEOUT_MS)
        {
            jump_to_app_area();
            break;
        }
    }
}

/*===========================================================================
 *                        OTA 主状态机
 *===========================================================================*/

void download_app_run(void)
{
    switch (download_status)
    {
    case CONNECT_TCP:         connect_tcp_init();                break;
    case CONNECT_TCP_FAIL:    tcp_connect_fail_handle();         break;
    case CONNECT_TCP_SUCCESS: updata_ack();                      break;
    case DOWNLOAD_ERROR:      jump_to_app_area();                break;
    case DOWNLOAD_START:      receive_data_write_to_flash();     break;
    case CHECK_DATA:          check_data();                      break;
    case CHECK_DATA_OK:       inquiry_jump_to_which_app_area();  break;
    case CHECK_DATA_FAIL:     check_data_error_handle();         break;
    default:                  jump_to_download_area();           break;
    }
}

/*===========================================================================
 *                        调试辅助函数
 *===========================================================================*/

/**
 * @brief  打印固件首尾各 50 字节，用于快速验证 Flash 数据
 */
void print_flash_data(void)
{
    uint32_t base = w25q_read_addr;
    uint32_t len  = updata_real_len;
    uint8_t  buf[50];

    if (len < 100)
    {
        printf("firmware too short for debug print (%lu bytes)\r\n",
               (unsigned long)len);
        return;
    }

    /* 前 50 字节 */
    ext_flash_read_data(base, buf, 50);
    printf("Flash 0x%08lX 前 50 字节:\r\n", (unsigned long)base);
    for (uint16_t i = 0; i < 50; i++)
        printf("%02X ", buf[i]);
    printf("\r\n");

    /* 后 50 字节 */
    ext_flash_read_data(base + len - 50, buf, 50);
    printf("Flash 0x%08lX 后 50 字节:\r\n", (unsigned long)(base + len - 50));
    for (uint16_t i = 0; i < 50; i++)
        printf("%02X ", buf[i]);
    printf("\r\n");
}

/*===========================================================================
 *                        EEPROM 调试函数
 *===========================================================================*/

/**
 * @brief  恢复出厂默认值: 将所有 OTA 相关 EEPROM 数据清零/初始化
 *
 *  谨慎调用! 会清除分区信息、版本记录等所有数据。
 */
void ota_eeprom_init(void)
{
    /* 1. 升级标志 (CHECK_UPDATE_ADDR) */
    updata_val val = {0};
    val.BOOT_UPDATE_FLAG       = BOOT_NO_UPDATA;
    val.NEW_APP_SAVE_ADDR_FLAG = 0;
    val.OLD_APP_SAVE_ADDR_FLAG = 0;
    val.NEW_DATA_LEN = 0;
    val.OLD_DATA_LEN = 0;
    eeprom_write_bytes(CHECK_UPDATE_ADDR, (uint8_t *)&val, sizeof(updata_val));

    /* 2. 设备信息 (DEVICE_INFORMATION_ADDR) */
    device_information dev = {0};
    dev.model       = 23;
    dev.new_version = 1;
    dev.old_version = 1;
    eeprom_write_bytes(DEVICE_INFORMATION_ADDR, (uint8_t *)&dev, sizeof(device_information));

    /* 3. CRC 数据 (CRC_VALUE_ADDR) */
    crc_data crc = {0};
    crc.OLD_APP_DATA_CRC = 0xFFFFFFFF;
    crc.NEW_APP_DATA_CRC = 0xFFFFFFFF;
    eeprom_write_bytes(CRC_VALUE_ADDR, (uint8_t *)&crc, sizeof(crc_data));

    /* 4. 4 分区回滚表 (ROLLBACK_INFO_ADDR) */
    rollback_info_t rollback = {0};
    for (int i = 0; i < 4; i++)
    {
        rollback.part[i].partition_version = 0;
        rollback.part[i].partition_crc     = 0xFFFFFFFF;
        rollback.part[i].DATA_LEN          = 0;
    }
    eeprom_write_bytes(ROLLBACK_INFO_ADDR, (uint8_t *)&rollback, sizeof(rollback_info_t));

    printf("OTA EEPROM 初始化完成!\r\n");
    printf("设备型号:%d, 版本:%d, 默认分区:%d\r\n",
           dev.model, dev.new_version, val.NEW_APP_SAVE_ADDR_FLAG);
}

/**
 * @brief  读取并打印所有 OTA 相关 EEPROM 数据 (调试用)
 */
void ota_eeprom_read_and_print(void)
{
    updata_val         val;
    device_information dev;
    crc_data           crc;
    rollback_info_t    rollback;

    eeprom_read_bytes(CHECK_UPDATE_ADDR,      (uint8_t *)&val,      sizeof(val));
    eeprom_read_bytes(DEVICE_INFORMATION_ADDR, (uint8_t *)&dev,      sizeof(dev));
    eeprom_read_bytes(CRC_VALUE_ADDR,          (uint8_t *)&crc,      sizeof(crc));
    eeprom_read_bytes(ROLLBACK_INFO_ADDR,      (uint8_t *)&rollback, sizeof(rollback));

    printf("\r\n=========================================\r\n");
    printf("        OTA EEPROM 读取结果\r\n");
    printf("=========================================\r\n");

    printf("启动标志        : %d\r\n", val.BOOT_UPDATE_FLAG);
    printf("新固件分区号    : %d\r\n", val.NEW_APP_SAVE_ADDR_FLAG);
    printf("旧固件分区号    : %d\r\n", val.OLD_APP_SAVE_ADDR_FLAG);
    printf("新固件长度      : %lu\r\n", (unsigned long)val.NEW_DATA_LEN);
    printf("旧固件长度      : %lu\r\n", (unsigned long)val.OLD_DATA_LEN);

    printf("-----------------------------------------\r\n");
    printf("设备型号        : %d\r\n", dev.model);
    printf("当前版本        : %d\r\n", dev.new_version);
    printf("旧版本          : %d\r\n", dev.old_version);

    printf("-----------------------------------------\r\n");
    printf("旧固件 CRC32    : 0x%08X\r\n", (unsigned)crc.OLD_APP_DATA_CRC);
    printf("新固件 CRC32    : 0x%08X\r\n", (unsigned)crc.NEW_APP_DATA_CRC);

    printf("-----------------------------------------\r\n");
    printf("4 分区回滚信息表:\r\n");
    for (int i = 0; i < 4; i++)
    {
        printf("  分区%d  版本:%d  CRC:0x%08X  长度:%lu\r\n",
               i,
               rollback.part[i].partition_version,
               (unsigned)rollback.part[i].partition_crc,
               (unsigned long)rollback.part[i].DATA_LEN);
    }
    printf("=========================================\r\n\r\n");
}
