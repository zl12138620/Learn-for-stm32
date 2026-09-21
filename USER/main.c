/**
  ******************************************************************************
  * @file    USER/main.c
  * @brief   程序入口 —— 开机初始化清单 + 一个空转的超级循环
  *
  *          这个文件刻意保持得很短: **看它一眼就知道这块板子跑了些什么**。
  *          具体的活都在 SoftWare/ 下按领域分好了:
  *
  *            system/   Tick(毫秒时基)  Led(心跳)  Ring_buffer(环形缓冲)
  *            comms/    Usart(调试串口, 含接收中断)
  *            display/  Lcd(ST7735S 屏)  Menu(三个界面的绘制)  字模
  *            camera/   OV7670(带 FIFO 的摄像头)
  *            motion/   Servo(SG90)  Encoder(旋转编码器)
  *            app/      App(界面状态机 + 串口回显 + 开机横幅)
  *
  *          加新外设的流程: 新模块放进对应领域 -> 在下面初始化清单里加一行
  *          -> 需要界面就在 app/App.c 的菜单里加一项。
  ******************************************************************************
  */

#include "main.h"

int main(void)
{
    /* ---- 0. NVIC 优先级分组: 必须放在**任何 NVIC_Init / 中断使能之前** ----
       ⚠ 这一步以前漏了, 而且后果极其隐蔽:
         FreeRTOS 要求 4 位优先级全给"抢占优先级"(PriorityGroup_4)。
         而 StdPeriph 的 NVIC_Init() 是拿 AIRCR.PRIGROUP 现算寄存器的 ——
         PRIGROUP 保持复位值时算出来是 0x00, 也就是说 Encoder.c 里写的
         "抢占优先级 4 / 5" **一个字节都没写进去**, 三个中断实际同优先级。
         不设这一条, 后面那些优先级数字全是白写。 */
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_4);

    /* ---- 1. 先把"能报信"的弄好 ----
       串口和 LED 放在最前面: 后面任何一个外设初始化卡住, 至少还知道
       固件跑到了哪一步。串口能出横幅、灯会闪, 排障就有抓手。 */
    Led_Init();
    Usart_Init(9600);

    /* ---- 2. 屏幕 ----
       1.8 寸 SPI TFT (ST7735S 128x160):
         SCK=PB13  SDA=PB15  CS=PB12  A0=PB10  RESET=PB11  (硬件 SPI2 + DMA) */
    LCD_Init();

    /* ---- 3. 外设 ----
       编码器的按键采样挂在 FreeRTOS 的 tick 钩子上(见 Tick.c), 而 tick 要等
       vTaskStartScheduler() 才开始跑 —— 所以这里只要把引脚配好就行。 */
    OV7670_Init();      /* PE0~PE15 + PB0/PB1, 内部要等约 200ms */
    Servo_Init();       /* PA1 + TIM5_CH2, 上电回中位 90° */
    Encoder_Init();     /* A=PB6 B=PB5 SW=PB7 + EXTI6 + TIM7 消抖 */

    /* ---- 4. 应用层: 建任务(此时还没开始调度) ---- */
    App_Init();

    /* ---- 5. 启动调度器 —— 这个函数**不返回** ----
       SysTick 从这一刻起归 FreeRTOS, 所有任务开始跑。
       下面的 while 只有一种情况会执行到: 内核堆连空闲任务都建不起来。 */
    vTaskStartScheduler();

    while (1)
    {
    }
}
