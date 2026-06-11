#ifndef __EEPROM_H
#define __EEPROM_H

#include "i2c.h"
#include "usart.h"

#define EEPROM_PAGE_SIZE 8 

#define W24C02_ADDR 0xA0
#define W24C02_ADDR_R (W24C02_ADDR | 0x01)

#define W24C02_ADDR_SIZE 8
#define W24C02_PAGE_SIZE 16

uint8_t eeprom_read_byte(uint8_t byte_addr);

void eeprom_write_byte(uint8_t byte_addr, uint8_t data);

void eeprom_read_bytes(uint8_t byte_addr, uint8_t *data, uint16_t len);

void eeprom_write_bytes(uint8_t byte_addr, uint8_t *data, uint16_t len);

#endif 
