"This is the Central Scrutinizer"

So you have built (or otherwise obtained) a device labelled "Central
Scrutinizer" which allows you interact with the serial port of a M1/M2
and reboot it.

Otherwise, you can look at
https://git.kernel.org/pub/scm/linux/kernel/git/maz/cs-hw.git
for the hardware side of the project. Everything in this file
assumes that the board is assembled and functionnal.

This file describe how to build the software and flash it onto the
Raspberry-Pi Pico that controls the adapter. As for the HW, the SW
started its life as m1-ubmc, but let's be clear that its name really
is "Central Scrutinizer" so that you too can channel your inner FZ.

But first, credit where credit is due: this software is not an
original work, but is built on top of code which is:

  Copyright (c) 2014 The Chromium OS Authors
  Copyright (c) 2018 Reclaimer Labs - https://www.reclaimerlabs.com/
  Copyright (c) 2020 The Asahi Linux Contributors

See the LICENSE file for the fine print.

** Install the pre-requisites

On a Debian system, this is what you need:

  sudo apt install cmake gcc-arm-none-eabi build-essential git

I'm sure there is an equivalent for other OSs, but life is too short
to use anything else.

** Install the pico-sdk:

I've used versions 1.4 and 1.5 of the SDK. YMMV.

  git clone -b master https://github.com/raspberrypi/pico-sdk.git

  export PICO_SDK_PATH=/the/long/and/winding/road/to/pico-sdk

  cd pico-sdk

  git submodule update --init

** Build the Pico firmware:

It builds just like any other pico-sdk project:

  git clone git://git.kernel.org/pub/scm/linux/kernel/git/maz/cs-sw

  cd cs-sw

  mkdir build

  cd build

  cmake ..

  make

You should end-up with a file called m1_ubmc.uf2 in the build
directory. If you don't, something is wrong. Finding what is wrong is
your responsibility, not mine! ;-)

** Flash it

Place the Pico in programming mode by pressing the BOOTROM button
while plugging the USB connector, and issue something along the lines
of:

  sudo mount /dev/disk/by-id/usb-RPI_RP2_E0C9125B0D9B-0\:0-part1 /mnt

  sudo cp m1_ubmc.uf2 /mnt

  sudo eject /mnt

** Plug it
  
You will need a cable that contains most (if not all) of the USB-C
wires, and crucially the SBU signals. Cheap cables won't carry them,
but you won't find out until you actually try them.

You really want something that looks thick and stiff, and flimsy
cables are unlikely work (the cable that ships with M1 laptops
doesn't). None of my cables labelled USB2.0 seem to work. I've had
good results with cables designed to carry video signals, and USB3.1
cables seem to do the trick.

Because I'm lazy, the hardware only connects a single CC line to the
board's PD controller, and there is no provision to swap TX and RX.
Which means that on the board side, there is only a single valid
orientation for the USB-C cable. Trial and error are, as usual, your
best friends. Put a label on the board's end of the cable as an
indication of the orientation.

Also, there is only *ONE* USB-C port that is capable of serial
output. The device will happily connect to it, and allow things like
rebooting the Mac, but you won't get any serial output.

Models I know of:
- M1 mini 2020: USB-C port closest to the power supply
- M1 Ultra 2022: USB-C port closest to the Ethernet port
- M2 MacBook Air 2022: USB-C port closest to the MagSafe port
- M2 Pro mini 2023: USB-C port closest to the Ethernet port (Oliver)

No idea about other machines.

Optionally, you can make use of the micro-USB connector that is on
the other side of the board. It's main use it to allow interacting
with the Asahi m1n1 firmware, such as tethered booting.

** Use it

If you have correctly built and flashed the firmware, you will have
the Pico led blinking at the rate of twice a second, and a couple of
/dev/ttyACM* being advertised by your host:

  [708023.097129] usb 1-4: new full-speed USB device number 72 using xhci_hcd
  [708023.265195] usb 1-4: New USB device found, idVendor=2e8a, idProduct=000a, bcdDevice= 1.00
  [708023.265213] usb 1-4: New USB device strings: Mfr=1, Product=2, SerialNumber=3
  [708023.265219] usb 1-4: Product: Central Scrutinizer
  [708023.265223] usb 1-4: Manufacturer: AAAFNRAA
  [708023.265228] usb 1-4: SerialNumber: E66164084319392A
  [708023.273622] cdc_acm 1-4:1.0: ttyACM0: USB ACM device
  [708023.278612] cdc_acm 1-4:1.2: ttyACM1: USB ACM device

The board identifies itself as a Pico (as per VID/PID), and claims to
be the Central Scrutinizer, as you were hoping for.

Each of the two /dev/ttyACM* devices is a potential connection to a
Mac. Assuming the likely case that you have a single USB-C port on
your board, run:

  screen /dev/ttyACM0

and you should see something like:

  This is the Central Scrutinizer
  Control character is ^_
  Press ^_ + ? for help
  P0: VBUS OFF
  P0: Device ID: 0x91
  P0: Init
  P0: STATUS0: 0x80
  P0: VBUS OFF
  P0: Disconnected
  P0: S: DISCONNECTED
  P0: Empty debug message
  P0: IRQ=0 10 0
  P0: Connected: cc1=2 cc2=0
  P0: Polarity: CC1 (normal)
  P0: VBUS ON
  P0: S: DFP_VBUS_ON
  P0: Empty debug message
  P0: IRQ=71 4 0
  P0: S: DFP_CONNECTED
  P0: >VDM serial -> SBU1/2
  P0: IRQ=71 4 1
  P0: <VDM RX SOP"DEBUG (5) [504f] 5ac8052 91340000 306 0 0

If you see the ">VDM serial -> SBU1/2" line, the serial line should
now be connected and you can interact with the M1. Note that you can
use any serial configuration you want on the Mac side as long as it is
115200n8. One day I may implement the required controls, but that's
super low priority on the list of things I want to do. Also, there is
no such list, and the current setup works well enough for me.

Replace "screen" with whatever you want to communicate with the
device, be it conserver, minicom, cu, or even cat (there is no
accounting for taste).

Typing ^_? (Control-Underscore followed by a question mark) will lead
to the follwing dump:

  P0: Current port
  ^_    Escape character
  ^_ ^_ Raw ^_
  ^_ ^@ Send break
  ^_ !  DUT reset
  ^_ ^R Central Scrutinizer reset
  ^_ ^^ Central Scrutinizer reset to programming mode
  ^_ ^D Toggle debug
  ^_ ^M Send empty debug VDM
  ^_ ?  This message
  P0: Port 0: present
  P0: Port 1: present

which is completely self explainatory, but let's expand on it anyway:

- ^_ ^_ sends a raw ^_, just in case you really need it

- ^_ ^@ sends a break, which is useful if interacting with a Linux
  console as you get the sysrq functionality.

- ^_ ! resets the Mac without any warning. Yes, this is dangerous, use
  with caution and only when nothing else will do.

- ^_ ^R reboots the Central Scrutinizer itself. Not very useful, except
  when it is.

- ^_ ^^ reboots the Central Scrutinizer in programming mode, exactly
  as if you had plugged it with the BOOTROM button pressed. You end-up
  in mass-storage mode and can update the firmware.

- ^_ ^D toggles the "debug" internal flag. This has very little effect
  at the moment as most of the debug statements ignore the flag and
  spit out the junk on the console unconditionally.

- ^_ ^M sends an empty debug message to the remote PD controller.
  That's a debug feature...

- ^_ ? prints the help message (duh).

Finally, the Port 0:/1: lines indicate which I2C/UART combinations the
board is using. The HW supports two boards being driven by a single
Pico, but it is very unlikely you have built such a configuration.
