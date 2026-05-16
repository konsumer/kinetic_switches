This is a device that turns kinetic (no battery) switches into wifi smart-switches with a shared ESP32.

### setup

Maybe at some point I will work out a detection routine for it (enter "detect mode", press a button, store ID) but it works for me to just modify the code. These switches vary widely, so you may have to work something else out, entirely, and even if they are identical, you will need to capture the IDs of yours, in [Universal Radio Hacker](https://github.com/jopohl/urh). Read [NOTES](NOTES.md) to see how I figured it out.


### upload

I am using platformio, so pip-install that first.

```sh
pio run -t upload -t monitor
```