# ESP32-S3-N16R8-with-Katana-50W-MKII
This project implements a USB‑MIDI foot controller for the Boss Katana 50W MKII using an ESP32‑S3‑N16R8.
The ESP32‑S3 operates in USB host mode and communicates with the amplifier through a USB‑C to USB‑B cable.
All communication is performed using the same SysEx messages used by Boss Tone Studio, ensuring full compatibility with the Katana MKII.

The controller supports:
- Four channel switches (CH1–CH4)
- Five effect switches (Booster, Mod, FX, Delay, Reverb)
- Effect type cycling (green, red, yellow) and on/off control
- Automatic Katana editor‑mode handshake on connection

Hardware requirements:
- ESP32‑S3‑N16R8 module
- USB OTG bridge and IN/OUT bridge soldered on the module
- 5V power supply
- USB‑C to USB‑B cable
- Momentary switches wired to GPIO pins with internal pull‑ups enabled

The firmware was created through extensive trial and error using Cursor AI and the GitHub Copilot extension in Visual Studio Code.
It provides a low‑cost, customizable alternative to commercial Katana foot controllers.
