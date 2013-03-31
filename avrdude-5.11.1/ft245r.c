/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2003-2004  Theodore A. Roth  <troth@openavr.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* $Id$ */

/* ft245r -- FT245R/FT232R Synchronuse BitBangMode Programmer
  default pin assign 
               FT232R / FT245R
  miso  = 1;  # RxD   / D1
  sck   = 2;  # RTS   / D2
  mosi  = 0;  # TXD   / D0
  reset = 4;  # DTR   / D4
*/

#include "ac_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>

#include "avr.h"
#include "pindefs.h"
#include "pgm.h"
#include "bitbang.h"
#include "ft245r.h"

//#define USE_LIBFTDI

#if defined(_WIN32) || defined(SUPPORT_FT245R)
#if defined(_WIN32)
#include <windows.h>
#include "ftd2xx.h"
#elif defined(USE_LIBFTDI)
#include "ftdi.h"
#define FT_STATUS	int
#define DWORD 		unsigned int
#define UCHAR		unsigned char
#define FT_OK		(0)
#define FT_ERROR	(-1)
#else
#include "ftd2xx.h"
#endif

#define FT245R_CYCLES	2
#define FT245R_FRAGMENT_SIZE  (is_hispeed?(62*64):(62*4))
#define FT245R_MAX_FRAGMENT_SIZE  (62*64)
#define REQ_OUTSTANDINGS	10
#define USE_INLINE_WRITE_PAGE
#define USE_LOCAL_DELAY

#define FT245R_DEBUG	1

extern char * progname;
extern int do_cycles;
extern int ovsigck;
extern int verbose;

#if defined(USE_LIBFTDI)
static struct ftdi_context *handle;
#else
static FT_HANDLE handle;
#endif

static unsigned char ft245r_ddr = 0;
static unsigned char ft245r_data = 0;

static unsigned char ft245r_sck = 0;
static unsigned char ft245r_mosi = 0;
static unsigned char ft245r_reset = 0;
static unsigned char ft245r_miso = 0;

static unsigned char ft245r_rdyled = 0;
static unsigned char ft245r_pgmled = 0;

static unsigned char ft245r_ddr_cbus = 0;
static const unsigned char ft245r_data_cbus = 0;

static unsigned char ft245r_rdyled_cbus = 0;
static unsigned char ft245r_pgmled_cbus = 0;

static int is_hispeed = 0;

static inline void setbit(UCHAR *data, int pinno, int v)
{
	if (v) {
		*data |= (1 << (pinno));
	} else {
		*data &= ~(1 <<(pinno));
	}
}

static inline int set_pgmled(unsigned char *buf, int on)
{
  int buf_pos = 0;

  if (on) {
        ft245r_data &= ~ft245r_pgmled;
        ft245r_data |= ft245r_rdyled;
  } else {
        ft245r_data |= ft245r_pgmled;
        ft245r_data &= ~ft245r_rdyled;
  }
  buf[buf_pos++] = ft245r_data;
  return buf_pos;
}

static int ft245r_delay(PROGRAMMER * pgm, int us);

static int ft245r_send(PROGRAMMER * pgm, unsigned char * buf, size_t len)
{
  FT_STATUS r = FT_OK;
  DWORD rlen;
#if !defined(USE_LIBFTDI)
  r = FT_Write(handle, buf, len, &rlen);
  if (r == FT_OK) return 0;
  if (len != rlen) return -1;
#else
  r = rlen = ftdi_write_data(handle, buf, len);
  if (r >= 0) r = FT_OK;
  if (len != rlen) r = FT_ERROR;
#endif
  return r;
}


static int ft245r_recv(PROGRAMMER * pgm, unsigned char * buf, size_t len)
{
  FT_STATUS r = FT_OK;
  DWORD rlen;

#if !defined(USE_LIBFTDI)
  r = FT_Read(handle, buf, len, &rlen);
#else
retry:
  r = rlen = ftdi_read_data(handle, buf, len);
  if (r >= 0) {
	r = FT_OK;
  	if (len != rlen) {
		buf += rlen;
		len -= rlen;
		goto retry;
	}
  }
#endif

  if (r != FT_OK || len != rlen) {
    fprintf(stderr,
	    "%s: ft245r_recv(): programmer is not responding\n",
	    progname);
    exit(1);
  }
  return 0;
}

static int set_reset(PROGRAMMER * pgm, int val)
{
  unsigned char buf[1];

  if (val) ft245r_data |= ft245r_reset;
  else     ft245r_data &= ~ft245r_reset;

  buf[0] = ft245r_data;

  ft245r_send (pgm, buf, 1);
  ft245r_recv (pgm, buf, 1);
  return 0;
}

static int set_rdyled(PROGRAMMER * pgm)
{
  FT_STATUS r = 0;
  unsigned char data;

  if (ft245r_rdyled_cbus || ft245r_pgmled_cbus) {
     data = (ft245r_ddr_cbus << 4) | ft245r_data_cbus;

#if !defined(USE_LIBFTDI)
     r = FT_SetBitMode(handle, data, FT_BITMODE_CBUS_BITBANG); // send CBUS data
#else
     r = ftdi_set_bitmode(handle, data, BITMODE_CBUS); // send CBUS data
#endif
     if ((verbose>=1) || FT245R_DEBUG) {
         fprintf(stderr, "ft245r: set CBUS %x -> %d\n",data ,(int)r);
     }
  }
#if !defined(USE_LIBFTDI)
  FT_SetBitMode(handle, ft245r_ddr, FT_BITMODE_SYNC_BITBANG);
#else
  ftdi_set_bitmode(handle, ft245_ddr, BITMODE_SYNCBB);
#endif
  set_reset(pgm, 0);

  return r;
}

static int ft245r_drain(PROGRAMMER * pgm, int display)
{
  FT_STATUS r = FT_OK;
#if !defined(USE_LIBFTDI)
  DWORD n;
  r = FT_GetQueueStatus(handle, &n);
  if (r != FT_OK) return -1;
  if (n) {
	fprintf(stderr, "ft245r_drain called but queue is not empty %d \n",
		(int)n);
  }
  r = FT_Purge(handle, FT_PURGE_RX | FT_PURGE_TX);
  set_reset(pgm, 0); // set init state
  r = FT_SetBitMode(handle, ft245r_ddr, FT_BITMODE_SYNC_BITBANG);
  if (r != FT_OK) return -1;
#else
  r = ftdi_usb_purge_buffers(handle);
  if (r != FT_OK) return -1;
  set_reset(pgm, 0); // set init state
  r = ftdi_set_bitmode(handle, ft245r_ddr, BITMODE_SYNCBB);
  if (r != FT_OK) return -1;
#endif
  return 0;
}

static inline int ft245r_sync(PROGRAMMER * pgm)
{
  FT_STATUS r = FT_OK;
#if !defined(USE_LIBFTDI)
  UCHAR ch;
  r = FT_GetBitMode(handle, &ch);
#else
  ftdi_async_complete(handle , 1);
#endif
  if (r != FT_OK) return -1;
  return 0;
}

static int ft245r_chip_erase(PROGRAMMER * pgm, AVRPART * p)
{
  unsigned char cmd[4];
  unsigned char res[4];

  if (p->op[AVR_OP_CHIP_ERASE] == NULL) {
    fprintf(stderr, "chip erase instruction not defined for part \"%s\"\n",
            p->desc);
    return -1;
  }

  memset(cmd, 0, sizeof(cmd));

  avr_set_bits(p->op[AVR_OP_CHIP_ERASE], cmd);
  pgm->cmd(pgm, cmd, res);
  ft245r_delay(pgm, p->chip_erase_delay);
  pgm->initialize(pgm, p);

  return 0;
}

static unsigned char saved_signature[3];

static int valid_rates_r[] = {	// FT245R,  FT232R , FT2232D 
   600, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 
   115200, 230400, 460800, 921600
};

static int valid_rates_h[] = {	// FT2232H , FT232H
   600, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 
   115200, 230400, 460800, 921600,
   1000000, 1200000, 1500000, 2000000, 2400000, 3000000,
   4000000, 6000000, 12000000
};

static int *valid_rates = valid_rates_r;
int valid_rates_num = sizeof(valid_rates_r)/sizeof(valid_rates_r[0]);

static int saved_rate;

static int ft245r_set_bitclock(PROGRAMMER * pgm, double bitclock) {
  FT_STATUS r = 0;
  int rate = 0;
  int i;

  if (bitclock == 0.0) { // using default
	  rate = 235000.0 /2; 
  } else if (bitclock <  0.0015) {
	  rate =   1500.0 /2;
  } else {
	  rate = bitclock * 1000000.0 /2 + 1.0;
  }
  for (i= valid_rates_num -1; i>=0; --i) 
  {
    if (valid_rates[i] <= rate) {
		rate = valid_rates[i];
		break;
    }
  }
  if (i<=0) rate = valid_rates[0];

#if !defined(USE_LIBFTDI)
  r = FT_SetBaudRate(handle, rate);
#else
  r = ftdi_set_baudrate(handle, rate);
#endif
  if ((verbose>=1) || FT245R_DEBUG) {
	fprintf(stderr," ft245r:  bitclk %d -> ft baud %d\n", 
			rate * 2, rate);
  }
  saved_rate = rate;
  return 0;
}

static int ft245r_restore_bitclock(PROGRAMMER * pgm) {
  FT_STATUS r;
#if !defined(USE_LIBFTDI)
  r = FT_SetBaudRate(handle, saved_rate);
#else
  r = ftdi_set_baudrate(handle, saved_rate);
#endif
  return r;
}

#define MAX_PULSE_LEN  56
static int pulse_reset(PROGRAMMER * pgm, int len)
{
  int i;
  unsigned char buf[MAX_PULSE_LEN];

  if (len <= 1)
	len = 1;
  if (len >= MAX_PULSE_LEN-1)
	len = MAX_PULSE_LEN-1;

#if !defined(USE_LIBFTDI)
  FT_SetBaudRate(handle, 1200); // 2400 Hz : period 0.156 ms
#else
  ftdi_set_baudrate(handle, 1200); // 2400 Hz : period 0.156 ms
#endif
  for (i=0; i< len; i++) {
  	buf[i] = ft245r_reset | ft245r_data;
  }
  buf[i++] = ft245r_data;

  ft245r_send (pgm, buf, i);
  ft245r_recv (pgm, buf, i);

  ft245r_restore_bitclock(pgm);
  return 0;
}

static int pulse_sck(PROGRAMMER * pgm)
{
  unsigned char buf[2];

  buf[0] = ft245r_data;
  buf[1] = ft245r_data | ft245r_sck;

  ft245r_send (pgm, buf, 2);
  ft245r_recv (pgm, buf, 2);
  return 0;
}

#define DELAY_LEN  (is_hispeed?(62*64):(62*4))
#define MAX_DELAY_LEN  (62*64)
static int ft245r_delay(PROGRAMMER * pgm, int us)
{
#ifdef USE_LOCAL_DELAY
  int len, sub_len;
  int led_on = 1;
  int send_count = 0;
  int recv_count = 0;
  unsigned char buf[MAX_DELAY_LEN];
  unsigned char dmy[MAX_DELAY_LEN];

  len = ( (double)us * saved_rate * 16/3 ) / 1000000.0;
  if (len < 1) len = 1;
  while (len > 0) {
    sub_len = len;
    if (sub_len > DELAY_LEN)
	sub_len = DELAY_LEN;
    len -= sub_len;

    set_pgmled(buf, led_on);
    led_on = !led_on;
    memset(buf+1, ft245r_data, sub_len-1);
    set_pgmled(buf+sub_len -1, 0);

    ft245r_send (pgm, buf, sub_len);
    send_count++;
    if (send_count > REQ_OUTSTANDINGS) {
	ft245r_recv (pgm, dmy, DELAY_LEN);
        recv_count++;
    }
  }
  while (send_count > recv_count) {
        recv_count++;
	ft245r_recv (pgm, dmy,(send_count==recv_count)?sub_len:DELAY_LEN);
  }
#else
  usleep(us);
#endif
  return 0;
}

static int ft245r_cmd(PROGRAMMER * pgm, unsigned char cmd[4], 
                      unsigned char res[4]);
/*
 * issue the 'program enable' command to the AVR device
 */
static int ft245r_program_enable_local(PROGRAMMER * pgm, AVRPART * p, int noreset)
{
  int retry_count = 0;
  unsigned char cmd[4];
  unsigned char res[4];
  int i,reset_ok;

  ft245r_set_bitclock(pgm, pgm->bitclock);

retry:
  reset_ok = 0;

  ft245r_ddr |= ft245r_rdyled;
  ft245r_ddr_cbus |= ft245r_rdyled_cbus;
  set_rdyled(pgm);

  if (!noreset) {
    ft245r_delay(pgm, 5000); // 5ms
    pulse_reset(pgm, 8); // H  8 * 0.156 ms -> L 
    ft245r_delay(pgm, 20000); // 20ms
  }

  for (i=0; i<32; i++) {
     cmd[0] = 0xAC;
     cmd[1] = 0x53;
     cmd[2] = 0;
     cmd[3] = 0;
     ft245r_cmd(pgm, cmd, res); 
     ft245r_delay(pgm, 20000); // 20ms
     if (res[2] == 0x53 ) {
        reset_ok = 1;
        break;
     }
     pulse_sck(pgm);
     ft245r_delay(pgm, 20000);
  }
  if (reset_ok) {
     for (i=0; i<3; i++) {
        cmd[0] = 0x30;
        cmd[1] = 0;
        cmd[2] = i;
        cmd[3] = 0;
        ft245r_cmd(pgm, cmd, res); 
        ft245r_delay(pgm, 20000); // 20ms
        saved_signature[i] = res[3];
     }
     if (saved_signature[0] == 0x1e) { // success
        return 0;
     }
  }

  retry_count++;
  if (!noreset && (retry_count <= 5)) {
	  goto retry;
  }
  if (noreset) {
        if (retry_count <= 5) goto retry;
	goto err_ret;
  }
  if ((verbose>=1) || FT245R_DEBUG) {
     fprintf(stderr,
	    "%s: ft245r_program_enable: failed\n", progname);
     fflush(stderr);
  }
err_ret:
  ft245r_ddr &= ~ft245r_rdyled;
  ft245r_ddr_cbus &= ~ft245r_rdyled_cbus;
  set_rdyled(pgm);
  return -1;
}

static int ft245r_program_enable(PROGRAMMER * pgm, AVRPART * p)
{
  int r;
  r = ft245r_program_enable_local(pgm, p, 1);
  if (r) {
      r = ft245r_program_enable_local(pgm, p, 0);
  }
  return r;
}

static int ft245r_read_sig_bytes(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m)
{
   m->buf[0] = saved_signature[0];
   m->buf[1] = saved_signature[1];
   m->buf[2] = saved_signature[2];
   return 3;
}

/*
 * initialize the AVR device and prepare it to accept commands
 */
static int ft245r_initialize(PROGRAMMER * pgm, AVRPART * p)
{
  return ft245r_program_enable(pgm, p);
}

static void ft245r_disable(PROGRAMMER * pgm)
{
  return;
}


static void ft245r_enable(PROGRAMMER * pgm)
{
  /* Do nothing. */

  return;
}

static inline int set_data(unsigned char *buf, unsigned char data)
{
    int j;
    int buf_pos = 0;
    unsigned char bit = 0x80;

    for (j=0; j<8; j++) {
	if (data & bit) ft245r_data |= ft245r_mosi;
	else            ft245r_data &= ~ft245r_mosi;

    	buf[buf_pos] = ft245r_data;
	buf_pos++;

    	buf[buf_pos] = ft245r_data | ft245r_sck;
	buf_pos++;

	bit >>= 1;
    }
    return buf_pos;
}

static inline unsigned char extract_data(unsigned char *buf, int offset)
{
    int j;
    int buf_pos = 1;
    unsigned char bit = 0x80;
    unsigned char r = 0;

    buf += offset * (8 * FT245R_CYCLES);
    for (j=0; j<8; j++) {
	if (buf[buf_pos] & ft245r_miso) {
		  r |= bit;
	}
	buf_pos += FT245R_CYCLES;
	bit >>= 1;
    }
    return r;
}

/* to check data */
static inline unsigned char extract_data_out(unsigned char *buf, int offset)
{
    int j;
    int buf_pos = 1;
    unsigned char bit = 0x80;
    unsigned char r = 0;

    buf += offset * (8 * FT245R_CYCLES);
    for (j=0; j<8; j++) {
	if (buf[buf_pos] & ft245r_mosi) {
		  r |= bit;
	}
	buf_pos += FT245R_CYCLES;
	bit >>= 1;
    }
    return r;
}

 
/*
 * transmit an AVR device command and return the results; 'cmd' and
 * 'res' must point to at least a 4 byte data buffer
 */
static int ft245r_cmd(PROGRAMMER * pgm, unsigned char cmd[4], 
                      unsigned char res[4])
{
  int i,buf_pos, pos;
  unsigned char buf[128]; // real usage ; 4 * 8 * 2 + 2

  buf_pos = 0;
  buf_pos += set_pgmled(buf+buf_pos, 1);
  pos = buf_pos;
  for (i=0; i<4; i++) {
     buf_pos += set_data(buf+buf_pos, cmd[i]);
  }
  buf_pos += set_pgmled(buf+buf_pos, 0);

  ft245r_send (pgm, buf, buf_pos);
  ft245r_recv (pgm, buf, buf_pos);
  res[0] = extract_data(buf+pos, 0);
  res[1] = extract_data(buf+pos, 1);
  res[2] = extract_data(buf+pos, 2);
  res[3] = extract_data(buf+pos, 3);

  return 0;
}

#if defined(USE_LIBFTDI)

#define FTDI_VID	0x0403
#define FTDI_PID	0x6001

static FT_STATUS ftdi_open_devnum(int devnum, struct ftdi_context **handlep) {
     FT_STATUS r;
     struct ftdi_context *h = NULL;
     struct ftdi_device_list *devlist = NULL, *p;
     int i;

     r = FT_ERROR;
     h = ftdi_new();
     if (!h) goto out;

     r = ftdi_usb_find_all(h, &devlist, FTDI_VID, FTDI_PID);
     if (r < 0) {
	goto out;
     }
     r = FT_ERROR;
     for (p = devlist,i=0; p; p = p->next,i++) {
	if (i == devnum) {
            r = ftdi_usb_open_dev(h, p->dev);
	    if (r == FT_OK) {
		break;
	    }
        }
     }
out:
     if (devlist) ftdi_list_free(&devlist);
     if (r == FT_OK) {
	*handlep = h;
     } else {
	if (h) ftdi_free(h);
     }
     return r;
}

static FT_STATUS ftdi_open_name(char *name, struct ftdi_context **handlep) {
     FT_STATUS r;
     struct ftdi_context *h = NULL;

     r = FT_ERROR;
     h = ftdi_new();
     if (!h) goto out;
     r = ftdi_usb_open_desc(h, FTDI_VID, FTDI_PID, name, NULL);
     if (r != FT_OK) {
         r = ftdi_usb_open_desc(h, FTDI_VID, FTDI_PID, NULL, name);
     }
out:
     if (r == FT_OK) {
	*handlep = h;
     } else {
	if (h) ftdi_free(h);
     }
     return r;
}
#endif

static int ft245r_open(PROGRAMMER * pgm, char * port)
{
  FT_STATUS r = FT_OK;
  int devnum = -1;

  strcpy(pgm->port, port);

  if ((strlen(port) == 3) && !strncasecmp("ft", port, 2) && '0' <= port[2] && port[2] <= '9') {
     devnum = port[2] - '0';
#if !defined(USE_LIBFTDI)
     r = FT_Open(devnum, &handle);
#else
     r = ftdi_open_devnum(devnum, &handle);
#endif
  } else {
#if !defined(USE_LIBFTDI)
     r = FT_OpenEx(port, FT_OPEN_BY_DESCRIPTION, &handle);
     if (r != FT_OK) {
        r = FT_OpenEx(port, FT_OPEN_BY_SERIAL_NUMBER, &handle);
     }
#else
     r = ftdi_open_name(port, &handle);
#endif
     if (r != FT_OK) {
        fprintf(stderr,
	    "%s: invalid portname  %s: use ft0 - ft9\n",progname,port);
        exit(1);
     }
  }
  if (r != FT_OK) {
    fprintf(stderr,
	    "%s: %s open failed \n",
	    progname, port);
    exit(1);
  }
#if !defined(USE_LIBFTDI)
  if (pgm->ispdelay > 1) {
  	r = FT_SetLatencyTimer(handle, pgm->ispdelay);
  } else {
  	r = FT_SetLatencyTimer(handle, 2);
  }
  r = FT_SetBitMode(handle, 0, FT_BITMODE_SYNC_BITBANG);
#else
  r = ftdi_set_bitmode(handle, 0, BITMODE_SYNCBB);
#endif
  if (r != FT_OK) {
    fprintf(stderr,
	    "%s: Synchronuse BitBangMode is not supported\n",
	    progname);
     exit(1);
  }
#if FT245R_DEBUG
  fprintf(stderr, "%s: BitBang OK \n", progname);
  fflush(stderr);
#endif
  ft245r_ddr = 0;
  ft245r_data = 0;
  ft245r_sck = 0;
  ft245r_mosi = 0;
  ft245r_reset = 0;
  ft245r_miso = 0;
  setbit(&ft245r_ddr, pgm->pinno[PIN_AVR_SCK], 1);
  setbit(&ft245r_ddr, pgm->pinno[PIN_AVR_MOSI], 1);
  setbit(&ft245r_ddr, pgm->pinno[PIN_AVR_RESET], 1);
  setbit(&ft245r_sck, pgm->pinno[PIN_AVR_SCK], 1);
  setbit(&ft245r_mosi, pgm->pinno[PIN_AVR_MOSI], 1);
  setbit(&ft245r_reset, pgm->pinno[PIN_AVR_RESET], 1);
  setbit(&ft245r_miso, pgm->pinno[PIN_AVR_MISO], 1);
  if ((verbose>=1) || FT245R_DEBUG) {
    fprintf(stderr,
	    "%s: pin assign miso %d sck %d mosi %d reset %d\n",
	      progname,
              pgm->pinno[PIN_AVR_MISO],
              pgm->pinno[PIN_AVR_SCK],
              pgm->pinno[PIN_AVR_MOSI],
              pgm->pinno[PIN_AVR_RESET]);
  }
  ft245r_ddr_cbus = 0;
  // ft245r_data_cbus = 0;
  ft245r_rdyled = 0;
  ft245r_rdyled_cbus = 0;
  ft245r_pgmled = 0;
  ft245r_pgmled_cbus = 0;
  if (pgm->pinno[PIN_LED_RDY] > 0) {
     if (pgm->pinno[PIN_LED_RDY] < 8)
         setbit(&ft245r_rdyled, pgm->pinno[PIN_LED_RDY], 1);
     else
         setbit(&ft245r_rdyled_cbus, pgm->pinno[PIN_LED_RDY] - 8, 1);
  }
  if (pgm->pinno[PIN_LED_PGM] > 0) {
     if (pgm->pinno[PIN_LED_PGM] < 8)
        setbit(&ft245r_pgmled, pgm->pinno[PIN_LED_PGM], 1);
     else
        setbit(&ft245r_pgmled_cbus, pgm->pinno[PIN_LED_PGM] - 8, 1);
  }
  ft245r_ddr |= ft245r_rdyled | ft245r_pgmled;
  ft245r_data |= ft245r_rdyled | ft245r_pgmled;
  if (((verbose>=1) || FT245R_DEBUG) && 
     (ft245r_rdyled || ft245r_rdyled_cbus || ft245r_pgmled || ft245r_pgmled_cbus)) {
    fprintf(stderr,
	    "%s: led pin assign rdy %d pgm %d \n",
	      progname,
              pgm->pinno[PIN_LED_RDY]?pgm->pinno[PIN_LED_RDY]:-1,
              pgm->pinno[PIN_LED_PGM]?pgm->pinno[PIN_LED_PGM]:-1);
  }

  /*
   * drain any extraneous input
   */
  ft245r_drain (pgm, 0);
#if FT245R_DEBUG
  fprintf(stderr, "%s: drain OK \n", progname);
  fflush(stderr);
#endif

  is_hispeed = 0;
  valid_rates = valid_rates_r;
  valid_rates_num = sizeof(valid_rates_r)/sizeof(valid_rates_r[0]);
#if defined(USE_LIBFTDI)
#else
{
  FT_DEVICE ft_type;
  DWORD ft_id;
  char sn[16];
  char desc[64];
  r = FT_GetDeviceInfo(handle, &ft_type, &ft_id, sn, desc, NULL);
  if ( (r == FT_OK) && (ft_type >= FT_DEVICE_2232H) ) {
     is_hispeed = 1;
     valid_rates = valid_rates_h;
     valid_rates_num = sizeof(valid_rates_h)/sizeof(valid_rates_h[0]);
  }
}
#endif
  return 0;
}

/*
 * parse the -E string
 */
static int ft245r_parseexitspecs(PROGRAMMER * pgm, char *s)
{
  char *cp;

  pgm->exit_reset = EXIT_RESET_DISABLED; // default
  pgm->exit_vcc = EXIT_VCC_DISABLED;
  while ((cp = strtok(s, ","))) {
    if (strcmp(cp, "reset") == 0) {
      pgm->exit_reset = EXIT_RESET_ENABLED;
    }
    else if (strcmp(cp, "noreset") == 0) {
      pgm->exit_reset = EXIT_RESET_DISABLED;
    }
    else {
      return -1;
    }
    s = 0; /* strtok() should be called with the actual string only once */
  }

  return 0;
}

static void ft245r_close(PROGRAMMER * pgm)
{
  ft245r_ddr &=  ~ft245r_sck;
  ft245r_ddr &=  ~ft245r_mosi;
  if (pgm->exit_reset == EXIT_RESET_ENABLED) { // -E reset
     ft245r_ddr &=  ~ft245r_reset;

     ft245r_ddr &=  ~ft245r_rdyled;
     ft245r_ddr &=  ~ft245r_pgmled;
     ft245r_ddr_cbus &=  ~ft245r_rdyled_cbus;
     ft245r_ddr_cbus &=  ~ft245r_pgmled_cbus;
  }
  set_rdyled(pgm);
  if (pgm->exit_reset == EXIT_RESET_ENABLED) { // -E reset
#if !defined(USE_LIBFTDI)
     FT_SetBitMode(handle, 0, FT_BITMODE_RESET);
#else
     ftdi_set_bitmode(handle, 0, BITMODE_RESET);
#endif
  }
#if !defined(USE_LIBFTDI)
  FT_SetLatencyTimer(handle, 16); // default
  FT_Close(handle);  
#else
  ftdi_usb_close(handle);
  ftdi_free(handle);
  handle = NULL;
#endif
}

static void ft245r_display(PROGRAMMER * pgm, const char * p)
{
  return;
}

static int ft245r_paged_write_gen(PROGRAMMER * pgm, AVRPART * p,
                                     AVRMEM * m, int page_size, int n_bytes)
{
  unsigned long    i;
  int rc;

  for (i=0; i<n_bytes; i++) {
    report_progress(i, n_bytes, NULL);

    rc = avr_write_byte_default(pgm, p, m, i, m->buf[i]);
    if (rc != 0) {
      i = -2;
      goto out;
    }

    if (m->paged) {
      /*
       * check to see if it is time to flush the page with a page
       * write
       */
      if (((i % m->page_size) == m->page_size-1) || (i == n_bytes-1)) {
        rc = avr_write_page(pgm, p, m, i);
        if (rc != 0) {
          i = -2;
          goto out;
        }
      }
    }
  }
out:
  return i;
}

static struct ft245r_request {
	int addr;
	int bytes;
	int n;
	struct ft245r_request *next;
} *req_head,*req_tail,*req_pool;

static void put_request(int addr, int bytes, int n)
{
  struct ft245r_request *p;
  if (req_pool) {
     p = req_pool;
     req_pool = p->next;
  } else {
     p = malloc(sizeof(struct ft245r_request));
     if (!p) {
       fprintf(stderr, "can't alloc memory\n");
       exit(1);
     }
  }
  memset(p, 0, sizeof(struct ft245r_request));
  p->addr = addr;
  p->bytes = bytes;
  p->n = n;
  if (req_tail) {
     req_tail->next = p;
     req_tail = p;
  } else {
     req_head = req_tail = p;
  }
}

static int do_request(PROGRAMMER * pgm, AVRMEM *m)
{
  struct ft245r_request *p;
  int addr, bytes, j, n;
  unsigned char buf[FT245R_MAX_FRAGMENT_SIZE+2+128];

  if (!req_head) return 0;
  p = req_head;
  req_head = p->next;
  if (!req_head) req_tail = req_head;

  addr = p->addr;
  bytes = p->bytes;
  n = p->n;
  memset(p, 0, sizeof(struct ft245r_request));
  p->next = req_pool;
  req_pool = p;

  ft245r_recv(pgm, buf, bytes);
  for (j=0; j<n; j++) {
     m->buf[addr++] = extract_data(buf+1 , (j * 4 + 3)); // offset 1
  }
#if 0
if (n == 0) // paged_write
fprintf(stderr, "recv addr 0x%04x buf size %d \n",addr, bytes);
#endif
  return 1;
}

static int ft245r_paged_write_flash(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m, 
                                    int page_size, int n_bytes)
{
  unsigned int    i,j;
  int addr,addr_save,buf_pos,do_page_write,req_count;
  unsigned int mask;
  int led_on = 1;
  unsigned char buf[FT245R_MAX_FRAGMENT_SIZE+2+128]; // 128 means 8 * 8 * 2

  req_count = 0;
  addr = 0;
  mask = m->page_size -1;
  for (i=0; i<n_bytes; ) {
     addr_save = addr;
     buf_pos = 0;
     do_page_write = 0;
     buf_pos += set_pgmled(buf+buf_pos, led_on);
     led_on = !led_on;
     for (j=0; j< FT245R_FRAGMENT_SIZE/8/FT245R_CYCLES/4; j++) {
        buf_pos += set_data(buf+buf_pos, (addr & 1)?0x48:0x40 ); 
        buf_pos += set_data(buf+buf_pos, (addr >> 9) & 0xff ); 
        buf_pos += set_data(buf+buf_pos, (addr >> 1) & 0xff );
        buf_pos += set_data(buf+buf_pos, m->buf[i]);
	addr ++;
	i++;
	if (((i % m->page_size) == 0) || (i == n_bytes)) {
		do_page_write = 1;
		break;
	}
     }
#if defined(USE_INLINE_WRITE_PAGE)
     if (do_page_write) {
        int addr_wk = addr_save & ~mask;
        /* If this device has a "load extended address" command, issue it. */
	if (m->op[AVR_OP_LOAD_EXT_ADDR]) {
	    unsigned char cmd[4];
	    OPCODE *lext = m->op[AVR_OP_LOAD_EXT_ADDR];

	    memset(cmd, 0, 4);
	    avr_set_bits(lext, cmd);
	    avr_set_addr(lext, cmd, addr_wk/2);
            buf_pos += set_data(buf+buf_pos, cmd[0]);
            buf_pos += set_data(buf+buf_pos, cmd[1]);
            buf_pos += set_data(buf+buf_pos, cmd[2]);
            buf_pos += set_data(buf+buf_pos, cmd[3]);
	}
        buf_pos += set_data(buf+buf_pos, 0x4C); /* Issue Page Write */
        buf_pos += set_data(buf+buf_pos,(addr_wk >> 9) & 0xff); 
        buf_pos += set_data(buf+buf_pos,(addr_wk >> 1) & 0xff); 
        buf_pos += set_data(buf+buf_pos, 0);
     }
#endif
     if (i >= n_bytes) {
        buf_pos += set_pgmled(buf+buf_pos, 0); // sck down
     }
     ft245r_send(pgm, buf, buf_pos);
     put_request(addr_save, buf_pos, 0);
     //ft245r_sync(pgm);
#if 0
fprintf(stderr, "send addr 0x%04x bufsize %d [%02x %02x] page_write %d\n",
		addr_save,buf_pos,
		extract_data_out(buf+1 , (0*4 + 3) ),
		extract_data_out(buf+1 , (1*4 + 3) ),
		do_page_write);
#endif
     req_count++;
     if (req_count > REQ_OUTSTANDINGS)
        do_request(pgm, m);
     if (do_page_write) {
#if defined(USE_INLINE_WRITE_PAGE)
        while (do_request(pgm, m))
	     ;
        ft245r_delay(pgm, m->max_write_delay);
#else
        int addr_wk = addr_save & ~mask;
	int rc;
        while (do_request(pgm, m))
	     ;
        rc = avr_write_page(pgm, p, m, addr_wk);
        if (rc != 0) {
	  return -2;
        }
#endif
        req_count = 0;
     }
     report_progress(i, n_bytes, NULL);
  }
  while (do_request(pgm, m))
	  ;
  return i;
}

static int ft245r_paged_write(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m,
                              int page_size, int n_bytes)
{
  int ret;
  ft245r_ddr |=  ft245r_pgmled;
  ft245r_ddr_cbus |=  ft245r_pgmled_cbus;
  ft245r_ddr &=  ~ft245r_rdyled;
  ft245r_ddr_cbus &=  ~ft245r_rdyled_cbus;

  if ((strcmp(m->desc, "flash") == 0) && (m->paged)) {
    ret = ft245r_paged_write_flash(pgm, p, m, page_size, n_bytes);
  }
  else if ((strcmp(m->desc, "flash")==0) || (strcmp(m->desc, "eeprom")==0)) {
    ret = ft245r_paged_write_gen(pgm, p, m, page_size, n_bytes);
  }
  else {
    ret = -2;
  }
  ft245r_ddr &=  ~ft245r_pgmled;
  ft245r_ddr_cbus &=  ~ft245r_pgmled_cbus;
  ft245r_ddr |=  ft245r_rdyled;
  ft245r_ddr_cbus |=  ft245r_rdyled_cbus;
  return ret;
}

#if 0
static int ft245r_paged_load_gen(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m, 
                             int page_size, int n_bytes)
{
  unsigned char    rbyte;
  unsigned long    i;
  int rc;

  for (i=0; i<n_bytes; i++) {
     rc = avr_read_byte_default(pgm, p, m, i, &rbyte);
     if (rc != 0) {
       return -2;
     }
     m->buf[i] = rbyte;
     report_progress(i, n_bytes, NULL);
  }
  return n_bytes;
}
#endif

static int ft245r_paged_load_eeprom(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m, 
                             int page_size, int n_bytes)
{
  unsigned long    i,j,n;
  //int rc;
  int addr,addr_save,buf_pos;
  int req_count = 0;
  int led_on = 1;
  unsigned char buf[FT245R_MAX_FRAGMENT_SIZE+2];

  addr = 0;
  for (i=0; i<n_bytes; ) {
     buf_pos = 0;
     addr_save = addr;
     buf_pos += set_pgmled(buf+buf_pos, led_on);
     led_on = !led_on;
     for (j=0; j< FT245R_FRAGMENT_SIZE/8/FT245R_CYCLES/4; j++) {
	if (i >= n_bytes) break;
        buf_pos += set_data(buf+buf_pos, 0xa0 ); 
        buf_pos += set_data(buf+buf_pos, (addr >> 8) & 0xff ); 
        buf_pos += set_data(buf+buf_pos, (addr ) & 0xff );
        buf_pos += set_data(buf+buf_pos, 0);
	addr ++;
	i++;
     }
     if (i >= n_bytes) {
        buf_pos += set_pgmled(buf+buf_pos, 0); // sck down
     }
     n = j;
     ft245r_send(pgm, buf, buf_pos);
     put_request(addr_save, buf_pos, n);
     req_count++;
     if (req_count > REQ_OUTSTANDINGS)
        do_request(pgm, m);
     report_progress(i, n_bytes, NULL);
  }
  while (do_request(pgm, m))
	  ;
  return n_bytes;
}

static int ft245r_paged_load_flash(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m, 
                             int page_size, int n_bytes)
{
  unsigned long    i,j,n;
  //int rc;
  int addr,addr_save,buf_pos;
  int req_count = 0;
  int led_on = 1;
  unsigned char buf[FT245R_MAX_FRAGMENT_SIZE+2];

  addr = 0;
  for (i=0; i<n_bytes; ) {
     buf_pos = 0;
     addr_save = addr;
     buf_pos += set_pgmled(buf+buf_pos, led_on);
     led_on = !led_on;
     for (j=0; j< FT245R_FRAGMENT_SIZE/8/FT245R_CYCLES/4; j++) {
	if (i >= n_bytes) break;
        buf_pos += set_data(buf+buf_pos, (addr & 1)?0x28:0x20 ); 
        buf_pos += set_data(buf+buf_pos, (addr >> 9) & 0xff ); 
        buf_pos += set_data(buf+buf_pos, (addr >> 1) & 0xff );
        buf_pos += set_data(buf+buf_pos, 0);
	addr ++;
	i++;
     }
     if (i >= n_bytes) {
        buf_pos += set_pgmled(buf+buf_pos, 0); // sck down
     }
     n = j;
     ft245r_send(pgm, buf, buf_pos);
     put_request(addr_save, buf_pos, n);
     req_count++;
     if (req_count > REQ_OUTSTANDINGS)
        do_request(pgm, m);
     report_progress(i, n_bytes, NULL);
  }
  while (do_request(pgm, m))
	  ;
  return n_bytes;
}

static int ft245r_paged_load(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m, 
                             int page_size, int n_bytes)
{
  if (strcmp(m->desc, "flash") == 0) {
    return ft245r_paged_load_flash(pgm, p, m, page_size, n_bytes);
  }
  else if (strcmp(m->desc, "eeprom") == 0) {
    return ft245r_paged_load_eeprom(pgm, p, m, page_size, n_bytes);
  }
  else {
    return -2;
  }
}

void ft245r_initpgm(PROGRAMMER * pgm)
{
  strcpy(pgm->type, "ft245r");

  /*
   * mandatory functions
   */
  pgm->initialize     = ft245r_initialize;
  pgm->display        = ft245r_display;
  pgm->enable         = ft245r_enable;
  pgm->disable        = ft245r_disable;
  pgm->program_enable = ft245r_program_enable;
  pgm->chip_erase     = ft245r_chip_erase;
  pgm->cmd            = ft245r_cmd;
  pgm->open           = ft245r_open;
  pgm->close          = ft245r_close;
  pgm->read_byte      = avr_read_byte_default;
  pgm->write_byte     = avr_write_byte_default;

  /*
   * optional functions
   */
  pgm->paged_write = ft245r_paged_write;
  pgm->paged_load = ft245r_paged_load;

  pgm->read_sig_bytes = ft245r_read_sig_bytes;
  pgm->set_sck_period = ft245r_set_bitclock;
  pgm->parseexitspecs = ft245r_parseexitspecs;

}
#else
static int ft245r_initialize(PROGRAMMER * pgm, AVRPART * p)
{
	return -1;
}

void ft245r_initpgm(PROGRAMMER * pgm)
{
  pgm->initialize     = ft245r_initialize;
}

#endif
