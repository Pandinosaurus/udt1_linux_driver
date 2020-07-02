# Linux kernel driver for Microchip CAN BUS Analyzer Tool

The CAN BUS Analyzer Tool is a simple to use low cost CAN bus monitor which can be used to develop and debug a high speed CAN network. The tool supports CAN 2.0b and ISO11898-2 and a broad range of functions which allow it to be used across various market segments including automotive, industrial, medical and marine. The toolkit comes with all the hardware and software required to connect a CAN network to a PC. The Graphical User Interface makes it easy to quickly observe and interpret bus traffic.

[Product site](http://www.microchip.com/Developmenttools/ProductDetails.aspx?PartNO=APGDT002)

Originally the tool is supported on Windows environment only. This project adds support for the tool to Linux Kernel (SocketCAN). 

**NOTE: Driver is a part of Linux Kernel starting from [4.12](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/net/can/usb/mcba_usb.c?h=v4.12) version**

## Usage
### Building and installing
```
git clone https://github.com/UniSwarm/udt1_linux_driver.git
cd udt1_linux_driver
sudo make modules_install run

if there are error to do :
make clean
sudo make modules_install run
```
### Basic SocketCAN usage
To start SocketCAN interface:
```
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
```
To send CAN frame:
```
cansend can0 001#DEADBEEF
cansend can0 1000001#DEADBEEF
```
To dump CAN frames:
```
candump can0
```

### Supported bus settings
The tool works internally with 40Mhz clock. Following bus speed are supported by default:
* 20 Kbps
* 33.3 Kbps
* 50 Kbps
* 80 Kbps
* 83.3 Kbps
* 100 Kbps
* 125 Kbps
* 150 Kbps
* 175 Kbps
* 200 Kbps
* 225 Kbps
* 250 Kbps
* 275 Kbps
* 300 Kbps
* 500 Kbps
* 625 Kbps
* 800 Kbps
* 1000 Kbps

Note: Bittiming parameters are hardcoded inside device. Only speed can be configured using iproute2 utils.

### Termination
The tool supports build in termination. It can be controlled by sysfs. To read current termination status:
```
cat /sys/class/net/can0/termination
```
To enable termination:
```
echo 1 > /sys/class/net/can0/termination
```
To disable termination:
```
echo 0 > /sys/class/net/can0/termination
```
Termination values are stored in device's EEPROM (no need to set it again after device reconnection).

## Known issues
Official Microchip CAN BUS Analyzer firmware v2.3 contains bugs:
* Too low SPI sychro time (PIC_USB->PIC_CAN) causes CAN frame to be lost
* Sending remote frames do not work

Above issues are fixed in: https://github.com/rkollataj/mcba_firmware
