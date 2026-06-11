#ifndef __INT_W24C02_H
#define __INT_W24C02_H

#include "i2c.h"
#include "usart.h"
#define W24C02_ADDR 0xA0
#define W24C02_ADDR_R (W24C02_ADDR | 0x01)

#define W24C02_ADDR_SIZE 8
#define W24C02_PAGE_SIZE 16

uint8_t w24c02_read_byte(uint8_t byte_addr);

void w24c02_write_byte(uint8_t byte_addr, uint8_t data);

void w24c02_read_bytes(uint8_t byte_addr, uint8_t *data, uint16_t len);

void w24c02_write_bytes(uint8_t byte_addr, uint8_t *data, uint16_t len);

#endif 
