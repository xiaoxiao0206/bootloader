#include "App_bootloader.h"

/*===========================================================================
 *                            全局变量
 *===========================================================================*/

uint8_t  app_boot_update_status = BOOT_CHECK_UPDATA;
updata_val receive_updata;
crc_data   crc_cal;

static uint32_t scan_tick         = 0;
static uint32_t updata_start_addr = 0;
static uint32_t data_len          = 0;
static uint8_t  back_version      = 0;   /* 0=新版本 1=回滚旧版本 */
uint8_t buf[1024];

/*===========================================================================
 *                            工具函数
 *===========================================================================*/

static uint8_t Key_Scan(uint16_t GPIO_Pin)
{
  if (HAL_GPIO_ReadPin(GPIOF, GPIO_Pin) == 0)
  {
    HAL_Delay(20);
    if (HAL_GPIO_ReadPin(GPIOF, GPIO_Pin) == 0)
    {
      while (HAL_GPIO_ReadPin(GPIOF, GPIO_Pin) == 0)
        HAL_Delay(20);
      return 1;
    }
  }
  return 0;
}

/*===========================================================================
 *                            EEPROM 读取
 *===========================================================================*/

static void get_eeprom_data(void)
{
  eeprom_read_bytes(CRC_VALUE_ADDR,    (uint8_t *)&crc_cal,      sizeof(crc_cal));
  eeprom_read_bytes(CHECK_UPDATE_ADDR, (uint8_t *)&receive_updata, sizeof(receive_updata));
}

/*===========================================================================
 *                   分区地址查询
 *===========================================================================*/

static const uint32_t partition_addrs[4] = {
  PARTITION_0_ADDR,
  PARTITION_1_ADDR,
  PARTITION_2_ADDR,
  PARTITION_3_ADDR
};

static void get_data_addr(uint8_t flag)
{
  if (flag < 4)
  {
    updata_start_addr = partition_addrs[flag];
    printf("use_%u: %08X\r\n", flag, updata_start_addr);
  }
}

/*===========================================================================
 *                   统一跳转接口
 *===========================================================================*/

/**
 * @brief  校验目标区域的栈指针和复位向量，通过则跳转
 *         共用逻辑，三个跳转函数都调这个
 */
static void jump_to(uint32_t area_start, uint32_t area_end, const char *name)
{
  uint32_t sp = *(volatile uint32_t *)(area_start);
  uint32_t pc = *(volatile uint32_t *)(area_start + 4);

  if ((sp & 0xFFFF0000) != STACK_ADDR)
  {
    printf("%s stack addr error: 0x%08X\r\n", name, sp);
    return;
  }
  if (pc < area_start || pc > area_end)
  {
    printf("%s reset handle error: 0x%08X\r\n", name, pc);
    return;
  }

  NVIC_DisableIRQ(USART1_IRQn);
  NVIC_DisableIRQ(TIM7_IRQn);
  SysTick->CTRL = 0;
  SysTick->LOAD = 0;
  SysTick->VAL  = 0;

  __disable_irq();
  __set_MSP(sp);
  SCB->VTOR = area_start;
  ((void (*)(void))pc)();
}

void jump_to_app_area(void)
{
  printf("jump app\r\n");
  jump_to(APP_START_ADDR, APP_END_ADDR, "APP");
}

void jump_to_download_area(void)
{
  jump_to(DOWNLOAD_START_ADDR, DOWNLOAD_END_ADDR, "DOWNLOAD");
}

void jump_to_factory_app_area(void)
{
  jump_to(FACTORY_START_ADDR, FACTORY_END_ADDR, "FACTORY");
}

/*===========================================================================
 *                   CRC32 计算
 *===========================================================================*/

/**
 * @brief  从外部 Flash 读取固件计算 CRC32
 */
static uint32_t ext_flash_calc_crc(uint32_t len, uint32_t addr)
{
  __HAL_CRC_DR_RESET(&hcrc);

  while (len > 0)
  {
    uint16_t chunk = (len > 1024) ? 1024 : (uint16_t)len;
    ext_flash_read_data(addr, buf, chunk);

    uint32_t word_cnt = chunk / 4;
    for (uint32_t i = 0; i < word_cnt; i++)
    {
      uint32_t word = ((uint32_t *)buf)[i];
     
      word = __REV(word);
      HAL_CRC_Accumulate(&hcrc, &word, 1);
    }

    uint8_t tail = chunk % 4;
    if (tail)
    {
      uint32_t last_word = 0;
      memcpy(&last_word, buf + chunk - tail, tail);
      /* tail 字节已经在低位，反转后低位变高位，
         硬件先处理高位 = 先处理原始低地址字节 */
      last_word = __REV(last_word);
      HAL_CRC_Accumulate(&hcrc, &last_word, 1);
    }

    addr += chunk;
    len  -= chunk;
  }

  return hcrc.Instance->DR;
}

/**
 * @brief  从内部 Flash (APP 区) 计算 CRC32
 */
static uint32_t internal_flash_calc_crc(uint32_t len, uint32_t addr)
{
  __HAL_CRC_DR_RESET(&hcrc);

  while (len > 0)
  {
    uint16_t chunk = (len > 1024) ? 1024 : (uint16_t)len;
    memcpy(buf, (const void *)addr, chunk);

    uint32_t word_cnt = chunk / 4;
    for (uint32_t i = 0; i < word_cnt; i++)
    {
      uint32_t word = ((uint32_t *)buf)[i];
      word = __REV(word);
      HAL_CRC_Accumulate(&hcrc, &word, 1);
    }

    uint8_t tail = chunk % 4;
    if (tail)
    {
      uint32_t last_word = 0;
      memcpy(&last_word, buf + chunk - tail, tail);
      last_word = __REV(last_word);
      HAL_CRC_Accumulate(&hcrc, &last_word, 1);
    }

    addr += chunk;
    len  -= chunk;
  }

  return hcrc.Instance->DR;
}

/* 保留旧名称兼容外部调用 */
uint32_t wirte_to_flash_data_check(void)
{
  return internal_flash_calc_crc(data_len, APP_START_ADDR);
}

/*===========================================================================
 *                   启动检查：读 EEPROM，按键选择
 *===========================================================================*/

void app_bootloader_check_update(void)
{
  printf("bootloader start\r\n");
  get_eeprom_data();
  printf("partition flag: %d\r\n", receive_updata.NEW_APP_SAVE_ADDR_FLAG);

  if (receive_updata.BOOT_UPDATE_FLAG == BOOT_UPDATA)
  {
    printf("new firmware ready, wait for key\r\n");
    scan_tick = HAL_GetTick();
    while (1)
    {
      if (Key_Scan(GPIO_PIN_13) == 1)
      {
        app_boot_update_status = BOOT_UPDATA;
        break;
      }
      if (Key_Scan(GPIO_PIN_14) == 1)
      {
        app_boot_update_status = BOOT_NO_UPDATA;
        break;
      }
      if (HAL_GetTick() - scan_tick >= 10000)
      {
        app_boot_update_status = BOOT_NO_UPDATA;
        break;
      }
    }
  }
  else if (receive_updata.BOOT_UPDATE_FLAG == BOOT_HAVE_NEW_DATA)
  {
    printf("new firmware not downloaded, wait for key\r\n");
    scan_tick = HAL_GetTick();
    while (1)
    {
      if (Key_Scan(GPIO_PIN_13) == 1)
      {
        app_boot_update_status = BOOT_HAVE_NEW_DATA;
        break;
      }
      if (Key_Scan(GPIO_PIN_14) == 1)
      {
        app_boot_update_status = BOOT_NO_UPDATA;
        break;
      }
      if (HAL_GetTick() - scan_tick >= 10000)
      {
        app_boot_update_status = BOOT_NO_UPDATA;
        break;
      }
    }
  }
  else
  {
    app_boot_update_status = BOOT_NO_UPDATA;
  }
}

/*===========================================================================
 *                   固件合法性校验
 *===========================================================================*/

void app_bootloader_check_data(void)
{
  data_len = (back_version == 0) ? receive_updata.NEW_DATA_LEN
                                  : receive_updata.OLD_DATA_LEN;
  get_data_addr((back_version == 0) ? receive_updata.NEW_APP_SAVE_ADDR_FLAG
                                     : receive_updata.OLD_APP_SAVE_ADDR_FLAG);

  printf("check data, len: %lu\r\n", data_len);

  /* 长度校验 */
  if (data_len < APP_SIZE_MIN || data_len > APP_SIZE_MAX)
  {
    printf("data len error\r\n");
    app_boot_update_status = BOOT_RESET;
    return;
  }

  /* 读取栈指针和复位向量 */
  uint8_t vector_buf[8];
  ext_flash_read_data(updata_start_addr, vector_buf, 8);

  uint32_t app_stack_ptr = vector_buf[0]
                         | (vector_buf[1] << 8)
                         | (vector_buf[2] << 16)
                         | (vector_buf[3] << 24);

  uint32_t app_reset_handle = vector_buf[4]
                            | (vector_buf[5] << 8)
                            | (vector_buf[6] << 16)
                            | (vector_buf[7] << 24);

  printf("stack: 0x%08X  reset: 0x%08X\r\n", app_stack_ptr, app_reset_handle);

  /* 栈指针校验 */
  if ((app_stack_ptr & 0xFFFF0000) != STACK_ADDR)
  {
    printf("stack addr error\r\n");
    app_boot_update_status = BOOT_RESET;
    return;
  }

  /* 复位向量校验 */
  if (app_reset_handle < APP_START_ADDR || app_reset_handle > APP_END_ADDR)
  {
    printf("reset handle error\r\n");
    app_boot_update_status = BOOT_RESET;
    return;
  }

  /* CRC 校验 */
  uint32_t expect_crc = (back_version == 0) ? crc_cal.NEW_APP_DATA_CRC
                                              : crc_cal.OLD_APP_DATA_CRC;
  uint32_t calc_crc   = ext_flash_calc_crc(data_len, updata_start_addr);

  printf("expect: %08X  calc: %08X\r\n", expect_crc, calc_crc);

  if (calc_crc == expect_crc)
  {
    printf("check pass\r\n");
    app_boot_update_status = BOOT_START_UPDATA;
  }
  else
  {
    printf("check failed\r\n");
    app_boot_update_status = BOOT_RESET;
  }
}

/*===========================================================================
 *                   内部 Flash 写入
 *===========================================================================*/

static uint8_t flash_data_buff[2049];

static void app_flash_erase(uint8_t pages)
{
  FLASH_EraseInitTypeDef erase_init;
  erase_init.TypeErase   = FLASH_TYPEERASE_PAGES;
  erase_init.Banks       = FLASH_BANK_1;
  erase_init.PageAddress = APP_START_ADDR;
  erase_init.NbPages     = pages;
  uint32_t page_error    = 0;
  HAL_FLASHEx_Erase(&erase_init, &page_error);
}

/**
 * @brief  将固件从外部 Flash 搬运到内部 Flash APP 区
 *
 *  关中断期间只做 Flash 操作，不调 printf。
 *  printf 统一移到开中断之后。
 */
void write_data_to_flash(void)
{
  uint32_t app_size      = data_len;
  uint32_t write_err_addr = 0;
  uint8_t  write_err      = 0;

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    printf("Flash unlock failed\r\n");
    app_boot_update_status = BOOT_RESET;
    return;
  }

  __disable_irq();

  app_flash_erase((app_size / FLASH_PAGE_SIZE) + 1);

  uint32_t remaining = app_size;

  /* 逐页写入（整页 + 尾部统一循环） */
  while (remaining > 0)
  {
    uint32_t offset = app_size - remaining;
    uint16_t chunk  = (remaining >= FLASH_PAGE_SIZE) ? FLASH_PAGE_SIZE
                                                      : (uint16_t)remaining;
    remaining -= chunk;

    ext_flash_read_data(updata_start_addr + offset, flash_data_buff, chunk);

    for (uint16_t i = 0; i < chunk; i += 2)
    {
      uint16_t halfword;
      if (i + 1 < chunk)
        halfword = flash_data_buff[i] | (flash_data_buff[i + 1] << 8);
      else
        halfword = flash_data_buff[i];  /* 奇数尾字节，高8位补0 */

      if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                             APP_START_ADDR + offset + i, halfword) != HAL_OK)
      {
        write_err_addr = APP_START_ADDR + offset + i;
        write_err = 1;
        break;
      }
    }

    if (write_err)
      break;
  }

  __enable_irq();
  HAL_FLASH_Lock();

  /* 所有 printf 移到开中断之后 */
  if (write_err)
  {
    printf("Flash write error at 0x%08X\r\n", write_err_addr);
    app_boot_update_status = BOOT_UPDATA_FAIL;
    return;
  }

  printf("write done: %lu bytes\r\n", app_size);

  /* 校验写入结果 */
  uint32_t expect_crc = (back_version == 0) ? crc_cal.NEW_APP_DATA_CRC
                                              : crc_cal.OLD_APP_DATA_CRC;
  uint32_t actual_crc = wirte_to_flash_data_check();

  if (actual_crc == expect_crc)
  {
    printf("update success\r\n");
    app_boot_update_status = BOOT_UPDATA_OK;
    receive_updata.BOOT_UPDATE_FLAG = BOOT_NO_UPDATA;
    eeprom_write_bytes(CHECK_UPDATE_ADDR, (uint8_t *)&receive_updata, sizeof(receive_updata));
  }
  else
  {
    printf("update verify failed\r\n");
    app_boot_update_status = BOOT_UPDATA_FAIL;
  }
}

void app_bootloader_start_updata(void)
{
  printf("start update\r\n");
  write_data_to_flash();
}

/*===========================================================================
 *                   升级失败处理
 *===========================================================================*/

static uint8_t fail_count = 0;

void boot_updata_fail_handle(void)
{
  if (fail_count > 3)
  {
    fail_count = 0;
    app_boot_update_status = BOOT_FACTORY_RESET;
    return;
  }

  scan_tick = HAL_GetTick();
  while (1)
  {
    if (Key_Scan(GPIO_PIN_13) == 1)
    {
      printf("retry new version\r\n");
      back_version = 0;
      fail_count++;
      app_boot_update_status = BOOT_UPDATA;
      break;
    }
    if (Key_Scan(GPIO_PIN_14) == 1)
    {
      printf("rollback to old version\r\n");
      back_version = 1;
      fail_count++;
      app_boot_update_status = BOOT_UPDATA;
      break;
    }
    if (HAL_GetTick() - scan_tick >= 10000)
    {
      printf("timeout, rollback\r\n");
      back_version = 1;
      fail_count++;
      app_boot_update_status = BOOT_UPDATA;
      break;
    }
  }
}

/*===========================================================================
 *                   主状态机
 *===========================================================================*/

void app_bootloader_run(void)
{
  switch (app_boot_update_status)
  {
  case BOOT_CHECK_UPDATA:   app_bootloader_check_update();  break;
  case BOOT_NO_UPDATA:      jump_to_app_area();             break;
  case BOOT_HAVE_NEW_DATA:  jump_to_download_area();        break;
  case BOOT_UPDATA:         app_bootloader_check_data();    break;
  case BOOT_START_UPDATA:   app_bootloader_start_updata();  break;
  case BOOT_UPDATA_OK:      jump_to_app_area();             break;
  case BOOT_UPDATA_FAIL:    boot_updata_fail_handle();      break;
  case BOOT_RESET:          jump_to_app_area();             break;
  case BOOT_FACTORY_RESET:  jump_to_factory_app_area();     break;
  default:                  app_boot_update_status = BOOT_NO_UPDATA; break;
  }
}

/*===========================================================================
 *                  进入开发者模式
 *===========================================================================*/

void start_delay(void)
{
  HAL_Delay(5000);
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == GPIO_PIN_3)
    app_boot_update_status = BOOT_FACTORY_RESET;
}
