# ESP32 Beverage Antenna Controller

## Summary

This project is organized around a clear controller model for the beverage antenna system. The clubhouse station is intended to be the central authority that manages the actual antenna state, while remote panels provide a convenient way to request changes from other locations.

That means remotes do not become the source of truth. They only ask. The club station decides and reports the real result.

In practice, this keeps the system behavior deterministic and easier to understand: local and remote actions both flow into the same controller, and the club station publishes the authoritative state that the rest of the system can trust.

## Notes

- **WiFi connection and callsign acquisition:** on boot, the firmware first tries to load saved WiFi credentials from NVS (Non-Volatile Storage) through HB9IIUPortal. If valid credentials are already stored, it connects directly to the configured network and then loads the station callsign from NVS for use as the panel or station identifier. If no valid credentials are available, the device starts the captive portal, where the user selects the WiFi network, enters the password, and provides the callsign; once validated, all of that information is saved and used on the next boot.

- **Factory reset behavior:** if the encoder push button is held during boot, the firmware performs a full NVS erase and restarts. This clears stored WiFi credentials, callsign and configuration data, presets, and other saved preferences, forcing the device back into first-time setup behavior.

- **Antenna selection workflow:** the clubhouse station is the authoritative controller for antenna state. Local encoder movement enters a preview phase on the dial, and the actual antenna change is only committed after rotation stops for a short settle interval. At commit time, the firmware executes the physical switching command and publishes the new antenna state over MQTT.

- **MQTT role and topics:** the clubhouse station subscribes to antenna/cmd for incoming remote selection requests and publishes the authoritative retained state on antenna/state. This allows remote panels to request a change while the clubhouse station remains the single source of truth for the selected antenna.

- **User interface structure:** the touchscreen firmware provides three main views: the antenna dial, a great-circle map view, and a keypad or relay page. The dial is the primary control surface, the map visualizes the currently selected azimuth from the configured home location, and the keypad page drives relay outputs through the PCF8574 expander.

- **Local control modes and presets:** in addition to the rotary encoder, the firmware supports left and right hardware buttons with two operating modes. Mode A provides direct antenna stepping, while Mode B provides preset recall and preset saving after a long-press arm action on the encoder button.

- **Hardware switching method:** antenna switching is not performed by directly selecting a relay matrix in this file. Instead, the committed antenna number is translated into a DTMF symbol and sent out through the HT9200 signaling path, which appears to control the downstream switching hardware.

## MQTT Explorer

Download MQTT Explorer from https://mqtt-explorer.com/ and create a connection to the same broker used by the firmware.

Connect with these settings:

- Open MQTT Explorer
- Create New Connection
- Set Host to 1db5ec5c4ff44e67bcb25a811f852e53.s1.eu.hivemq.cloud
- Set Port to 8883
- Turn on TLS/SSL
- Set Username to esp32-club
- Set Password to the value from CLUB_STATION.cpp:85
- Set Client ID to something unique, for example daniel-mqtt-test
- Click Connect