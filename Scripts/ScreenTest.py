import os, sys
import keyboard
import asyncio
from evdev import InputDevice, categorize, ecodes

XSIZE = 512
YSIZE = 64

# Initial position of the square, middle of the screen
xpos = XSIZE / 2
ypos = YSIZE / 2

def checkRemoveCursor():
    # Check for cursor blinking
    stream = os.popen("cat /sys/class/graphics/fbcon/cursor_blink")
    cursor = stream.read()

    # If it is, turn it off
    if "0" not in cursor:
        print("Making cursor invisible.")
        os.popen("echo 0 > /sys/class/graphics/fbcon/cursor_blink")
        os.popen("clear")

def updateFramebuffer():
    global xpos
    global ypos

    # Open the framebuffer and write to it
    print("Writing to frame buffer...")
    # print(f"Drawing squre at ({xpos}, {ypos})")

    with open("/dev/fb0", 'w') as fb:
        print("Opening fb0 for writing")
        for y in range(0, YSIZE):
            for x in range(0, XSIZE):
                if ((ypos - 5) < y < (ypos + 5)) and ((xpos - 10) < x < (xpos + 10)):
                    # print(f"Drawing squre at ({xpos}, {ypos})")
                    fb.write("\x0F")
                else:
                    fb.write("\x00")

def inputHelper(dev):
    global xpos
    global ypos

    for ev in dev.async_read_loop():
        # print(categorize(ev))

        if ev.type == ecodes.EV_KEY and (ev.value == 1 or ev.value == 2):
            print(ev.value)

            if ev.code == 103: # Up
                ypos = ypos - 5
            elif ev.code == 108: # Down
                ypos = ypos + 5
            elif ev.code == 106: # Right
                xpos = xpos + 10
            elif ev.code == 105: # Left
                xpos = xpos - 10

            updateFramebuffer()

def setup():
    dev = InputDevice("/dev/input/event2")
    inputHelper(dev)

def main():
    checkRemoveCursor()
    updateFramebuffer()
    setup()

if __name__ == "__main__":
    main()
