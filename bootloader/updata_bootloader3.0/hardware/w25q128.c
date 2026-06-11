#include "w25q128.h"

void w25q128_start(void)
{
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_12, GPIO_PIN_RESET);
}

void w25q128_stop(void)
{
    HAL_GPIO_WritePin(GPIOB,GPIO_PIN_12, GPIO_PIN_SET);
}

void w25q128_write_byte(uint8_t data)
{
    HAL_SPI_Transmit(&hspi2, &data, 1, 100);
}

uint8_t w25q128_read_byte(void)
{
    uint8_t data;
    HAL_SPI_Receive(&hspi2, &data, 1, 100);
    return data;
}

void w25q128_read_id(uint8_t *mf_id, uint16_t *device_id)
{
    w25q128_start();
    w25q128_write_byte(W25Q_JEDEC_ID);

    *mf_id = w25q128_read_byte();
    uint8_t high = w25q128_read_byte();
    uint8_t low = w25q128_read_byte();
    *device_id = high << 8 | low;

    w25q128_stop();
}

static void w25q128_wait_busy(void)
{
    w25q128_start();
    while (1)
    {
        w25q128_write_byte(W25Q_READ_STATUS_REG);
        uint8_t status = w25q128_read_byte();
        if ((status & 0x01) == 0)
        {
            break;
        }
    }
    w25q128_stop();
}

void w25q128_read_data_with_32addr(uint32_t addr, uint8_t *data, uint16_t len)
{
    w25q128_start();
    w25q128_write_byte(W25Q_READ_DATA);
    w25q128_write_byte((addr >> 16) & 0xFF);
    w25q128_write_byte((addr >> 8) & 0xFF);
    w25q128_write_byte(addr & 0xFF);

    for (uint16_t i = 0; i < len; i++)
    {
        data[i] = w25q128_read_byte();
    }
    w25q128_stop();
}

static void w25q128_write_enable(void)
{
    w25q128_start();
    w25q128_write_byte(W25Q_WRITE_ENABLE);
    w25q128_stop();
}

void w25q128_write_data(uint8_t block, uint8_t sector, uint8_t page, uint8_t addr, uint8_t *data, uint16_t len)
{
    w25q128_write_enable();

    w25q128_start();
    uint32_t addr_24 = (uint32_t)block << 16 | (uint32_t)sector << 12 | (uint32_t)page << 8 | addr;
    w25q128_write_byte(W25Q_WRITE_DATA);
    w25q128_write_byte((addr_24 >> 16) & 0xFF);
    w25q128_write_byte((addr_24 >> 8) & 0xFF);
    w25q128_write_byte(addr_24 & 0xFF);
    
    for (uint16_t i = 0; i < len; i++)
    {
        w25q128_write_byte(data[i]);
    }
    w25q128_stop();

    w25q128_wait_busy();
}

void w25q128_write_datawith_32addr(uint32_t addr, uint8_t *data, uint16_t len)
{
    w25q128_write_enable();
    w25q128_start();
    w25q128_write_byte(W25Q_WRITE_DATA);
    w25q128_write_byte((addr >> 16) & 0xFF); 
    w25q128_write_byte((addr >> 8) & 0xFF);  
    w25q128_write_byte(addr & 0xFF);         

    for (uint16_t i = 0; i < len; i++)
    {
        w25q128_write_byte(data[i]);
    }

    w25q128_stop();
    w25q128_wait_busy();
}

void w25q128_erase_sector(uint32_t sector_addr)
{
    
    w25q128_write_enable(); 
    w25q128_start();
    w25q128_write_byte(W25Q_ERASE_SECTOR);

    w25q128_write_byte((sector_addr >> 16) & 0xFF);
    w25q128_write_byte((sector_addr >> 8) & 0xFF);
    w25q128_write_byte(sector_addr & 0xFF);

    w25q128_stop();
    w25q128_wait_busy();
}

void w25q128_erase_area(uint32_t start_addr, uint32_t total_len)
{
    
    uint16_t sector_count = total_len / SECTOR_SIZE;

    for (uint16_t i = 0; i < sector_count; i++)
    {
        w25q128_erase_sector(start_addr); 
        start_addr += SECTOR_SIZE;       
    }
}
