# Weather Station Decoder

Turn your 433 MHz weather station into a fully integrated Home Assistant sensor.

An ESP32 with a CC1101 radio that listens on 433.92 MHz and decodes the driveway
alarm and the weather station. It uses [rtl_433_ESP](https://github.com/NorthernMan54/rtl_433_ESP),
the ESP32 version of rtl_433, which recognizes a few hundred 433 MHz devices on its
own: Acurite, Ambient Weather, La Crosse and Fine Offset weather stations, EV1527-style
driveway alarms and remotes, door sensors and more. You don't need to set a brand or
model first.

## What it does

- **Web page** at `http://rf433.local` (or the ESP32's IP address) that updates every
  3 seconds:
  - **Weather and sensors**: temperature, humidity, wind, rain, pressure and so on,
    shown in °F, mph and inches.
  - **Alarms and remotes**: devices that only send a code, like the driveway alarm. When
    one goes off, the page flashes a red **ALERT** banner for 30 seconds.
- **Blue LED** on the ESP32 stays on for 30 seconds after an alarm.
- **Serial monitor** prints every decoded message.
- **Home Assistant** (optional): each device shows up on its own through MQTT
  discovery. Weather readings become sensors, and alarms become on/off motion
  sensors you can use in automations.

## Parts

- ESP32-WROOM-32 dev board
- CC1101 433 MHz module with SMA antenna (DWEII)
- Wireless driveway alarm, 58 melodies, 500 ft range ([Amazon B0GTPRM7R1](https://www.amazon.com/dp/B0GTPRM7R1))

Alarms like this one send a 24-bit EV1527 code. rtl_433 decodes those as
`Generic-Remote`, but only at the timing it expects, so the program also has its own
backup decoder that reads the code at any transmitter speed from about 200 to 800 µs
per pulse. Either way the alarm shows up as `Generic-Remote-<number>`.

- Sainlogic SA68 12-in-1 weather station ([Amazon B0H4GFXGYS](https://www.amazon.com/dp/B0H4GFXGYS))

Sainlogic uses the same outdoor sensor for the SA6, SA8 and SA68, and rtl_433 decodes
it as `Sainlogic-SA8` (433.92 MHz): temperature, humidity, wind speed, gust,
direction, rain total and battery. One search result claimed the SA68 transmits at
915 MHz instead; Sainlogic's own pages don't say. If the station never shows up on the
web page while the console is getting readings, it's probably a 915 MHz unit, and it
needs a 915 MHz CC1101 as a second radio (the library supports two at once).

## Wiring

Open `WIRING.html` in a browser for the diagram and a checklist.

| CC1101 | ESP32 |
|---|---|
| VCC | 3V3 (not 5V) |
| GND | GND |
| MOSI / SI | D23 |
| SCK / SCLK | D18 |
| MISO / SO | D19 |
| CSN / CS | D5 |
| GDO0 | D13 |
| GDO2 | D4 |

## ESP32-S3 version

The `esp32-s3` environment in `platformio.ini` is set up for an ESP32-S3-N16R8 board
(16 MB flash, 8 MB PSRAM) and is the one tested end to end. Build it with
`pio run -e esp32-s3 -t upload`. Wiring for the S3:

| CC1101 | ESP32-S3 |
|---|---|
| VCC | 3V3 (not 5V) |
| GND | GND |
| CSN / CS | GPIO10 |
| MOSI / SI | GPIO11 |
| SCK | GPIO12 |
| MISO / SO | GPIO13 |
| GDO0 | GPIO4 |
| GDO2 | GPIO5 |

Notes from getting it running:

- **Keep the CC1101 and its antenna at least 8 inches from the ESP32**, antenna pointing
  straight up. Next to the ESP32 the radio hears its electrical noise instead of the
  weather station. Long jumper wires or a small box for the radio fix this.
- **Some CC1101 clone modules report chip version 0x07**, which RadioLib rejects with
  "chip not found" (error -2). `patch_radiolib.py` runs before the build and adds 0x07 to
  the versions RadioLib accepts.
- **Uploading over the S3's native USB port:** if the upload can't connect, hold BOOT,
  tap RESET, release BOOT and try again. On a Mac, approve the "allow accessory" prompt.
  If the board then stays in upload mode, unplug it and plug it back in without holding
  any button.
- **Don't put GPIO 35, 36 or 37 to use**: the 8 MB PSRAM takes them.

## Setup

1. Install [PlatformIO](https://platformio.org/).
2. Copy `src/secrets.example.h` to `src/secrets.h` and fill in your Wi-Fi name and
   password. `secrets.h` is git-ignored so your password is never uploaded. The ESP32
   only works on 2.4 GHz Wi-Fi.
3. For Home Assistant, fill in `MQTT_HOST`, `MQTT_USER` and `MQTT_PASSWORD` in
   `secrets.h` (the Mosquitto add-on). Leave `MQTT_HOST` empty to turn MQTT off.
4. Plug in the ESP32 with a USB cable that carries data, then run
   `pio run -t upload -t monitor`.

## Naming devices

Neighbors' sensors often show up too. To tell yours apart, copy the gray ID under a
device on the web page into the `NAMES` list near the top of `src/main.cpp`:

```cpp
const FriendlyName NAMES[] = {
  {"Generic-Remote-12345", "Driveway Alarm"},
  {"Acurite-5n1-1234-A",   "Weather Station"},
  {nullptr, nullptr}
};
```

## Credits

Created by [Claude Code](https://claude.com/claude-code), Anthropic's AI coding
assistant, for James Booher's ESP32 weather projects. Claude Code wrote the code,
the wiring guide and this README, and tested it on a real ESP32-S3 with a CC1101.
