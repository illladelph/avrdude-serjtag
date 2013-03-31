#ifndef serjtag_h
#define serjtag_h

#include "avrpart.h"

#define JTAG_BUFSIZE	64

/* flags */
#define JTAG_RECIEVE	0x80
#define JTAG_TMS_HIGH	0x40
#define JTAG_USE_DELAY	0x10
#define JTAG_BITS	0x7

/* 's' command (JTAG_SET) flags */
#define JTAG_SET_TDI	0x80
#define JTAG_SET_TMS	0x40
#define JTAG_SET_TCK	0x20
#define JTAG_SET_DELAY	0x0f

#ifdef __cplusplus
extern "C" {
#endif

void serjtag_initpgm (PROGRAMMER * pgm);

#ifdef __cplusplus
}
#endif

#endif /* serjtag_h */
