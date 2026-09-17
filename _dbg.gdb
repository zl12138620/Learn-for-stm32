set pagination off
set confirm off
set print pretty off

target extended-remote localhost:3333

echo \n\n########## 1. 目标状态 ##########\n
monitor halt

echo \n########## 2. 当前 PC / SP / LR ##########\n
info registers pc sp lr xpsr

echo \n########## 3. 当前 PC 处的反汇编 ##########\n
x/4i $pc

echo \n########## 4. 调用栈 ##########\n
bt

echo \n\n########## 5. GPIOB 寄存器 (基址 0x40020400) ##########\n
echo --- MODER  (模式, 每脚2位: 00=输入 01=输出 10=复用 11=模拟)\n
x/1xw 0x40020400
echo --- OTYPER (0=推挽 1=开漏)\n
x/1xw 0x40020404
echo --- OSPEEDR\n
x/1xw 0x40020408
echo --- PUPDR  (00=无 01=上拉 10=下拉)\n
x/1xw 0x4002040C
echo --- IDR    (实际引脚电平)\n
x/1xw 0x40020410
echo --- ODR    (输出数据寄存器)\n
x/1xw 0x40020414
echo --- AFR[1] (PB8~PB15 复用选择)\n
x/1xw 0x40020424

echo \n########## 6. RCC 时钟使能 ##########\n
echo --- AHB1ENR (bit1 = GPIOBEN)\n
x/1xw 0x40023830
echo --- APB1ENR (bit21 = I2C1EN)\n
x/1xw 0x40023840

echo \n########## 7. 关键变量 ##########\n
echo --- SystemCoreClock\n
p/x SystemCoreClock

echo \n########## 8. 全部核心寄存器 ##########\n
info registers

echo \n\n########## 完成 ##########\n
