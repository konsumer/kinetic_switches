- [Great Walkthrough Video](https://www.youtube.com/watch?v=tz_F4Tjhap0&t=997s)
- [some analysis](https://tomasmcguinness.com/2025/02/04/detecting-a-433mhz-kinetic-push-switch-with-an-esp32-part-1/)
- [more complete analysis](https://oliver-hihn.de/blog/1743491646169-kinetic-switch-hack/)

I used [Universal Radio Hacker](https://github.com/jopohl/urh) to figure this out for mine:

- center: `433.92 MHz`
- noise: `0.0026`
- center: `0.0013`
- samples per symbol: `70`
- error tolerance: `1`
- modulation: `ASK`
- bits per symbol: `1`

these numbers repeat in data (hex) when I press the buttons

- Switch A: `8e8e88e88888`
- Switch B: `8eeee88eeeff`
- Switch C: `8eeee88eeeee`

So that is what I setup.

### hardware

I used ESP32 S2 Mini, because I had a couple laying around, so I hooked up like like this, but yours may be different. Any ESP32 (or pretty much any arduino that can do wifi) should be fine. I use colors to keep track of things, obviously you can use whatever colored wire you want.

| CC1101 pin | S2 Mini pin | Wire   |
|------------|-------------|--------|
| VCC        | 3.3V        | Red    |
| GND        | GND         | Black  |
| SCK        | 36          | Yellow |
| MISO (SO)  | 37          | Blue   |
| MOSI (SI)  | 35          | White  |
| CSN        | 34          | Green  |
| GDO0       | 33          | Orange |
| GDO2       | 16          | Purple |

- CC1101 is 3.3V only — don't connect to 5V or it dies.
- on-board LED is 15, so I use that

If you are not using ESP32 S2 Mini, edit pin-defines in [platformio](platformio.ini)


