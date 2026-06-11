#include "bootlader.h"
#include "App_bootloader.h"

uint8_t uart_rec_buff[BOOTLOADER_UART_REC_BUFF_LEN] = {0};
uint16_t uart_rec_len = 0;
uint16_t uart_rec_full_len = 0;

uint32_t flash_write_offset = 0;

uint32_t last_rec_time = 0;

uint8_t last_byte_flag = 0;
uint8_t last_byte = 0;

uint8_t flag;

static void Int_flash_erase(void)
{
    uint8_t is_erase = 0;
    uint32_t page_addr = 0;
    for (uint16_t i = 0; i < uart_rec_len; i++)
    {
        uint8_t data = *(volatile uint8_t *)(APP_START_ADDR + i + flash_write_offset);
        if (data != 0xff)
        {
            // printf("erase:%d,%d,%c", i, flash_write_offset, data);
            is_erase = 1;
            page_addr = (APP_START_ADDR + i + flash_write_offset) - (APP_START_ADDR + i + flash_write_offset) % FLASH_PAGE_SIZE;
            break;
        }
    }

    if (is_erase)
    {
        FLASH_EraseInitTypeDef erase_init;
        erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
        erase_init.Banks = FLASH_BANK_1;
        erase_init.PageAddress = page_addr;
        erase_init.NbPages = 1;
        uint32_t page_error = 0;
        HAL_FLASHEx_Erase(&erase_init, &page_error);
    }
}

static void Int_flash_write_with_last(void)
{
    for (uint16_t i = 0; i < uart_rec_len; i += 2)
    {
        uint32_t flash_addr = APP_START_ADDR + i + flash_write_offset;
        uint16_t data16;
        if (i == 0)
        {
           data16 = last_byte | (uart_rec_buff[i] << 8);
        }
        else
        {
            data16 = uart_rec_buff[i - 1] | (uart_rec_buff[i] << 8);
        }
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr, data16);
    }
}

static void Int_flash_write_no_last(void)
{
    
    for (uint16_t i = 0; i < uart_rec_len; i += 2)
    {
        uint32_t flash_addr = APP_START_ADDR + i + flash_write_offset;
        uint16_t data16;
        if (i + 1 < uart_rec_len)
        {
            data16 = ((uint16_t)uart_rec_buff[i+1] << 8) | uart_rec_buff[i];
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, flash_addr, data16);
        }
    }
}

static void Int_flash_write_halfword(void)
{
    
    if ((uart_rec_len + last_byte_flag) % 2 == 0)
    {
        if (last_byte_flag)
        {            
            Int_flash_write_with_last();           
            flash_write_offset += uart_rec_len + 1;
        }
        else
        {            
            Int_flash_write_no_last();            
            flash_write_offset += uart_rec_len;
        }
        last_byte_flag = 0;
    }
    
    else
    {
        if (last_byte_flag)
        {            
            Int_flash_write_with_last();           
            last_byte = uart_rec_buff[uart_rec_len - 1];           
            flash_write_offset += uart_rec_len;
        }
        else
        {            
            Int_flash_write_no_last();
            last_byte = uart_rec_buff[uart_rec_len - 1];            
            flash_write_offset += uart_rec_len - 1;
        }
        last_byte_flag = 1;
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{

    if (huart->Instance == USART1)
    {
        uart_rec_len = Size;
        uart_rec_full_len += uart_rec_len;
        //printf("%d", uart_rec_full_len);
    
        HAL_FLASH_Unlock();
        Int_flash_erase();
        Int_flash_write_halfword(); 
        HAL_FLASH_Lock();
				//printf("ok");
       // memset(uart_rec_buff, 0, BOOTLOADER_UART_REC_BUFF_LEN);
        __HAL_UART_CLEAR_OREFLAG(&huart1);
        __HAL_UART_CLEAR_IDLEFLAG(&huart1);
        HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rec_buff, BOOTLOADER_UART_REC_BUFF_LEN);
    }
}

void Int_bootloader_receive_app(void)
{
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);
    HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rec_buff, BOOTLOADER_UART_REC_BUFF_LEN);
}

uint8_t bootloader_jump_to_app(void)
{
    typedef void (*pFunc)(void);

    uint32_t app_stack_ptr = *(volatile uint32_t *)(APP_START_ADDR);
    uint32_t app_reset_handle = *(volatile uint32_t *)(APP_START_ADDR + 4);

    if ((app_stack_ptr & 0xffff0000) != STACK_ADDR)
    {
        printf("stack addr error\n");
        return 1;
    }

    
    if (app_reset_handle < APP_START_ADDR || app_reset_handle > APP_END_ADDR)
    {
        printf("reset handle error\n");
        return 1;
    }

   
    NVIC_DisableIRQ(USART1_IRQn);
    
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    HAL_DeInit();
    __disable_irq();

    __set_MSP(app_stack_ptr);    
    SCB->VTOR = APP_START_ADDR;
    pFunc jump_to_app = (pFunc)app_reset_handle;
    jump_to_app();

    return 0;
}

void Int_bootloader_erase_flash(uint32_t page_addr, uint16_t pages)
{
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase_init;
    erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
    erase_init.Banks = FLASH_BANK_1;
    erase_init.PageAddress = page_addr;
    erase_init.NbPages = pages;
    uint32_t page_error = 0;
    HAL_FLASHEx_Erase(&erase_init, &page_error);
    HAL_FLASH_Lock();
}
/*
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
	if (GPIO_Pin == GPIO_PIN_3)
    {
        //flag = 1;
    }
}
*/
