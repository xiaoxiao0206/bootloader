#include "eeprom.h"

// 读取单个字节
uint8_t eeprom_read_byte(uint8_t byte_addr)
{
    uint8_t data;
    HAL_I2C_Mem_Read(&hi2c2, W24C02_ADDR_R, byte_addr, I2C_MEMADD_SIZE_8BIT, &data, 1, 100);
    return data;
}

// 写入单个字节
void eeprom_write_byte(uint8_t byte_addr, uint8_t data)
{
    HAL_I2C_Mem_Write(&hi2c2, W24C02_ADDR, byte_addr, I2C_MEMADD_SIZE_8BIT, &data, 1, 100);
}

// 连续读取多个字节
void eeprom_read_bytes(uint8_t byte_addr, uint8_t *data, uint16_t len)
{
    HAL_I2C_Mem_Read(&hi2c2, W24C02_ADDR, byte_addr, I2C_MEMADD_SIZE_8BIT, data, len, 1000);
}



// 连续写入多个字节（分页写入，适配W24C02硬件特性）
void eeprom_write_bytes(uint8_t byte_addr, uint8_t *data, uint16_t len)
{
   if (byte_addr + len > 256)  // 24C02 地址范围 0-255
    {
        printf("ERROR: address out of range\r\n");
        return;
    }

    while (len > 0)
    {
        uint8_t page_remain = EEPROM_PAGE_SIZE - (byte_addr % EEPROM_PAGE_SIZE);
        uint8_t write_len = (len > page_remain) ? page_remain : len;
        
        HAL_I2C_Mem_Write(&hi2c2, W24C02_ADDR, byte_addr, I2C_MEMADD_SIZE_8BIT, data, write_len, 1000);
        HAL_Delay(10);  // 等待页写完成（24C02 典型写周期 5ms）
        
        byte_addr += write_len;
        data += write_len;
        len -= write_len;
    }
}
