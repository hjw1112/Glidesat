# test code using micropython for UART bridge.

import machine
import sys


en_pin = machine.Pin(10, machine.Pin.OUT)
en_pin.value(1)

led = machine.Pin(2, machine.Pin.OUT)
led.value(1)

led = machine.Pin(3, machine.Pin.OUT)
led.value(1)




uart = machine.UART(0, 115200, tx=0, rx=1)
print("UART bridge running")

while True:
    if uart.any():
        data = uart.read()
        sys.stdout.write(data.decode('utf-8', errors='ignore'))