#ifndef _ESP8266_H_
#define _ESP8266_H_
#include"main.h"
#include <stdio.h>   
#include <string.h>
#include <stdarg.h>

void uart2_receiver_handle(void);

uint16_t parse_json_msg(uint8_t *json_msg, uint8_t json_len);

uint8_t esp8266_send_cmd(unsigned char *cmd, unsigned char len, char *rec_data);

uint8_t esp8266_config_network(void);

uint8_t esp8266_connect_server(void);

uint8_t esp8266_reset(void);

uint8_t esp8266_send_msg(const char *name, uint16_t value);

uint16_t esp8266_receive_msg(void);

uint8_t esp8266_init(void);

uint8_t esp8266_tcp_connect_init(void);


void TCP_Send_Format(const char *format, ...);

void receive_tcp_data(uint8_t *buf);

#endif
