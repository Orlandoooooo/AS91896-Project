/*
  Environmental Weather Station
  ------------------------------
  Reads temperature, humidity, pressure and light level from three
  I2C sensors, classifies the conditions into a simple weather type,
  and shows the results on a TFT screen. A single button short-press
  cycles between three screens; a long-press toggles the temperature
  unit between Celsius and Fahrenheit. The same data can also be
  queried over the Serial Monitor.
*/

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <Adafruit_LTR390.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

// ---------------------------------------------------------
// Pin numbers and timing constants (kept out of the code below
// so there are no unexplained "magic numbers")
// ---------------------------------------------------------
#define SDA_PIN 3
#define SCL_PIN 4
#define BUTTON_PIN 0
#define UPDATE_INTERVAL 2000 // how often sensors are re-read, in ms
#define LONG_PRESS_MS 600    // how long the button must be held to count as "long press"

Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;
Adafruit_LTR390 ltr;
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ---------------------------------------------------------
// State that needs to persist between loop() iterations
// ---------------------------------------------------------
int currentScreen = 0;
bool lastButtonState = HIGH;
bool useCelsius = true;
bool redrawScreen = true; // "dirty flag" - only redraw the TFT when something has actually changed

bool ahtFound = false;
bool bmpFound = false;
bool ltrFound = false;

unsigned long lastUpdate = 0;
unsigned long buttonPressTime = 0;
bool buttonHeld = false;

String serialCommand = "";

// Most recent sensor readings and derived weather info, cached here so
// both the display and the Serial commands can reuse the same values
// without re-reading the sensors every time
float temperature = -999;
float humidityValue = -1;
float pressure = -1;
float lux = -1;
String weatherType = "";
String weatherAdvice = "";

/*
  getWeatherType()
  Classifies the current conditions into a short weather label based
  on simple threshold rules across temperature, humidity, pressure
  and light level.
*/
String getWeatherType(float temp, float humidity, float pressureHpa, float lightLux)
{
    if (pressureHpa < 1000 && humidity > 80 && lightLux < 5000)
        return "Possible Rain";
    if (temp > 30 && lightLux > 8000)
        return "Hot";
    if (temp < 12 && lightLux < 5000)
        return "Cold";
    if (humidity > 75 && temp > 20)
        return "Humid";
    if (lightLux > 15000 && humidity < 65)
        return "Sunny";
    if (lightLux < 5000 && pressureHpa >= 1000)
        return "Cloudy";
    return "Pleasant";
}

/*
  getWeatherAdvice()
  Returns a short human-readable explanation matching a given
  weather type label.
*/
String getWeatherAdvice(String type)
{
    if (type == "Possible Rain")
        return "Low pressure, high humidity and low light suggest rain may be approaching.";
    if (type == "Hot")
        return "High temperature and bright conditions detected. Stay hydrated.";
    if (type == "Cold")
        return "Low temperature and low light detected. Wear warm clothing.";
    if (type == "Humid")
        return "Warm and humid conditions. It may feel uncomfortable.";
    if (type == "Sunny")
        return "Bright and clear conditions. Sun protection is recommended.";
    if (type == "Cloudy")
        return "Low light suggests cloudy or overcast conditions.";
    return "Weather conditions are stable and comfortable.";
}

/*
  getWeatherEmoji()
  Returns an emoji matching the weather type, for use in Serial
  Monitor output (the TFT's built-in font has no emoji glyphs, so
  the display uses drawWeatherIcon() instead - see further down).
*/
String getWeatherEmoji(String type)
{
    if (type == "Possible Rain")
        return "\xF0\x9F\x8C\xA7"; // 🌧
    if (type == "Hot")
        return "\xF0\x9F\x94\xA5"; // 🔥
    if (type == "Cold")
        return "\xE2\x9D\x84"; // ❄
    if (type == "Humid")
        return "\xF0\x9F\x92\xA7"; // 💧
    if (type == "Sunny")
        return "\xE2\x98\x80"; // ☀
    if (type == "Cloudy")
        return "\xE2\x98\x81"; // ☁
    return "\xF0\x9F\x99\x82"; // 🙂 (Pleasant)
}

/*
  getTempColor()
  Picks a display colour for the temperature reading, so the user
  gets an at-a-glance sense of "cold/mild/warm/hot" from colour alone.
*/
uint16_t getTempColor(float temp)
{
    if (temp < 10)
        return ST77XX_BLUE;
    if (temp < 20)
        return ST77XX_CYAN;
    if (temp < 28)
        return ST77XX_GREEN;
    return ST77XX_RED;
}

/*
  toDisplayTemp()
  Converts a Celsius reading to whichever unit the user currently
  has selected (useCelsius toggled by a long button press).
*/
float toDisplayTemp(float celsius)
{
    return useCelsius ? celsius : (celsius * 9.0 / 5.0 + 32.0);
}

/*
  tempUnit()
  Returns the unit letter ("C" or "F") matching the current display
  setting, for use next to any printed temperature.
*/
String tempUnit()
{
    return useCelsius ? "C" : "F";
}

/*
  drawWrapped()
  Draws a block of text on the TFT, automatically wrapping onto new
  lines so it fits within maxWidth pixels. Builds the text word by
  word rather than character by character, so words are never split
  across lines.
*/
void drawWrapped(String text, int x, int y, int maxWidth)
{
    int lineHeight = 10;
    String word = "";
    String line = "";

    for (int i = 0; i <= (int)text.length(); i++)
    {
        char c = (i < (int)text.length()) ? text[i] : ' ';

        if (c == ' ' || i == (int)text.length())
        {
            String test = line + word;

            // Rough width estimate: ~6px per character at text size 1
            if ((int)(test.length() * 6) > maxWidth && line.length() > 0)
            {
                tft.setCursor(x, y);
                tft.print(line);
                y += lineHeight;
                line = word + " ";
            }
            else
            {
                line = test + " ";
            }

            word = "";
        }
        else
        {
            word += c;
        }
    }

    if (line.length() > 0)
    {
        tft.setCursor(x, y);
        tft.print(line);
    }
}

/*
  drawButtonHint()
  Draws the small greyed-out hint text at the bottom of every screen
  telling the user what the button currently does.
*/
void drawButtonHint(String label)
{
    tft.setTextColor(0x4208);
    tft.setTextSize(1);
    tft.setCursor(10, 124);
    tft.print(label);
}

/*
  drawStatusLine()
  Draws one "Label: OK/FAIL" line on the Status screen, colouring
  the result green or red. Pulled out into its own method so the
  three sensor status lines don't need three copies of this logic.
*/
void drawStatusLine(int y, String label, bool ok)
{
    tft.setCursor(10, y);
    tft.setTextColor(ST77XX_WHITE);
    tft.print(label);
    tft.setTextColor(ok ? ST77XX_GREEN : ST77XX_RED);
    tft.print(ok ? " OK" : " FAIL");
}

/*
  drawScreen0()
  Draws the "Readings" screen: a large colour-coded temperature
  reading, plus humidity, pressure and light level underneath.
*/
void drawScreen0()
{
    tft.fillScreen(ST77XX_BLACK);

    tft.setTextColor(ST77XX_CYAN);
    tft.setTextSize(2);
    tft.setCursor(10, 5);
    tft.print("Readings");

    tft.drawFastHLine(0, 26, 240, 0x4208);

    if (temperature == -999)
    {
        tft.setTextColor(ST77XX_RED);
        tft.setTextSize(1);
        tft.setCursor(10, 35);
        tft.print("Temp: ERROR");
    }
    else
    {
        tft.setTextColor(getTempColor(temperature));
        tft.setTextSize(3);
        tft.setCursor(10, 32);
        tft.print(toDisplayTemp(temperature), 1);
        tft.print((char)247); // degree symbol
        tft.print(tempUnit());
    }

    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);

    tft.setCursor(10, 68);
    tft.print("Humidity:  ");
    if (humidityValue < 0)
        tft.print("ERROR");
    else
    {
        tft.print(humidityValue, 0);
        tft.print(" %");
    }

    tft.setCursor(10, 84);
    tft.print("Pressure:  ");
    if (pressure < 0)
        tft.print("ERROR");
    else
    {
        tft.print(pressure, 1);
        tft.print(" hPa");
    }

    tft.setCursor(10, 100);
    tft.print("Light:     ");
    if (lux < 0)
        tft.print("ERROR");
    else
    {
        tft.print(lux, 0);
        tft.print(" lux");
    }

    // Small reminder of what a long press does, over on the right
    tft.setTextColor(0x4208);
    tft.setTextSize(1);
    tft.setCursor(180, 68);
    tft.print("Hold:");
    tft.setCursor(180, 78);
    tft.print(useCelsius ? "=> F" : "=> C");

    drawButtonHint("[D0] Next: Weather >");
}

/*
  drawCloud()
  Draws a simple cloud shape (three overlapping circles plus a base)
  centred at (x, y). Reused by both the "Cloudy" and "Possible Rain"
  icons, since rain is just a cloud with drops added underneath.
*/
void drawCloud(int x, int y, uint16_t color)
{
    tft.fillCircle(x - 8, y, 8, color);
    tft.fillCircle(x + 8, y, 8, color);
    tft.fillCircle(x, y - 6, 10, color);
    tft.fillRect(x - 8, y, 16, 8, color);
}

/*
  drawSnowflake()
  Draws a simple six-pointed snowflake (three lines crossing through
  the centre point) for the "Cold" weather icon.
*/
void drawSnowflake(int centerX, int centerY, uint16_t color)
{
    for (int angle = 0; angle < 180; angle += 60)
    {
        float rad = angle * PI / 180.0;
        int dx = cos(rad) * 12;
        int dy = sin(rad) * 12;
        tft.drawLine(centerX - dx, centerY - dy, centerX + dx, centerY + dy, color);
    }
}

/*
  drawWeatherIcon()
  Draws a small vector icon representing the current weather type at
  the given centre point. Used instead of an emoji character because
  the TFT's built-in font has no emoji glyphs - this is the visual
  equivalent, built from basic shapes so it renders on any screen.
*/
void drawWeatherIcon(int centerX, int centerY, String type)
{
    if (type == "Sunny" || type == "Hot")
    {
        // Sun: filled circle with rays radiating outward
        uint16_t sunColor = (type == "Hot") ? ST77XX_RED : ST77XX_YELLOW;
        tft.fillCircle(centerX, centerY, 10, sunColor);

        for (int angle = 0; angle < 360; angle += 45)
        {
            float rad = angle * PI / 180.0;
            int x1 = centerX + cos(rad) * 14;
            int y1 = centerY + sin(rad) * 14;
            int x2 = centerX + cos(rad) * 20;
            int y2 = centerY + sin(rad) * 20;
            tft.drawLine(x1, y1, x2, y2, sunColor);
        }
    }
    else if (type == "Possible Rain")
    {
        drawCloud(centerX, centerY - 6, 0x7BEF); // grey cloud

        // Rain drops as short diagonal lines below the cloud
        for (int i = -8; i <= 8; i += 8)
        {
            tft.drawLine(centerX + i, centerY + 6, centerX + i - 3, centerY + 16, ST77XX_CYAN);
        }
    }
    else if (type == "Cold")
    {
        drawSnowflake(centerX, centerY, ST77XX_CYAN);
    }
    else if (type == "Humid")
    {
        drawCloud(centerX, centerY - 6, ST77XX_WHITE);
        tft.fillCircle(centerX, centerY + 12, 4, ST77XX_BLUE); // single droplet
    }
    else
    {
        // Cloudy or Pleasant: plain cloud
        drawCloud(centerX, centerY, ST77XX_WHITE);
    }
}

/*
  drawScreen1()
  Draws the "Weather" screen: a vector weather icon, the classified
  weather type in large text, and the matching advice wrapped
  underneath it.
*/
void drawScreen1()
{
    tft.fillScreen(ST77XX_BLACK);

    tft.setTextColor(ST77XX_GREEN);
    tft.setTextSize(2);
    tft.setCursor(10, 5);
    tft.print("Weather");

    tft.drawFastHLine(0, 26, 240, 0x4208);

    drawWeatherIcon(205, 42, weatherType);

    tft.setTextColor(ST77XX_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(10, 32);
    tft.print(weatherType);

    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    drawWrapped(weatherAdvice, 10, 62, 220);

    drawButtonHint("[D0] Next: Status >");
}

/*
  drawScreen2()
  Draws the "Status" screen: OK/FAIL for each of the three sensors,
  useful for diagnosing a wiring problem without a laptop attached.
*/
void drawScreen2()
{
    tft.fillScreen(ST77XX_BLACK);

    tft.setTextColor(ST77XX_YELLOW);
    tft.setTextSize(2);
    tft.setCursor(10, 5);
    tft.print("Status");

    tft.drawFastHLine(0, 26, 240, 0x4208);

    tft.setTextSize(1);

    drawStatusLine(40, "AHT20: ", ahtFound);
    drawStatusLine(60, "BMP280:", bmpFound);
    drawStatusLine(80, "LTR390:", ltrFound);

    drawButtonHint("[D0] Next: Readings >");
}

/*
  readSensors()
  Takes one fresh reading from each sensor, range-checks it against
  a physically plausible window, and stores the result (or an
  "invalid" sentinel value if the sensor is missing or out of range).
  Also recalculates the weather classification from the new readings.
*/
void readSensors()
{
    if (ahtFound)
    {
        sensors_event_t hEvent, tEvent;
        aht.getEvent(&hEvent, &tEvent);

        float t = tEvent.temperature;
        float h = hEvent.relative_humidity;

        // Boundary check: reject readings outside a sane physical range
        temperature = (t >= -40 && t <= 85) ? t : -999;
        humidityValue = (h >= 0 && h <= 100) ? h : -1;
    }

    if (bmpFound)
    {
        float p = bmp.readPressure() / 100.0;
        pressure = (p >= 300 && p <= 1200) ? p : -1;
    }

    if (ltrFound)
    {
        float l = ltr.readALS();
        lux = (l >= 0) ? l : -1;
    }

    weatherType = getWeatherType(temperature, humidityValue, pressure, lux);
    weatherAdvice = getWeatherAdvice(weatherType);
}

/*
  handleSerialCommand()
  Parses a single line typed into the Serial Monitor and prints the
  matching response, using the cached sensor/weather data so the
  reply is instant rather than waiting for the next sensor read.
*/
void handleSerialCommand(String command)
{
    command.trim();
    command.toLowerCase();

    if (command == "help")
    {
        Serial.println("Commands: help | readings | weather | status");
    }
    else if (command == "readings")
    {
        Serial.print("Temperature: ");
        Serial.print(temperature);
        Serial.println(" C");
        Serial.print("Humidity: ");
        Serial.print(humidityValue);
        Serial.println(" %");
        Serial.print("Pressure: ");
        Serial.print(pressure);
        Serial.println(" hPa");
        Serial.print("Light: ");
        Serial.print(lux);
        Serial.println(" lux");
    }
    else if (command == "weather")
    {
        Serial.print("Type: ");
        Serial.print(weatherType);
        Serial.print(" ");
        Serial.println(getWeatherEmoji(weatherType));
        Serial.print("Advice: ");
        Serial.println(weatherAdvice);
    }
    else if (command == "status")
    {
        Serial.print("AHT20: ");
        Serial.println(ahtFound ? "OK" : "FAIL");
        Serial.print("BMP280: ");
        Serial.println(bmpFound ? "OK" : "FAIL");
        Serial.print("LTR390: ");
        Serial.println(ltrFound ? "OK" : "FAIL");
    }
    else
    {
        Serial.println("Unknown command. Type 'help'.");
    }
}

/*
  setup()
  Initialises Serial, I2C, the button, the TFT display, and each of
  the three sensors. Shows an error screen listing which sensors
  failed to initialise, if any, then takes the first sensor reading.
*/
void setup()
{
    Serial.begin(115200);
    delay(1000);

    Wire.begin(SDA_PIN, SCL_PIN);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(TFT_BACKLITE, OUTPUT);
    digitalWrite(TFT_BACKLITE, HIGH);
    pinMode(TFT_I2C_POWER, OUTPUT);
    digitalWrite(TFT_I2C_POWER, HIGH);
    delay(100);

    tft.init(135, 240);
    tft.setRotation(3);
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(1);
    tft.setCursor(10, 10);
    tft.println("Starting...");

    // Try each sensor at its default (and, for BMP280, alternate) address
    ahtFound = aht.begin();
    bmpFound = bmp.begin(0x76);
    if (!bmpFound)
        bmpFound = bmp.begin(0x77);
    ltrFound = ltr.begin();

    if (ltrFound)
    {
        ltr.setMode(LTR390_MODE_ALS);
        ltr.setGain(LTR390_GAIN_3);
        ltr.setResolution(LTR390_RESOLUTION_16BIT);
    }

    if (bmpFound)
    {
        bmp.setSampling(
            Adafruit_BMP280::MODE_NORMAL,
            Adafruit_BMP280::SAMPLING_X2,
            Adafruit_BMP280::SAMPLING_X16,
            Adafruit_BMP280::FILTER_X16,
            Adafruit_BMP280::STANDBY_MS_500);
    }

    if (!ahtFound || !bmpFound || !ltrFound)
    {
        tft.fillScreen(ST77XX_BLACK);
        tft.setTextColor(ST77XX_RED);
        tft.setTextSize(2);
        tft.setCursor(10, 10);
        tft.println("Sensor Error!");
        tft.setTextColor(ST77XX_WHITE);
        tft.setTextSize(1);
        tft.setCursor(10, 40);
        tft.print("AHT20:  ");
        tft.setTextColor(ahtFound ? ST77XX_GREEN : ST77XX_RED);
        tft.println(ahtFound ? "OK" : "FAIL");
        tft.setTextColor(ST77XX_WHITE);
        tft.setCursor(10, 58);
        tft.print("BMP280: ");
        tft.setTextColor(bmpFound ? ST77XX_GREEN : ST77XX_RED);
        tft.println(bmpFound ? "OK" : "FAIL");
        tft.setTextColor(ST77XX_WHITE);
        tft.setCursor(10, 76);
        tft.print("LTR390: ");
        tft.setTextColor(ltrFound ? ST77XX_GREEN : ST77XX_RED);
        tft.println(ltrFound ? "OK" : "FAIL");
        delay(3000);
    }

    // Take an initial reading immediately so the first screen isn't blank
    readSensors();
    redrawScreen = true;
}

/*
  loop()
  Non-blocking main loop. Every cycle it: checks the button for a
  short press (change screen) or long press (toggle temp unit);
  re-reads the sensors once per UPDATE_INTERVAL; redraws the TFT
  only when something has actually changed (redrawScreen flag,
  rather than redrawing every cycle); and checks for Serial commands.
*/
void loop()
{
    // --- Button handling ---
    bool buttonState = digitalRead(BUTTON_PIN);

    // Button just pressed down: start timing for a possible long press
    if (lastButtonState == HIGH && buttonState == LOW)
    {
        buttonPressTime = millis();
        buttonHeld = false;
    }

    // Button has been held long enough: treat as a long press, toggle unit
    if (buttonState == LOW && !buttonHeld && (millis() - buttonPressTime >= LONG_PRESS_MS))
    {
        buttonHeld = true;
        useCelsius = !useCelsius;
        redrawScreen = true;
    }

    // Button released: if it wasn't a long press, treat it as a short
    // press and move to the next screen
    if (lastButtonState == LOW && buttonState == HIGH)
    {
        if (!buttonHeld)
        {
            currentScreen = (currentScreen + 1) % 3;
            redrawScreen = true;
        }
        delay(50); // brief debounce on release
    }

    lastButtonState = buttonState;

    // --- Periodic sensor refresh ---
    if (millis() - lastUpdate >= UPDATE_INTERVAL)
    {
        lastUpdate = millis();
        readSensors();
        redrawScreen = true;
    }

    // --- Redraw only if something changed, to avoid unnecessary flicker ---
    if (redrawScreen)
    {
        redrawScreen = false;
        if (currentScreen == 0)
            drawScreen0();
        else if (currentScreen == 1)
            drawScreen1();
        else
            drawScreen2();
    }

    // --- Serial command handling ---
    if (Serial.available())
    {
        serialCommand = Serial.readStringUntil('\n');
        handleSerialCommand(serialCommand);
    }
}