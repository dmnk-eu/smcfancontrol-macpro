# SMCFancontrol

Source of original script: https://wiki.debian.org/DebianOnIntelMacPro

The script reads the temperature values from memory and adjusts the speed of the fans accordingly. The watchdog is also supported, so the process restarts if the script ends unexpectedly.

## Compile

Make sure `libsystemd-dev` is installed and run the following command:

```
gcc -o smcfancontrol smcfancontrol.c -lsystemd
```

## Install

```
sudo mkdir -p /opt/smcfancontrol/
sudo cp ./* /opt/smcfancontrol/
sudo chmod 700 /opt/smcfancontrol/smcfancontrol
sudo cp /opt/smcfancontrol/smcfancontrol.service /etc/systemd/system/
sudo systemctl enable smcfancontrol.service
sudo systemctl start smcfancontrol.service
```
