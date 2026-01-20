# Coyote OS for PicoCalc
Basic Calculator Firmware for PicoCalc. It uses Tinyexpr to evaluate expressions in calculator mode.

![calculator mode](/assets/scr_000.bmp)

Also includes a simple graphing mode

![graphing mode](/assets/scr_001.bmp)

Also includes a simple text mode, with file saving/loading from the SD card. 
Text mode can be accessed by pressing "Shift + Tab", which will pop up a menu. 

The calculator can also be rebooted to bootloader by pressing F5, and selecting the reboot option. 


## Building
```
git clone --recursive https://github.com/laingcc/Picocalc-Coyote-OS.git
cd Picocalc-Coyote-OS/Coyote/

mkdir build
cd build
export PICO_SDK_PATH=/path/to/pico-sdk
cmake ..
make
```

## How to Upload UF2 

Uploading a UF2 file to the Raspberry Pi Pico on a Linux system is straightforward. Here’s how you can do it:

### Step 1: Prepare Your Raspberry Pi Pico
Enter Bootloader Mode:

- Hold down the BOOTSEL button on your Pico.
- While holding the button, connect the Pico to your Linux PC via USB.
- Release the BOOTSEL button.
- Check If the Pico Is Recognized:

Your Pico should appear as a mass storage device named RPI-RP2.

Verify using the following command:

```bash
lsblk
```

You should see a new device (e.g., /media/$USER/RPI-RP2 or /run/media/$USER/RPI-RP2).

### Step 2: Copy the UF2 File to the Pico
```
cp your_firmware.uf2 /media/$USER/RPI-RP2/
```

### Step 3: Run it
On PicoCalc, the default serial port of the Pico is the USB Type-C port, not its built-in Micro USB port.  
So here is the standard running procedures: 

- Unplug the pico from Micro-USB cable
- Plug the pico via USB Type-C
- Press Power On on Top of the PicoCalc


## Serial Output

If your firmware includes serial output, you can monitor it using **minicom**, **screen**, or the Arduino IDE serial monitor.  
See instructions above for connecting and selecting the correct serial port.


## Requirements

- Raspberry Pi Pico
- PicoCalc hardware

## License

See LICENSE file for details.
