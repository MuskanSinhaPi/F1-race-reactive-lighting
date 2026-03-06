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

sleep exit

pixel format

frame rate

color mode

display on

So the sequence becomes:

Power on

↓

Display internal reset

↓

ESP8266 sends initialization commands

↓

Display works

### When you *would* need a reset pin

You'd want a reset pin if:

-   the display **sometimes shows garbage after reboot**

-   you **reset the ESP8266 but not the display**

-   the display **shares SPI with other devices**

-   you want **very reliable startup**
