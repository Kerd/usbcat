# usbcat
Communicate with USB devices using libusb library. Allow to specify input and output device endpoints, so don't need special devices. Wrote it to send AT commands to my LTE modem under FreeBSD

## Simple utility used mostly to send AT commands to LTE modems in CDC mode under FreeBSD

My ZTEWelink ME3630 LTE Modem (vid:0x19d2, pid:0x1476) didn't work well with 'usbconfig do_request' method
and i had to deal with if somehow. Seems bulk transfers do the job.

I used 'bulk' demo program by Joerg Wunsch as example and borrowed most of the code.
This utility is non optimal and not complete piece of code. Spent ~2h to write and test it just to be able
to configure my LTE modem under OPNSense, nothing more :) Maybe will be useful to someone also. Feel free to
modify it as you need.

## Usage

```Usage: usbcat <VID> <PID> <OUT_EP> <IN_EP> [IN_TIMEOUT (ms)]```

* VID and PID - usual USB stuff to find your device
* OUT_EP - End point number to send data to device
* IN_EP - End point number to receive data from device (must hive high bit set, so value must be 0x80 or greater)
* IN_TIMEOUT - how long (ms) will wait each read from IN_EP

* <stdin> is used as source of commands. Up to BUFLEN (512 bytes).

Environment variables:
 WITH_DEBUG - if defined, will print a bit of debug output
 WITH_DRAIN - if defined, will read and print data from IN_EP before sending to OUT_EP

### Result:
 - 0: everything is ok
 - 1: got no data from IN_EP after sending to OUT_EP
 - 2: failed to send to OUT_EP
 - 3: both, 1 and 2
 - 66: device not found (EX_NOINPUT)
 - 70: some initialization error (EX_SOFTWARE)

## Compile:

 ```gcc -lusb -o usbcat usbcat.c```

## Examples:

ME3630 got multiple intrafaces with 2-3 endpoints in each. So, AT command interface got OUT_EP=3 and IN_EP=0x84.

* Get endpoint addresses for OUT_EP and IN_EP from 'usbconfig' under FreeBSD. For ME3630 i used BULK OUT (3)/BULK IN (0x84) endpoints of Interface 2.

  ```usbconfig -d ugen1.2 -v | grep -E " Interface| Endpoint|bEndpointAddress|bmAttributes"```

* Get device identification... With debug and reading pending data. It is fast, so timeout is short (100ms)

  ```printf "ATI\r" | WITH_DEBUG=1 WITH_DRAIN=1 ./usbcat 0x19d2 0x1476 0x3 0x84 100```

* Just drain pending data from IN_EP and get response to AT command as well to be sure the device is there.

  ```printf "AT\r" | WITH_DRAIN=1 ./usbcat 0x19d2 0x1476 0x3 0x84 100```

* Get info about ECM connection state:

  ```printf "AT+ZECMCALL?\r" | ./usbcat 0x19d2 0x1476 0x3 0x84 500```

  It should output something like:

  ```
  +ZECMCALL: IPV4, 12.192.19.88, 12.192.19.89, 10.10.52.131, 10.10.52.130
  OK
  ```

* Start ECM call. It may take some time, so timeout is long enough if we bother about result. Or you may issue command with short timeout and then use WITH_DRAIN a bit later.

  ```printf "AT+ZECMCALL=1\r" | ./usbcat 0x19d2 0x1476 0x3 0x84 5000```
