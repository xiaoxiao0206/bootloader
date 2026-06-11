#ifndef __EXT_FLASH_H
#define __EXT_FLASH_H
#include "spi.h"

#define W25Q_JEDEC_ID 0x9F
#define W25Q_READ_STATUS_REG 0x05
#define W25Q_READ_DATA 0x03
#define W25Q_WRITE_DATA 0x02
#define W25Q_ERASE_SECTOR 0x20
#define W25Q_WRITE_ENABLE 0X06
#define SECTOR_SIZE 0x1000

void ext_flash_start(void);
void ext_flash_stop(void);
void ext_flash_write_byte(uint8_t data);
uint8_t ext_flash_read_byte(void);
void ext_flash_read_id(uint8_t *mf_id, uint16_t *device_id);
void ext_flash_read_data(uint32_t addr, uint8_t *data, uint16_t len);
void ext_flash_write_data(uint32_t addr, uint8_t *data, uint16_t len);
void ext_flash_erase_sector(uint32_t sector_addr);
void ext_flash_erase_area(uint32_t start_addr, uint32_t total_len);
#endif
