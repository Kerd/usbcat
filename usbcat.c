/* 
 * Simple utility used mostly to send AT commands to LTE modems in CDC mode under FreeBSD
 *
 * My ZTEWelink ME3630 LTE Modem (vid:0x19d2, pid:0x1476) didn't work well with 'usbconfig do_request' method
 * and i had to deal with it somehow. Seems bulk transfers do the job.
 *
 * I used 'bulk' demo program by Joerg Wunsch as example and borrowed most of the code.
 * This utility is non optimal and not complete piece of code. Spent ~2h to write and test it just to be able
 * to configure my LTE modem under OPNSense, nothing more :) Maybe will be useful to someone also. Feel free to
 * modify it as you need.
 * 
 * Usage: usbcat <VID> <PID> <OUT_EP> <IN_EP> [IN_TIMEOUT (ms)]
 *
 * * VID and PID - usual USB stuff to find your device
 * * OUT_EP - End point number to send data to device
 * * IN_EP - End point number to receive data from device (must hive high bit set, so value must be 0x80 or greater)
 * * IN_TIMEOUT - how long (ms) will wait each read from IN_EP
 *
 * * <stdin> is used as source of commands. Up to BUFLEN (512 bytes).
 *
 * Environment variables:
 *  WITH_DEBUG - if defined, will print a bit of debug output
 *  WITH_DRAIN - if defined, will read and print data from IN_EP before sending to OUT_EP
 *
 * Result:
 *  0 - everything is ok
 *  1 - got no data from IN_EP after sending to OUT_EP
 *  2 - failed to send to OUT_EP
 *  3 - both, 1 and 2
 *  66 - device not found (EX_NOINPUT)
 *  70 - some initialization error (EX_SOFTWARE)
 *
 * Compile:
 *
 *  gcc -lusb -o usbcat usbcat.c
 *
 * Examples:
 *
 * ME3630 got multiple intrafaces with 2-3 endpoints in each. So, AT command interface got OUT_EP=3 and IN_EP=0x84.
 *
 * * Get endpoint addresses for OUT_EP and IN_EP from 'usbconfig' under FreeBSD.
 * * For ME3630 i used BULK OUT (3)/BULK IN (0x84) endpoints of Interface 2.
 *
 *   usbconfig -d ugen1.2 -v | grep -E " Interface| Endpoint|bEndpointAddress|bmAttributes"
 * 
 * * Get device identification... With debug and reading pending data. It is fast, so timeout is short (100ms)
 *
 *   printf "ATI\r" | WITH_DEBUG=1 WITH_DRAIN=1 ./usbcat 0x19d2 0x1476 0x3 0x84 100 
 * 
 * * Just drain pending data from IN_EP and get response to AT command as well to be sure the device is there.
 *
 *   printf "AT\r" | WITH_DRAIN=1 ./usbcat 0x19d2 0x1476 0x3 0x84 100
 *
 * * Get info about ECM connection state:
 *
 *   printf "AT+ZECMCALL?\r" | ./usbcat 0x19d2 0x1476 0x3 0x84 500
 *
 * It should output something like:
 *
 *   +ZECMCALL: IPV4, 12.192.19.88, 12.192.19.89, 10.10.52.131, 10.10.52.130
 *   OK
 *
 * * Start ECM call. It may take some time, so timeout is long enough if we bother about result.
 * * Or you may issue command with short timeout and then use WITH_DRAIN a bit later.
 *
 *   printf "AT+ZECMCALL=1\r" | ./usbcat 0x19d2 0x1476 0x3 0x84 5000
 *
 * (C) 2025, Kirill Kuteynikov
*/

/*-
 * SPDX-License-Identifier: Beerware
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42) (by Poul-Henning Kamp):
 * <joerg@FreeBSD.ORG> wrote this file.  As long as you retain this notice you
 * can do whatever you want with this stuff. If we meet some day, and you think
 * this stuff is worth it, you can buy me a beer in return.        Joerg Wunsch
 * ----------------------------------------------------------------------------
 */

#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sysexits.h>
#include <unistd.h>

#include <libusb20.h>
#include <libusb20_desc.h>

/*
 * Print "len" bytes from "buf" in hex, followed by an ASCII
 * representation (somewhat resembling the output of hd(1)).
 */
void
print_formatted(uint8_t *buf, uint32_t len)
{
  int i, j;

  for (j = 0; j < len; j += 16)
    {
      printf("%02x: ", j);

      for (i = 0; i < 16 && i + j < len; i++)
	printf("%02x ", buf[i + j]);
      printf("  ");
      for (i = 0; i < 16 && i + j < len; i++)
	{
	  uint8_t c = buf[i + j];
	  if(c >= ' ' && c <= '~')
	    printf("%c", (char)c);
	  else
	    putchar('.');
	}
      putchar('\n');
    }
}

/*
 * If you want to see the details of the internal datastructures
 * in the debugger, unifdef the following.
 */
#ifdef DEBUG
#  include <sys/queue.h>
#  include "/usr/src/lib/libusb/libusb20_int.h"
#endif

#define BUFLEN 512

#define OTIMEOUT 5000 		/* 5 s */
#define ITIMEOUT 5000 		/* 5 s */

int with_debug = 0;
int with_drain = 0;

size_t get_out_data(uint8_t* buf, size_t maxlen) {
    size_t len;
    len = fread(buf, 1, maxlen, stdin);	
    /*
    while(!feof(stdin)) {
	len = fread(buf, 1, maxlen, stdin);	

    }
    */
    return len;
}

static int doit(struct libusb20_device *dev, int rd_ep, int wr_ep, unsigned long itimeout)
{
    int rv;
    uint8_t out_buf[BUFLEN];
    size_t out_len;
    int result = 0;
    
    out_len = get_out_data(out_buf, sizeof(out_buf));
    if (out_len < 0) {
	fprintf(stderr, "Failed to read data to send.\n");
	return EX_SOFTWARE;
    }
    
  /*
   * Open the device, allocating memory for two possible (bulk or
   * interrupt) transfers.
   *
   * If only control transfers are intended (via
   * libusb20_dev_request_sync()), transfer_max can be given as 0.
   */
    if ((rv = libusb20_dev_open(dev, 2)) != 0)
    {
	fprintf(stderr, "libusb20_dev_open: %s\n", libusb20_strerror(rv));
	return EX_SOFTWARE;
    }

  /*
   * If the device has more than one configuration, select the desired
   * one here.
   */
    if ((rv = libusb20_dev_set_config_index(dev, 0)) != 0)
    {
      fprintf(stderr, "libusb20_dev_set_config_index: %s\n", libusb20_strerror(rv));
      return EX_SOFTWARE;
    }

  /*
   * Two transfers have been requested in libusb20_dev_open() above;
   * obtain the corresponding transfer struct pointers.
   */
    struct libusb20_transfer *xfr_out = libusb20_tr_get_pointer(dev, 0);
    struct libusb20_transfer *xfr_in = libusb20_tr_get_pointer(dev, 1);

    if (xfr_in == NULL || xfr_out == NULL)
    {
      fprintf(stderr, "libusb20_tr_get_pointer: %s\n", libusb20_strerror(rv));
      return EX_SOFTWARE;
    }

  /*
   * Open both transfers, the "out" one for the write endpoint, the
   * "in" one for the read endpoint (ep | 0x80).
   */
    if ((rv = libusb20_tr_open(xfr_out, 0, 1, wr_ep)) != 0)
    {
	fprintf(stderr, "libusb20_tr_open: %s\n", libusb20_strerror(rv));
	return EX_SOFTWARE;
    }
    if ((rv = libusb20_tr_open(xfr_in, 0, 1, rd_ep)) != 0)
    {
	fprintf(stderr, "libusb20_tr_open: %s\n", libusb20_strerror(rv));
	return EX_SOFTWARE;
    }
    libusb20_tr_set_flags(xfr_out, LIBUSB20_TRANSFER_DO_CLEAR_STALL);
    libusb20_tr_set_flags(xfr_in, LIBUSB20_TRANSFER_DO_CLEAR_STALL);

    uint8_t in_buf[BUFLEN];
    uint32_t rlen = 1;

    while(with_drain && (rlen > 0)) {
	rlen = 0;
	if ((rv = libusb20_tr_bulk_intr_sync(xfr_in, in_buf, BUFLEN, &rlen, 50)) != 0)
        {
	    if (rv != LIBUSB20_TRANSFER_TIMED_OUT)
		fprintf(stderr, "libusb20_tr_bulk_intr_sync: %s\n", libusb20_strerror(rv));
	}
	if (rlen > 0) {
	    if (with_debug) fprintf(stderr, "drain received %d bytes\n", rlen);
	    fwrite(in_buf, 1, rlen, stdout);
	    fputs("\n", stdout);
	    //print_formatted(in_buf, rlen);
	}
    };

    if (out_len > 0)
    {
	if ((rv = libusb20_tr_bulk_intr_sync(xfr_out, out_buf, out_len, &rlen, OTIMEOUT)) != 0)
	{
	    fprintf(stderr, "libusb20_tr_bulk_intr_sync (OUT): %s\n", libusb20_strerror(rv));
	}
	if (rlen != out_len) {
	    fprintf(stderr, "sent %d bytes of %d\n", rlen, out_len);
	    result |= 2;
	}
    }
    size_t len = 0;
    do {
	rlen = 0;
	if ((rv = libusb20_tr_bulk_intr_sync(xfr_in, in_buf, BUFLEN, &rlen, itimeout)) != 0)
	{
	    if (rv != LIBUSB20_TRANSFER_TIMED_OUT)
		fprintf(stderr, "libusb20_tr_bulk_intr_sync: %s\n", libusb20_strerror(rv));
        }
	if (rlen > 0) {
	    if (with_debug) fprintf(stderr, "received %d bytes\n", rlen);
	    fwrite(in_buf, 1, rlen, stdout);
	    len += rlen;
	//print_formatted(in_buf, rlen);
	}
    } while(rlen > 0);
    if (len == 0) {
	fputs("\n", stdout);
	result |= 1;
    }

    libusb20_tr_close(xfr_out);
    libusb20_tr_close(xfr_in);

    libusb20_dev_close(dev);
    return result;
}

static void usage(const char* app) {
    fprintf(stderr, "Usage %s <VID> <PID> <OUT_EP> <IN_EP> [IN_TIMEOUT (ms)]");
    exit(EX_USAGE);
}

int
main(int argc, char **argv)
{
    int rd_ep = 0, wr_ep = 0; /* endpoints */
    unsigned long itimeout=ITIMEOUT;
    unsigned int vid = UINT_MAX, pid = UINT_MAX; /* impossible VID:PID */
    int result;

    if (argc < 5) usage(argv[0]);
    if (argc > 6) usage(argv[0]);

    if (getenv("WITH_DEBUG")) with_debug = 1;
    if (getenv("WITH_DRAIN")) with_drain = 1;

    vid = strtoul(argv[1], NULL, 0);
    pid = strtoul(argv[2], NULL, 0);
    wr_ep = strtoul(argv[3], NULL, 0);
    rd_ep = strtoul(argv[4], NULL, 0);
    if (argc > 5) {
	itimeout = strtoul(argv[5], NULL, 0);	
    }

    if (rd_ep == 0 || wr_ep == 0) {
	usage(argv[0]);
    }
    if ((rd_ep & 0x80) == 0) {
    	fprintf(stderr, "IN_EP must have bit 7 set\n");
	return (EX_USAGE);
    }

    struct libusb20_backend *be;
    struct libusb20_device *dev;

    if ((be = libusb20_be_alloc_default()) == NULL)
    {
      	perror("libusb20_be_alloc()");
	return EX_SOFTWARE;
    }

    dev = NULL;
    result = EX_NOINPUT; // device not found
    while ((dev = libusb20_be_device_foreach(be, dev)) != NULL)
    {
      	struct LIBUSB20_DEVICE_DESC_DECODED *ddp = libusb20_dev_get_device_desc(dev);
	
	if (ddp->idVendor == vid && ddp->idProduct == pid) {
	    if (with_debug)
		fprintf(stderr, "Found device %s (VID:PID = 0x%04x:0x%04x)\n",
		    libusb20_dev_get_desc(dev), ddp->idVendor, ddp->idProduct);
	    result = doit(dev, rd_ep, wr_ep, itimeout);
	    break;
	}
    }

    libusb20_be_free(be);

    return result;
}
