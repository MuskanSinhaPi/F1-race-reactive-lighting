Great question — this gets into **how the display controller actually boots**.

### What #define TFT\_RST -1 means

In the **Adafruit ST7735 library**, the reset pin is optional.

When you write:

Plain textANTLR4BashCC#CSSCoffeeScriptCMakeDartDjangoDockerEJSErlangGitGoGraphQLGroovyHTMLJavaJavaScriptJSONJSXKotlinLaTeXLessLuaMakefileMarkdownMATLABMarkupObjective-CPerlPHPPowerShell.propertiesProtocol BuffersPythonRRubySass (Sass)Sass (Scss)SchemeSQLShellSwiftSVGTSXTypeScriptWebAssemblyYAMLXML`   #define TFT_RST -1   `

you are telling the library:

> "There is **no reset GPIO connected**, so don't try to control one."

Inside the library code, it basically does something like:

Plain textANTLR4BashCC#CSSCoffeeScriptCMakeDartDjangoDockerEJSErlangGitGoGraphQLGroovyHTMLJavaJavaScriptJSONJSXKotlinLaTeXLessLuaMakefileMarkdownMATLABMarkupObjective-CPerlPHPPowerShell.propertiesProtocol BuffersPythonRRubySass (Sass)Sass (Scss)SchemeSQLShellSwiftSVGTSXTypeScriptWebAssemblyYAMLXML`   if (rstPin != -1) {      digitalWrite(rstPin, LOW);      delay(10);      digitalWrite(rstPin, HIGH);  }   `

So when -1 is used:

*   The **software reset step is skipped**
    
*   The display relies on its **power-on reset instead**
    

### Why connecting RESET to 3.3V works

The **RESET pin on the display is active-LOW**.

Reset Pin StateMeaningLOWReset the display controllerHIGHNormal operation

So when you connect:

Plain textANTLR4BashCC#CSSCoffeeScriptCMakeDartDjangoDockerEJSErlangGitGoGraphQLGroovyHTMLJavaJavaScriptJSONJSXKotlinLaTeXLessLuaMakefileMarkdownMATLABMarkupObjective-CPerlPHPPowerShell.propertiesProtocol BuffersPythonRRubySass (Sass)Sass (Scss)SchemeSQLShellSwiftSVGTSXTypeScriptWebAssemblyYAMLXML`   RESET → 3.3V   `

the display is **held in the non-reset state**.

When the board powers up:

1.  Power rises from **0V → 3.3V**
    
2.  The controller internally resets
    
3.  RESET becomes HIGH
    
4.  The display starts normally
    

This is called a **power-on reset**.

Most display controllers like:

*   **ST7735**
    
*   **ILI9341**
    
*   **ST7789**
    

are designed to handle this automatically.

### Why libraries sometimes toggle RESET anyway

The manual reset is useful when:

*   the display **locks up**
    
*   you **reinitialize it without rebooting the MCU**
    
*   SPI communication gets corrupted
    

The reset sequence forces the controller into a known state.

But in many small projects it isn't necessary.

### Why your display still worked without a reset pulse

Because the **initialization commands sent over SPI** already configure everything:

Plain textANTLR4BashCC#CSSCoffeeScriptCMakeDartDjangoDockerEJSErlangGitGoGraphQLGroovyHTMLJavaJavaScriptJSONJSXKotlinLaTeXLessLuaMakefileMarkdownMATLABMarkupObjective-CPerlPHPPowerShell.propertiesProtocol BuffersPythonRRubySass (Sass)Sass (Scss)SchemeSQLShellSwiftSVGTSXTypeScriptWebAssemblyYAMLXML`   sleep exit  pixel format  frame rate  color mode  display on   `

So the sequence becomes:

Plain textANTLR4BashCC#CSSCoffeeScriptCMakeDartDjangoDockerEJSErlangGitGoGraphQLGroovyHTMLJavaJavaScriptJSONJSXKotlinLaTeXLessLuaMakefileMarkdownMATLABMarkupObjective-CPerlPHPPowerShell.propertiesProtocol BuffersPythonRRubySass (Sass)Sass (Scss)SchemeSQLShellSwiftSVGTSXTypeScriptWebAssemblyYAMLXML`   Power on  ↓  Display internal reset  ↓  ESP8266 sends initialization commands  ↓  Display works   `

### When you _would_ need a reset pin

You'd want a reset pin if:

*   the display **sometimes shows garbage after reboot**
    
*   you **reset the ESP8266 but not the display**
    
*   the display **shares SPI with other devices**
    
*   you want **very reliable startup**
    

### Practical rule used in many projects

For small SPI TFT displays:

Plain textANTLR4BashCC#CSSCoffeeScriptCMakeDartDjangoDockerEJSErlangGitGoGraphQLGroovyHTMLJavaJavaScriptJSONJSXKotlinLaTeXLessLuaMakefileMarkdownMATLABMarkupObjective-CPerlPHPPowerShell.propertiesProtocol BuffersPythonRRubySass (Sass)Sass (Scss)SchemeSQLShellSwiftSVGTSXTypeScriptWebAssemblyYAMLXML`   RESET → 3.3V  TFT_RST = -1   `

This:

*   saves **one GPIO**
    
*   simplifies wiring
    
*   works **~95% of the time**
    

Since you're building that **ESP8266 race telemetry display**, there's actually another optimization you might like:your screen updates will be **~6–8× faster** if you switch from **Adafruit\_ST7735** to **TFT\_eSPI**. I can show you how to configure it for your exact pin layout if you want.
