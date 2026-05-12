# MeshLog firmware
Firmware for [MeshCore logger](https://github.com/Anrijs/MeshLog)

## Setup
 - `log url https://<your_site>/meshlog/log.php` Where data is sent (should point to `log.php` file)
 - `log report 1800` Self-report interval, can be 0 to disable
 - `log auth SomeSecret` Secret used for web authorization
 - `wifi ssid YourWifiSSID`
 - `wifi password YourWifiPassword`
 - `set name Node-Name`
 - `set lat xx.xxxxx`
 - `set lon xx.xxxxx`
 - `reboot` Apply changes