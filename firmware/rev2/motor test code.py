# test code using micropython to verify motor circuit functionality.

import machine

en_pin = machine.Pin(10, machine.Pin.OUT)
en_pin.value(1)

led = machine.Pin(2, machine.Pin.OUT)
led.value(1)

led = machine.Pin(3, machine.Pin.OUT)
led.value(1)

"""
motor and encoder test
RP2040, DRV8838DSGR

pin map
  ph(dir)       - GPIO14
  en(pwm)       - GPIO13
  sleep         - GPIO28
  enc a         - GPIO11
  enc b         - GPIO12
"""

from machine import Pin, PWM
import time

# pin set
ph    = Pin(14, Pin.OUT)                        # dir
en    = PWM(Pin(13), freq=20_000, duty_u16=0)   # pwm
sleep = Pin(28, Pin.OUT)                        # en

enc_a = Pin(11, Pin.IN, Pin.PULL_UP)            # enc a
enc_b = Pin(12, Pin.IN, Pin.PULL_UP)            # enc b

sleep.value(1)


# encoder counter                                                     #

position = 0

def encoder_isr(pin):
    global position
    if enc_b.value():
        position += 1
    else:
        position -= 1

enc_a.irq(trigger=Pin.IRQ_RISING, handler=encoder_isr)


def motor_forward(speed):
    """speed:0-100"""
    ph.value(0)
    en.duty_u16(int(speed / 100 * 65535))

def motor_reverse(speed):
    """speed 0-100"""
    ph.value(1)
    en.duty_u16(int(speed / 100 * 65535))

def motor_brake():
    en.duty_u16(0)

def motor_coast():
    en.duty_u16(0)
    sleep.value(0)

def motor_wake():
    sleep.value(1)

# test sequence

print("=" * 40)
print("DRV8838 + encoder")
print("=" * 40)

# 1 - forward increasing speed
print("\n1. forward 25% → 100%")
position = 0
for speed in [25, 50, 75, 100]:
    motor_forward(speed)
    time.sleep(1)
    print(f"speed {speed:3d}%, encoder count: {position}")

motor_brake()
time.sleep(0.5)

# 2 - reverse increasing speed
print("\n2 reverse ramp 25% → 100%")
position = 0
for speed in [25, 50, 75, 100]:
    motor_reverse(speed)
    time.sleep(1)
    print(f"speed {speed:3d}%, encoder count: {position}")

motor_brake()
time.sleep(0.5)

# 3 - brake, coast
print("\n3 brake test")
motor_forward(75)
time.sleep(1)
motor_brake()
print("brake applied, quick stop")
time.sleep(1)

print("\n4 coast test")
motor_forward(75)
time.sleep(1)
motor_coast()
print("coasting, spin down slowly")
time.sleep(2)
motor_wake()

# 4 - encoder acc 
print("\n5 encoder accuracy - forward 3 seconds")
position = 0
motor_forward(50)
time.sleep(3)
motor_brake()
print(f"encoder count after 3s at 50%: {position} ticks")

print("\n6 - encoder accuracy — reverse 3 seconds")
position = 0
motor_reverse(50)
time.sleep(3)
motor_brake()
print(f"encoder count after 3s at 50%: {position} ticks")

# --- Done ---
print("\n" + "=" * 40)
print("complete")
print("=" * 40)
en.deinit()