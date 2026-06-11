#include "esp8266.h"
#include "usart.h"
#include <stdlib.h>
#include "ota.h"

/*注意注意注意！！！！串口2的中断函数要进行修改如下，否则程序卡死*/
// void USART2_IRQHandler(void)
// {
//   /* USER CODE BEGIN USART2_IRQn 0 */
// if (receive_data_flag == 0)
// {
//     uart2_receiver_handle();
// }
// /* USER CODE END USART2_IRQn 0 */
// HAL_UART_IRQHandler(&huart2);
// /* USER CODE BEGIN USART2_IRQn 1 */

// /* USER CODE END USART2_IRQn 1 */
// }


// ===================== 宏定义=====================
#define WIFI_SSID        "lvluoer"          // WiFi名称
#define WIFI_PASSWD      "mx1234567890"     // WiFi密码
#define TCP_SERVER_IP    "192.168.0.88"    // TCP服务器IP
#define TCP_SERVER_PORT  "8080"             // TCP服务器端口

    // ===================== 变量定义 =====================
    unsigned char receive_buf[128]; // 串口2接收缓存
unsigned char receive_start = 0; // 接收开始标志
uint16_t receive_count = 0;       // 接收数据计数
uint16_t receive_finish = 0;     // 接收完成标志


// ===================== 串口2接收处理 =====================
void uart2_receiver_handle(void)
{
    unsigned char receive_data = 0;
    if (__HAL_UART_GET_FLAG(&huart2, UART_FLAG_RXNE) != RESET)
    {
        HAL_UART_Receive(&huart2, &receive_data, 1, 1000);
        receive_buf[receive_count++] = receive_data;
        receive_start = 1;
        receive_finish = 0;
    }
}

// ===================== 串口2接收清空 =====================
void uart2_receiver_clear(uint16_t len)
{
    memset(receive_buf, 0x00, len);
    receive_count = 0;
    receive_start = 0;
    receive_finish = 0;
}

// ===================== 保留：ESP8266发送命令 =====================
uint8_t esp8266_send_cmd(unsigned char *cmd, unsigned char len, char *rec_data)
{
    unsigned char retval = 0;
    unsigned int count = 0;

    HAL_UART_Transmit(&huart2, cmd, len, 1000);
    while ((receive_start == 0) && (count < 1000))
    {
        count++;
        HAL_Delay(1);
    }

    if (count >= 1000)
    {
        retval = 1;
    }
    else
    {
        do
        {
            receive_finish++;
            HAL_Delay(1);
        } while (receive_finish < 500);
        retval = 2;
        if (strstr((const char *)receive_buf, rec_data))
        {
            retval = 0;
        }
    }
    uart2_receiver_clear(receive_count);
    return retval;
}

// ===================== 保留：WiFi配置 =====================
uint8_t esp8266_config_network(void)
{
    uint8_t retval = 0;
    uint16_t count = 0;

    HAL_UART_Transmit(&huart2, (unsigned char *)"AT+CWJAP=\"" WIFI_SSID "\",\"" WIFI_PASSWD "\"\r\n", strlen("AT+CWJAP=\"" WIFI_SSID "\",\"" WIFI_PASSWD "\"\r\n"), 1000);

    while ((receive_start == 0) && (count < 1000))
    {
        count++;
        HAL_Delay(1);
    }

    if (count >= 1000)
    {
        retval = 1;
    }
    else
    {
        HAL_Delay(8000);
        if (strstr((const char *)receive_buf, "OK"))
        {
            retval = 0;
        }
        else
        {
            retval = 1;
        }
    }
    uart2_receiver_clear(receive_count);
    return retval;
}

// ===================== 保留：ESP8266复位 =====================
uint8_t esp8266_reset(void)
{
    uint8_t retval = 0;
    uint16_t count = 0;

    HAL_UART_Transmit(&huart2, (unsigned char *)"AT+RST\r\n", 8, 1000);
    while ((receive_start == 0) && (count < 2000))
    {
        count++;
        HAL_Delay(1);
    }
    if (count >= 2000)
    {
        retval = 1;
    }
    else
    {
        HAL_Delay(5000);
        if (strstr((const char *)receive_buf, "OK"))
        {
            retval = 0;
        }
        else
        {
            retval = 1;
        }
    }
    uart2_receiver_clear(receive_count);
    return retval;
}


void TCP_Send_Format(const char *format, ...)
{
    char send_buf[64];  
    va_list args;     
    
    
    va_start(args, format);
    vsprintf(send_buf, format, args);  
    va_end(args);
    

    HAL_UART_Transmit(&huart2, (uint8_t *)send_buf, strlen(send_buf), HAL_MAX_DELAY);
    HAL_UART_Transmit(&huart2, (uint8_t *)"\r\n", 2, HAL_MAX_DELAY);
}


void receive_tcp_data(uint8_t *buf)
{
    uint16_t recv_len = 0;
    uint8_t temp_byte;
    memset(buf, 0, 100);

    while(recv_len < 99)
    {
        if(HAL_UART_Receive(&huart2, &temp_byte, 1, 30000) == HAL_OK)
        {
            buf[recv_len++] = temp_byte;

            if(temp_byte == '\n') 
            {
                printf("检测到换行\r\n");
                recv_len = 0;
                break;
            }
        }
        else
        {
            // 关键：打印退出原因
            printf("【接收异常/连接断开】当前长度：%d\r\n", recv_len);
            break;
        }
    }
}
// ===================== 精简：ESP8266初始化（无MQTT） =====================
uint8_t esp8266_init(void)
{
    uint8_t retry_cnt;
    __HAL_UART_ENABLE_IT(&huart2, UART_IT_RXNE);

    // 1. 设置Station模式
    printf("1.SETTING STATION MODE\r\n");
    retry_cnt = 0;
    while (retry_cnt < 2)
    {
        if (esp8266_send_cmd((uint8_t *)"AT+CWMODE=1\r\n", strlen("AT+CWMODE=1\r\n"), "OK") == 0) break;
        retry_cnt++;
        HAL_Delay(200);
    }
    if (retry_cnt >= 2)
    {
        printf("Error: SET STATION MODE FAILED!\r\n");
        return 1;
    }

    // 2. 关闭回显
    printf("2.CLOSE ECHO\r\n");
    retry_cnt = 0;
    while (retry_cnt < 2)
    {
        if (esp8266_send_cmd((uint8_t *)"ATE0\r\n", strlen("ATE0\r\n"), "OK") == 0) break;
        retry_cnt++;
        HAL_Delay(200);
    }
    if (retry_cnt >= 2)
    {
        printf("Error: CLOSE ECHO FAILED!\r\n");
        return 1;
    }

    // 3. 关闭自动连接WiFi
    printf("3.DISABLE AUTO WIFI\r\n");
    retry_cnt = 0;
    while (retry_cnt < 2)
    {
        if (esp8266_send_cmd((uint8_t *)"AT+CWAUTOCONN=0\r\n", strlen("AT+CWAUTOCONN=0\r\n"), "OK") == 0) break;
        retry_cnt++;
        HAL_Delay(200);
    }
    if (retry_cnt >= 2)
    {
        printf("Error: DISABLE AUTO WIFI FAILED!\r\n");
        return 1;
    }

    // 4. 复位模块
    printf("4.RESET ESP8266\r\n");
    retry_cnt = 0;
    while (retry_cnt < 2)
    {
        if (esp8266_reset() == 0) break;
        retry_cnt++;
        HAL_Delay(200);
    }
    if (retry_cnt >= 2)
    {
        printf("Error: RESET FAILED!\r\n");
        return 1;
    }
    HAL_Delay(1000);

    // 5. 连接WiFi
    printf("5.CONNECT WIFI\r\n");
    retry_cnt = 0;
    while (retry_cnt < 2)
    {
        if (esp8266_config_network() == 0) break;
        retry_cnt++;
        HAL_Delay(200);
    }
    if (retry_cnt >= 2)
    {
        printf("Error: CONNECT WIFI FAILED!\r\n");
        return 1;
    }

    printf("ESP8266 INIT OK!!!\r\n");
    return 0;
}

// ===================== 保留：TCP透传初始化 =====================
uint8_t esp8266_tcp_transparent_init(void)
{
    uint8_t retry_cnt; 
    char tcp_cmd_buf[64];

    // 连接TCP服务器
    printf("2.CONNECT TCP SERVER\r\n");
    sprintf(tcp_cmd_buf, "AT+CIPSTART=\"TCP\",\"%s\",%s\r\n", TCP_SERVER_IP, TCP_SERVER_PORT);
    retry_cnt = 0;
    while (retry_cnt < 2)
    {
        if (esp8266_send_cmd((uint8_t *)tcp_cmd_buf, strlen(tcp_cmd_buf), "OK") == 0) break;
        retry_cnt++;
        HAL_Delay(500);
    }
    if (retry_cnt >= 2)
    {
        printf("Error: CONNECT TCP FAILED!\r\n");
        return 1;
    }

    // 开启透传模式
    printf("3.OPEN TRANSPARENT MODE\r\n");
    retry_cnt = 0;
    while (retry_cnt < 2)
    {
        if (esp8266_send_cmd((uint8_t *)"AT+CIPMODE=1\r\n", strlen("AT+CIPMODE=1\r\n"), "OK") == 0) break;
        retry_cnt++;
        HAL_Delay(200);
    }
    if (retry_cnt >= 2)
    {
        printf("Error: OPEN TRANS MODE FAILED!\r\n");
        return 1;
    }

    // 进入透传发送状态
    printf("4.ENTER TRANS MODE\r\n");
    retry_cnt = 0;
    while (retry_cnt < 2)
    {
        if (esp8266_send_cmd((uint8_t *)"AT+CIPSEND\r\n", strlen("AT+CIPSEND\r\n"), "OK") == 0) break;
        retry_cnt++;
        HAL_Delay(200);
    }
    if (retry_cnt >= 2)
    {
        printf("Error: ENTER TRANS FAILED!\r\n");
        return 1;
    }

    printf("TCP TRANSPARENT INIT OK!!!\r\n");
    return 0;
}

// ===================== 保留：TCP总初始化 =====================
uint8_t esp8266_tcp_connect_init(void)
{
    uint8_t retry_cnt;

    printf("8.INIT TCP TRANS MODE\r\n");
    retry_cnt = 0;
    while (retry_cnt < 2)
    {
        if (esp8266_tcp_transparent_init() == 0) break;
        retry_cnt++;
        HAL_Delay(500);
    }
    if (retry_cnt >= 2)
    {
        printf("Error: TCP INIT FAILED\r\n");
        return 1;
    }
  
    printf("ESP8266 TCP TRANS OK\r\n");
    return 0;
}
