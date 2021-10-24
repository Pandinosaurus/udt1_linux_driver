# Linux kernel driver for UniSwarm UDT1CR CAN Debugger Tool 

## Usage
### Automatic installation dkms and rules udev:

Clone repository on your local machine:

```bash
sudo apt install dkms can-utils
git clone https://github.com/UniSwarm/udt1_linux_driver.git
cd udt1_linux_driver
sudo make run_auto
```

Plug your UDT on USB port, can0 appear.

```bash
candump can0
```

#### DKMS method:
```bash
sudo make dkms
```

if UEFI Secure Boot is actived follow instruction:

* Configuring Secure Boot :
	- Ok and enter new pasword
	- reboot


* Perform MOK management :
	- select "Enroll MOK"
	- select "Continue" -> "Ok" -> enter password
	- reboot


#### Installation rules udev:

```bash
sudo make udev_install
```

### To remove all installed files: 
```bash
sudo make remove_all
```
#### Classic method 
```bash
sudo make modules_install run
```

if there are error :

```bash
make clean
sudo make modules_install run
```

After that, you can simply connect the debugger to PC with USB B.

### Basic SocketCAN usage
To start SocketCAN interface:

```bash
sudo ip link set can0 type can bitrate 500000
sudo ip link set can0 up
```

Install tools:
```bash
sudo apt install can-utils
```

To send CAN frame:

```bash
cansend can0 001#DEADBEEF
cansend can0 1000001#DEADBEEF
```
To dump CAN frames:

```bash
candump can0
```

### Supported bus settings
The tool works internally with 80Mhz clock. Following bus speed are supported by default:

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
