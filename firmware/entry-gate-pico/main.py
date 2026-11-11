"""
Smart Parking Infrastructure using Cyber-Physical Systems
Module : Entry Gate Control
Target : Raspberry Pi Pico (RP2040), MicroPython

- Ultrasonic sensor 1 (TRIG GP0 / ECHO GP1) detects a vehicle at the gate
- MFRC522 RFID reader (SPI0) authenticates the driver's card
- SG90 servo (GP4) raises / lowers the barrier
- SSD1306 OLED (I2C0, SDA GP8 / SCL GP9) shows STOP / SCAN / GO / DENIED
- Traffic LEDs (GP14 red, GP15 yellow, GP16 green) guide the driver;
  ultrasonic sensor 2 (TRIG GP2 / ECHO GP3) watches the lane past the gate

Finite state machine:  STOP --vehicle--> WAIT(SCAN) --valid RFID--> GO --vehicle clears--> STOP

Requires on the Pico: ssd1306.py and mfrc522.py driver modules.
"""
from machine import Pin, PWM, I2C, SPI
import time
import ssd1306
from mfrc522 import MFRC522

# ---------------- STATES ----------------
STOP = 0
WAIT = 1
GO = 2

state = STOP
last_state = -1

# ---------------- LED STATES ----------------
LED_RED = 0
LED_YELLOW = 1
LED_GREEN = 2

current_led = LED_RED
previous_led = LED_RED

# ---------------- PINS (UNCHANGED) ----------------
trig1 = Pin(0, Pin.OUT)
echo1 = Pin(1, Pin.IN)

trig2 = Pin(2, Pin.OUT)
echo2 = Pin(3, Pin.IN)

servo = PWM(Pin(4))
servo.freq(50)

red = Pin(14, Pin.OUT)
yellow = Pin(15, Pin.OUT)
green = Pin(16, Pin.OUT)

# ---------------- RFID ----------------
spi = SPI(0,
          baudrate=1000000,
          polarity=0,
          phase=0,
          sck=Pin(18),
          mosi=Pin(19),
          miso=Pin(20))

rfid = MFRC522(spi=spi, cs=Pin(17), rst=Pin(21))

AUTHORIZED_CARDS = {
    (80, 137, 84, 85, 216),
    (130, 240, 23, 2, 103)
}

last_uid = None

# ---------------- OLED ----------------
i2c = I2C(0, scl=Pin(9), sda=Pin(8), freq=400000)
oled = ssd1306.SSD1306_I2C(128, 64, i2c)

def show_status(text):
    oled.fill(0)
    oled.text(text, 40, 30)
    oled.show()

# ---------------- SERVO ----------------
def servo_angle(angle):
    min_us = 500
    max_us = 2500
    us = min_us + (angle / 180) * (max_us - min_us)
    duty = int(us * 65535 / 20000)
    servo.duty_u16(duty)

# ---------------- ULTRASONIC ----------------
def measure_distance(trig, echo):
    readings = []
    for _ in range(5):
        trig.low()
        time.sleep_us(2)
        trig.high()
        time.sleep_us(10)
        trig.low()

        timeout = time.ticks_us()
        while echo.value() == 0:
            if time.ticks_diff(time.ticks_us(), timeout) > 30000:
                return 999

        start = time.ticks_us()
        while echo.value() == 1:
            if time.ticks_diff(time.ticks_us(), start) > 30000:
                return 999

        end = time.ticks_us()
        duration = time.ticks_diff(end, start)
        distance = (duration * 0.0343) / 2
        readings.append(distance)
        time.sleep_ms(10)

    readings.sort()
    return sum(readings[1:4]) / 3

# ---------------- THRESHOLDS ----------------
CAR_DETECT = 10
CAR_CLEAR = 15

# ---------------- INITIAL ----------------
servo_angle(0)
show_status("STOP")

red.value(1)
yellow.value(0)
green.value(0)

arm_open = False

print("System Ready")

# ---------------- MAIN LOOP ----------------
while True:

    d1 = measure_distance(trig1, echo1)
    d2 = measure_distance(trig2, echo2)

    # ---------------- ENTRY LOGIC ----------------
    if state == STOP:
        if d1 < CAR_DETECT:
            state = WAIT

    elif state == WAIT:

        if d1 > CAR_CLEAR:
            state = STOP

        # ---- RFID Scan ----
        stat, _ = rfid.request(rfid.REQIDL)
        if stat == rfid.OK:
            stat, uid = rfid.anticoll()

            if stat == rfid.OK:
                uid_tuple = tuple(uid)

                if uid_tuple != last_uid:

                    if uid_tuple in AUTHORIZED_CARDS:
                        print("Access Granted:", uid_tuple)
                        state = GO
                        servo_angle(90)
                        arm_open = True
                    else:
                        print("Access Denied:", uid_tuple)
                        show_status("DENIED")
                        time.sleep(2)

                    last_uid = uid_tuple

    elif state == GO:

        # ---- Detect YELLOW → RED transition ----
        if arm_open and previous_led == LED_YELLOW and current_led == LED_RED:
            print("Yellow → Red transition detected. Immediate closure.")
            servo_angle(0)
            arm_open = False
            state = STOP

        # ---- Normal closing after car leaves ----
        elif arm_open and d1 > CAR_CLEAR:
            time.sleep(2)
            servo_angle(0)
            arm_open = False

            red.value(0)
            yellow.value(0)
            green.value(1)
            time.sleep(2)

            green.value(0)
            red.value(1)
            current_led = LED_RED
            state = STOP

    # ---------------- OLED UPDATE ----------------
    if state != last_state:
        if state == STOP:
            show_status("STOP")
        elif state == WAIT:
            show_status("SCAN")
        elif state == GO:
            show_status("GO")
        last_state = state

    # ---------------- EXIT LED LOGIC (UNCHANGED) ----------------
    previous_led = current_led

    if d2 > CAR_CLEAR:
        red.value(1)
        yellow.value(0)
        green.value(0)
        current_led = LED_RED

    elif d2 < CAR_DETECT:

        if state == STOP:
            red.value(0)
            yellow.value(0)
            green.value(1)
            current_led = LED_GREEN
        else:
            yellow.value(1)
            red.value(0)
            green.value(0)
            current_led = LED_YELLOW

    if state != WAIT:
        last_uid = None

    time.sleep(0.1)
