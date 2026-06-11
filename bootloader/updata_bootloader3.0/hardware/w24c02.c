#include "w24c02.h"


uint8_t w24c02_read_byte(uint8_t byte_addr)
{
    uint8_t data;  
    HAL_I2C_Mem_Read(&hi2c2, W24C02_ADDR_R, byte_addr, I2C_MEMADD_SIZE_8BIT, &data, 1, 100);
    return data;
}

void w24c02_write_byte(uint8_t byte_addr, uint8_t data)
{   
    HAL_I2C_Mem_Write(&hi2c2, W24C02_ADDR, byte_addr, I2C_MEMADD_SIZE_8BIT, &data, 1, 100);
}


void w24c02_read_bytes(uint8_t byte_addr, uint8_t *data, uint16_t len)
{
    HAL_I2C_Mem_Read(&hi2c2, W24C02_ADDR, byte_addr, I2C_MEMADD_SIZE_8BIT, data, len, 1000);
}


void w24c02_write_bytes(uint8_t byte_addr, uint8_t *data, uint16_t len)
{
    
    if (byte_addr + len > 255)
    {
        printf("EEROR\r\n");
        return;
    }

    uint8_t page_remain_len = 16 - byte_addr % 16;

    if (len <= page_remain_len)
    {        
        HAL_I2C_Mem_Write(&hi2c2, W24C02_ADDR, byte_addr, I2C_MEMADD_SIZE_8BIT, data, len, 1000);
    }
    else
    {
			
        uint8_t start_page_addr = byte_addr;    
        uint8_t page_count = 0;
        while (len > page_remain_len)
        {
            HAL_I2C_Mem_Write(&hi2c2, W24C02_ADDR, start_page_addr, I2C_MEMADD_SIZE_8BIT, data + page_count * 16, page_remain_len, 1000);
            page_count++;
            start_page_addr += page_remain_len;
            len -= page_remain_len;
            page_remain_len = 16;
            HAL_Delay(10);
        }
        if (len != 0)
        {
            HAL_I2C_Mem_Write(&hi2c2, W24C02_ADDR, start_page_addr, I2C_MEMADD_SIZE_8BIT, data + page_count * 16, len, 1000);
        }
    }
}
