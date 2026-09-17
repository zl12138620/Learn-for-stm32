# PB8/PB9 引脚实际电平高频采样
# IDR = 0x40020410, bit8=PB8 bit9=PB9
# 目标: 每 50ms 采一次, 连续 20 次, 看 IDR 是否在 0x300 和 0x000 之间跳变

init
halt

for {set i 0} {$i < 20} {incr i} {
    resume
    sleep 50
    halt
    mdw 0x40020410
}

resume
exit
