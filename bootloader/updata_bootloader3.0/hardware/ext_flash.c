#include "ext_flash.h"

// Flash片选使能（拉低CS）
void ext_flash_start(void)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_RESET);
}

// Flash片选失能（拉高CS）
void ext_flash_stop(void)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12, GPIO_PIN_SET);
}

// SPI单字节发送
void ext_flash_write_byte(uint8_t data)
{
    HAL_SPI_Transmit(&hspi2, &data, 1, 100);
}

// SPI单字节读取
uint8_t ext_flash_read_byte(void)
{
    uint8_t data;
    HAL_SPI_Receive(&hspi2, &data, 1, 100);
    return data;
}

// 读取Flash ID（厂商ID+设备ID）
void ext_flash_read_id(uint8_t *mf_id, uint16_t *device_id)
{
    ext_flash_start();
    ext_flash_write_byte(W25Q_JEDEC_ID);

    *mf_id = ext_flash_read_byte();
    uint8_t high = ext_flash_read_byte();
    uint8_t low = ext_flash_read_byte();
    *device_id = high << 8 | low;

    ext_flash_stop();
}

// 等待Flash空闲（内部静态函数）
static void ext_flash_wait_busy(void)
{
    ext_flash_start();
    while (1)
    {
        ext_flash_write_byte(W25Q_READ_STATUS_REG);
        uint8_t status = ext_flash_read_byte();
        if ((status & 0x01) == 0)
        {
            break;
        }
    }
    ext_flash_stop();
}

// 32位地址读取Flash数据
void ext_flash_read_data(uint32_t addr, uint8_t *data, uint16_t len)
{
    ext_flash_start();
    ext_flash_write_byte(W25Q_READ_DATA);
    ext_flash_write_byte((addr >> 16) & 0xFF);
    ext_flash_write_byte((addr >> 8) & 0xFF);
    ext_flash_write_byte(addr & 0xFF);

    for (uint16_t i = 0; i < len; i++)
    {
        data[i] = ext_flash_read_byte();
    }
    ext_flash_stop();
}

// 写使能（内部静态函数）
static void ext_flash_write_enable(void)
{
    ext_flash_start();
    ext_flash_write_byte(W25Q_WRITE_ENABLE);
    ext_flash_stop();
}


// 32位地址分页写入（自动跨页，推荐使用）
void ext_flash_write_data(uint32_t addr, uint8_t *data, uint16_t len)
{
    uint16_t remaining = len;
    uint16_t offset = 0;

    while (remaining > 0)
    {
        uint16_t page_offset = addr % 256;
        uint16_t chunk = 256 - page_offset;
        if (chunk > remaining)
            chunk = remaining;

        ext_flash_write_enable();
        ext_flash_start();
        ext_flash_write_byte(W25Q_WRITE_DATA);
        ext_flash_write_byte((addr >> 16) & 0xFF);
        ext_flash_write_byte((addr >> 8) & 0xFF);
        ext_flash_write_byte(addr & 0xFF);

        for (uint16_t i = 0; i < chunk; i++)
        {
            ext_flash_write_byte(data[offset + i]);
        }

        ext_flash_stop();
        ext_flash_wait_busy();

        addr += chunk;
        offset += chunk;
        remaining -= chunk;
    }
}

// 扇区擦除
void ext_flash_erase_sector(uint32_t sector_addr)
{
    ext_flash_write_enable();
    ext_flash_start();
    ext_flash_write_byte(W25Q_ERASE_SECTOR);

    ext_flash_write_byte((sector_addr >> 16) & 0xFF);
    ext_flash_write_byte((sector_addr >> 8) & 0xFF);
    ext_flash_write_byte(sector_addr & 0xFF);

    ext_flash_stop();
    ext_flash_wait_busy();
}

// 批量擦除指定区域
void ext_flash_erase_area(uint32_t start_addr, uint32_t total_len)
{
    uint16_t sector_count = total_len / SECTOR_SIZE;

    for (uint16_t i = 0; i < sector_count; i++)
    {
        ext_flash_erase_sector(start_addr);
        start_addr += SECTOR_SIZE;
    }
    
}
