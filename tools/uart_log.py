"""抓启动日志，但绝不碰 DTR —— 这块板的 USB 串口 DTR 接 IO0，
拉低后再复位就会掉进下载模式。只用 RTS 复位 EN。
用法: python tools/uart_log.py [COM3] [秒数]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM3"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.1
s.dtr = False
s.rts = False
s.open()

s.rts = True
time.sleep(0.2)
s.rts = False

t0 = time.time()
while time.time() - t0 < secs:
    sys.stdout.write(s.read(4096).decode("utf-8", "replace"))
    sys.stdout.flush()
s.close()
