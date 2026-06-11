#ifndef __BOOTLADER_H
#define __BOOTLADER_H

#include "usart.h"
#include "stdlib.h"
#include "string.h"
#define BOOTLOADER_UART_REC_BUFF_LEN 512


void Int_bootloader_receive_app(void);
uint8_t bootloader_jump_to_app(void);
void Int_bootloader_erase_flash(uint32_t page_addr, uint16_t pages);

#endif

