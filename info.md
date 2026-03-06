# Great question — this gets into **how the display controller actually boots**.

### What `#define TFT_RST -1` means

In the **Adafruit ST7735 library**, the reset pin is optional.

When you write:

```c
#define TFT_RST -1
```

you are telling the library:

> "There is **no reset GPIO connected**, so don't try to control one."

Inside the library code, it basically does something like:

```c
if (rstPin != -1) {
    digitalWrite(rstPin, LOW);
    delay(10);
    digitalWrite(rstPin, HIGH);
}
```

So when `-1` is used:
- The **software reset step is skipped**
- The display relies on its **power-on reset instead**

### Why connecting RESET to 3.3V works

The **RESET pin on the display is active-LOW**.

| Reset Pin State | Meaning |
|------------------|---------|
| LOW              | Reset the display controller |
| HIGH             | Normal operation |

So when you connect:

```plaintext
RESET → 3.3V
```
the display is **held in the non-reset state**.

When the board powers up:
1. Power rises from **0V → 3.3V**
2. The controller internally resets
3. RESET becomes HIGH
4. The display starts normally

This is called a **power-on reset**.

Most display controllers like:
- **ST7735**
- **ILI9341**
- **ST7789**
designed to handle this automatically.

### Why libraries sometimes toggle RESET anyway
The manual reset is useful when:
- the display **locks up**
- you **reinitialize it without rebooting the MCU**
- SPI communication gets corrupted 
the reset sequence forces the controller into a known state.
But in many small projects, it isn't necessary.

### Why your display still worked without a reset pulse
Because the **initialization commands sent over SPI** already configure everything:

```plaintext
sleep exit pixel format frame rate color mode display on \
'the sequence of commands sent during initialization ensures proper startup even without an explicit reset pulse.
```
e.g., power on → internal reset → ESP8266 sends initialization commands → display works.
 
defaults are set during these commands that ensure proper operation.
display will typically initialize correctly without an external reset if these commands are properly sent after power-up.
'this sequence makes sure your display works even if you skip manual hardware resets.'
during startup, most controllers rely on their internal power-on reset and configuration commands rather than an external hardware reset signal.
'this simplifies wiring and reduces complexity in small projects.'
p>
the sequence becomes:
p>
p> Power on → Internal Reset → Initialization Commands Sent by ESP8266 → Display Works 
p>
p### When you _would_ need a reset pin?
p-
the display **sometimes shows garbage after reboot**,
p-
you want a more reliable startup,
p-
you share SPI with other devices,
and/or,
you want to ensure consistent behavior across reboots or resets.
'this helps especially in debugging or critical applications where stability matters.'
d>
b> You'd want a reset pin if:
b>- The display **sometimes shows garbage after reboot**
b>- You **reset the ESP8266 but not the display**
b>- The display shares SPI with other devices,
b>- You want very reliable startup.'
here's a practical rule used in many projects for small SPI TFT displays:
h>
define `TFT_RST = -1` and connect `RESET` directly to `3.3V`. This approach saves GPIO pins, simplifies wiring, and works about 95% of the time.
h>
since you're building that *ESP8266 race telemetry* display, there's another optimization: your screen updates will be approximately *6–8× faster* if you switch from *Adafruit_ST7735* to *TFT_eSPI*. I can help you configure it for your exact pin layout if you're interested.
