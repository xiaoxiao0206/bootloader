#ifndef __W25Q128_H
#define __W25Q128_H
#include "spi.h"

#define W25Q_JEDEC_ID 0x9F
#define W25Q_READ_STATUS_REG 0x05
#define W25Q_READ_DATA 0x03
#define W25Q_WRITE_DATA 0x02
#define W25Q_ERASE_SECTOR 0x20
#define W25Q_WRITE_ENABLE 0X06
#define SECTOR_SIZE 0x1000

void w25q128_start(void);
void w25q128_stop(void);
void w25q128_write_byte(uint8_t data);
uint8_t w25q128_read_byte(void);
void w25q128_read_id(uint8_t *mf_id, uint16_t *device_id);
void w25q128_read_data_with_32addr(uint32_t addr, uint8_t *data, uint16_t len);
void w25q128_write_data(uint8_t block, uint8_t sector, uint8_t page, uint8_t addr, uint8_t *data, uint16_t len);
void w25q128_write_datawith_32addr(uint32_t addr, uint8_t *data, uint16_t len);
void w25q128_erase_sector(uint32_t sector_addr);
void w25q128_erase_area(uint32_t start_addr, uint32_t total_len);
#endif
