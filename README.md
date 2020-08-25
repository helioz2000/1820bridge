## DS18B20 Bridge to MQTT
Server component for Ross Wheeler's DS18B20 Arduino Nano based temperature reader connected via USB-serial port. 

Reads temperature values from the Arduino and publishes them to an MQTT server.

Target platform: Raspberry Pi

This code is run via two independant threads. The primary thread handles the tag configuration and MQTT publishing. The secondary thread handles reading from USB-serial port.

---
Note: the achieve consistent USB port assignment edit `/etc/udev/rules.d/50-usb.rules` 
and add something like this:
`SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", ATTRS{idProduct}=="7523", SYMLINK+="ttyNANOTEMP`
