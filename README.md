# ESP32 433 MHz Receiver

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

Written by [Claude Code](https://claude.com/claude-code), Anthropic's AI coding
assistant, for James Booher's ESP32 weather projects.
