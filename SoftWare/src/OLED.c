#include "OLED.h"
#include "stm32f4xx.h"


#define SDA_RCC_CLK RCC_AHB1Periph_GPIOB
#define SDA_PORT    GPIOB    
#define SDA_PIN     GPIO_Pin_10

#define SCL_RCC_CLK RCC_AHB1Periph_GPIOB
#define SCL_PORT    GPIOB    
#define SCL_PIN     GPIO_Pin_10


void OLED_Init(void)
{
    
}

