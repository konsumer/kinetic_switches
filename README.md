This is a device that turns kinetic (no battery) switches into wifi smart-switches with a shared ESP32.

It will trigger a URL, when it detects some bytes over 433 (like the kinetic switches.)


### esp32

- upload with `pio run -t upload` (need platformio installed)
- capture the identifiers of each switch (use URH with SDR.) Read [NOTES](NOTES.md) to see how I figured it out. I will try to get "autodetect" working, at some point.
- hold "BOOT" button to restart in "config mode" connect to KiteticSwitch wifi and setup wifi & id/triggers (in portal window that pops up)

## smart home

### home assistant

- Go to `/config/automation/dashboard` and setup a new automation webhook for each button


### alexa

- Enable [URL Routine Trigger](https://www.amazon.com/dp/B0BD8PP22L/)
- Go to [trigger admin](https://www.virtualsmarthome.xyz/url_routine_trigger/) and add a trigger URL for each button

