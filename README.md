This is a device that turns kinetic (no battery) switches into wifi smart-switches with a shared ESP32.

### setup

- Enable [URL Routine Trigger](https://www.amazon.com/dp/B0BD8PP22L/)
- Go to [trigger admin](https://www.virtualsmarthome.xyz/url_routine_trigger/) and add a URL for each button
- capture the identifiers of each switch (use URH with rtlsdr.) Read [NOTES](NOTES.md) to see how I figured it out.
- hold "BOOT" to restart in "config mode" connect to KiteticSwitch wifi and setup wifi & id/triggers (in portal window that pops up)

### upload

I am using platformio, so pip-install that first.

```sh
pio run -t upload -t monitor
```