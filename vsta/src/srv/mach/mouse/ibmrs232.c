/*
 * ibmrs232.c
 *    Polling driver for the IBM serial mouse.
 *
 * Copyright (C) 1993 by G.T.Nicol, all rights reserved.
 *
 * A lot of this code came from the Linux/MGR mouse driver code.
 */

#include <stdio.h>
#include <std.h>
#include <sys/mman.h>
#include "machine.h"
#include "mouse.h"

/*
 * Our function table
 */
static mouse_function_table ibm_serial_functions = {
   ibm_serial_poller_entry_point,	 /* mouse_poller_entry_point */
   NULL,				 /* mouse_interrupt          */
   ibm_serial_coordinates,		 /* mouse_coordinates        */
   ibm_serial_bounds,			 /* mouse_bounds */
   ibm_serial_update_period,		 /* mouse_update_period      */
};

/*
 * General data used by the driver
 */
static ushort      ibm_serial_delay_period    = 100;
static uint        ibm_serial_update_allowed  = TRUE;
static uint        ibm_serial_baud_rate       = 1200;
static uint        ibm_serial_port_number     = 0;
static ibm_model_t ibm_serial_model           = RS_UNKNOWN;
static uint        ibm_serial_iobase          = 0;

static uchar ibm_serial_data[6][5] =
{
   /* hd_mask hd_id   dp_mask dp_id   nobytes */
   {0x00, 0x00, 0x00, 0x00, 0,},	 /* Unknown        */
   {0x40, 0x40, 0x40, 0x00, 3,},	 /* MicroSoft      */
   {0xf8, 0x80, 0x00, 0x00, 3,},	 /* MouseSystems 3 */
   {0xf8, 0x80, 0x00, 0x00, 5,},	 /* MouseSystems 5 */
   {0xe0, 0x80, 0x80, 0x00, 3,},	 /* MMSeries       */
   {0xe0, 0x80, 0x80, 0x00, 3,},	 /* Logitech       */
};

/*
 * rs232_init()
 *	Initialise the com port to the baud specified.
 */
static void
rs232_init(uint port, uint baud)
{
   uchar hi_baud = 0x0;
   uchar lo_baud = 0x0C;

   switch(baud){
   case  300:
   case 1200:
   case 2400:
   case 4800:
      fprintf(stderr,"mouse: Unknown baud rate. Using 9600\n");
   case 9600:
      hi_baud  = 0x0;
      lo_baud  = 0x0C;
      break;
   default:
      fprintf(stderr,"mouse: Unknown baud rate. Using 9600\n");
      hi_baud  = 0x0;
      lo_baud  = 0x0C;
   }
   outportb(IBM_RS_LINEREG, 0x80);        /* 9600 baud */
   outportb(IBM_RS_HIBAUD,  hi_baud);
   outportb(IBM_RS_LOWBAUD, lo_baud);
   outportb(IBM_RS_LINEREG, 3);           /* 8 bits, one stop */
}

/*
 * rs232_putc()
 *	Busy-wait and then send a character
 */
static void
rs232_putc(int c)
{
   while ((inportb(IBM_RS_LINESTAT) & 0x20) == 0)
      ;
   outportb(IBM_RS_DATA, c & 0x7F);
}

/*
 * rs232_getc()
 *	Busy-wait and return next character
 */
static int
rs232_getc(void)
{
   while ((inportb(IBM_RS_LINESTAT) & 1) == 0)
      ;
   return(inportb(IBM_RS_DATA) & 0x7F);
}

/*
 * ibm_serial_check_status()
 *    Read in the mouse coordinates.
 */
static inline void
ibm_serial_check_status(void)
{
   short new_x, new_y;
   unsigned char buffer[5];
   char x_off, y_off;
   int i, buttons;

   uchar *m_data = ibm_serial_data[ibm_serial_model];

   buffer[0] = rs232_getc();

restart:
   /* find a header packet */
   while ((buffer[0] & m_data[0]) != m_data[1]) {
      buffer[0] = rs232_getc();
   }

   /* read in the rest of the packet */
   for (i = 1; i < m_data[4]; ++i) {
      buffer[i] = rs232_getc();
      /* check whether it's a data packet */
      if ((buffer[i] & m_data[2]) != m_data[3] || buffer[i] == 0x80)
	 goto restart;
   }

   set_semaphore(&ibm_serial_update_allowed, FALSE);
   new_x = mouse_data.pointer_data.x;
   new_y = mouse_data.pointer_data.y;

   switch (ibm_serial_model) {
   case RS_MICROSOFT:			 /* Microsoft */
   default:
      buttons = ((buffer[0] & 0x20) >> 3) | ((buffer[0] & 0x10) >> 4);
      x_off = (char) (((buffer[0] & 0x03) << 6) | (buffer[1] & 0x3F));
      y_off = (char) (((buffer[0] & 0x0C) << 4) | (buffer[2] & 0x3F));
      break;
   case RS_MOUSE_SYS_3:		 /* Mouse Systems 3 byte */
      buttons = (~buffer[0]) & 0x07;
      x_off = (char) (buffer[1]);
      y_off = -(char) (buffer[2]);
      break;
   case RS_MOUSE_SYS_5:		 /* Mouse Systems Corp 5 bytes */
      buttons = (~buffer[0]) & 0x07;
      x_off = (char) (buffer[1]) + (char) (buffer[3]);
      y_off = -((char) (buffer[2]) + (char) (buffer[4]));
      break;
   case RS_MM:				 /* MM Series */
   case RS_LOGITECH:			 /* Logitech */
      buttons = buffer[0] & 0x07;
      x_off = (buffer[0] & 0x10) ? buffer[1] : -buffer[1];
      y_off = (buffer[0] & 0x08) ? -buffer[2] : buffer[2];
      break;
   };

   /*
    *  If they've changed, update  the current coordinates
    */
   if (x_off != 0 || y_off != 0) {
      new_x += x_off;
      new_y += y_off;
      /*
         Make sure we honour the bounding box
      */
      if (new_x < mouse_data.pointer_data.bx1)
	 new_x = mouse_data.pointer_data.bx1;
      if (new_x > mouse_data.pointer_data.bx2)
	 new_x = mouse_data.pointer_data.bx2;
      if (new_y < mouse_data.pointer_data.by1)
	 new_y = mouse_data.pointer_data.by1;
      if (new_y > mouse_data.pointer_data.by2)
	 new_y = mouse_data.pointer_data.by2;
      /*
         Set up the new mouse position
      */
      mouse_data.pointer_data.x = new_x;
      mouse_data.pointer_data.y = new_y;
   }
   switch (buttons) {			 /* simulate a 3 button mouse here */
   case 4:
      mouse_data.pointer_data.buttons = (1 << 3);	/* left   */
      break;
   case 1:
      mouse_data.pointer_data.buttons = (1 << 1);	/* right  */
      break;
   case 0:
      mouse_data.pointer_data.buttons = (1 << 2);	/* middle */
      break;
   default:
      mouse_data.pointer_data.buttons = 0;	/* none   */
   };
   set_semaphore(&ibm_serial_update_allowed, TRUE);
}

/*
 * ibm_serial_poller_entry_point()
 *    Main loop of the polling thread.
 */
void
ibm_serial_poller_entry_point(void)
{
   for (;;) {
      ibm_serial_check_status();
      if (ibm_serial_delay_period) {
	 __msleep(ibm_serial_delay_period);
      }
   }
}

/*
 * ibm_serial_bounds()
 *    Change or read the mouse bounding box.
 */
void
ibm_serial_bounds(ushort x1, ushort y1, ushort x2, ushort y2)
{
   while (!ibm_serial_update_allowed);
   set_semaphore(&ibm_serial_update_allowed, FALSE);
   mouse_data.pointer_data.bx1 = x1;
   mouse_data.pointer_data.bx2 = x2;
   mouse_data.pointer_data.by1 = y1;
   mouse_data.pointer_data.by2 = y2;
   set_semaphore(&ibm_serial_update_allowed, TRUE);
}

/*
 * ibm_serial_coordinates()
 *    Change or read the mouse coordinates.
 */
void
ibm_serial_coordinates(ushort x, ushort y)
{
   while (!ibm_serial_update_allowed);
   set_semaphore(&ibm_serial_update_allowed, FALSE);
   mouse_data.pointer_data.x = x;
   mouse_data.pointer_data.y = y;
   set_semaphore(&ibm_serial_update_allowed, TRUE);
}

/*
 * ibm_serial_update_period()
 *    Read the mouse button status
 */
void
ibm_serial_update_period(ushort period)
{
   if (period == 0)
      period = 5;
   set_semaphore(&ibm_serial_update_allowed, FALSE);
   ibm_serial_delay_period = 1000 / period;
   mouse_data.update_frequency = period;
   set_semaphore(&ibm_serial_update_allowed, TRUE);

   if (ibm_serial_model == RS_LOGITECH) {
      rs232_putc('S');
   }
   if (period <= 0)
      rs232_putc('O');
   else if (period <= 15)
      rs232_putc('J');
   else if (period <= 27)
      rs232_putc('K');
   else if (period <= 42)
      rs232_putc('L');
   else if (period <= 60)
      rs232_putc('R');
   else if (period <= 85)
      rs232_putc('M');
   else if (period <= 125)
      rs232_putc('Q');
   else
      rs232_putc('N');
}

/*
 * ibm_serial_usage()
 *    Infor the user how to use the ibm_serial bus driver
 */
static void
ibm_serial_usage(void)
{
   fprintf(stderr,
	   "Usage: mouse [-baud #] [-port #] [-delay #] [-microsoft] [-mouse_sys_3] [-mouse_sys_5] [-mm] [-logitech]\n");
   exit(1);
}

/*
 * ibm_serial_parse_args()
 *    Parse the command line...
 */
static void
ibm_serial_parse_args(int argc, char **argv)
{
   int microsoft_option = 0, mouse_sys_3_option = 0, mouse_sys_5_option = 0;
   int mm_option = 0, logitech_option = 0;
   int arg;

   for (arg = 1; arg < argc; arg++) {
      if (argv[arg][0] == '-') {
	 if (strcmp(&argv[arg][1], "baud") == 0) {
	    if (++arg == argc)
	       ibm_serial_usage();
	    ibm_serial_baud_rate = atoi(argv[arg]);
	 } else if (strcmp(&argv[arg][1], "port") == 0) {
	    if (++arg == argc)
	       ibm_serial_usage();
	    ibm_serial_port_number = atoi(argv[arg]);
	 } else if (strcmp(&argv[arg][1], "delay") == 0) {
	    if (++arg == argc)
	       ibm_serial_usage();
	    ibm_serial_delay_period = atoi(argv[arg]);
	 } else if (strcmp(&argv[arg][1], "microsoft") == 0) {
	    microsoft_option = 1;
	    ibm_serial_model = RS_MICROSOFT;
	 } else if (strcmp(&argv[arg][1], "mouse_sys_3") == 0) {
	    mouse_sys_3_option = 1;
	    ibm_serial_model = RS_MOUSE_SYS_3;
	 } else if (strcmp(&argv[arg][1], "mouse_sys_5") == 0) {
	    mouse_sys_5_option = 1;
	    ibm_serial_model = RS_MOUSE_SYS_5;
	 } else if (strcmp(&argv[arg][1], "mm") == 0) {
	    mm_option = 1;
	    ibm_serial_model = RS_MM;
	 } else if (strcmp(&argv[arg][1], "logitech") == 0) {
	    logitech_option = 1;
	    ibm_serial_model = RS_LOGITECH;
	 }
      } else
	 break;
   }

   if (microsoft_option + mouse_sys_3_option + mouse_sys_5_option +
       mm_option + logitech_option > 1) {
      ibm_serial_usage();
   }
}


/*
 * ibm_serial_initialise()
 *    Initialise the mouse system.
 */
int
ibm_serial_initialise(int argc, char **argv)
{
   /*
    *  Initialise the system data.
    */
   mouse_data.functions             = ibm_serial_functions;
   mouse_data.pointer_data.x        = 320;
   mouse_data.pointer_data.y        = 320;
   mouse_data.pointer_data.buttons  = 0;
   mouse_data.pointer_data.bx1      = 0;
   mouse_data.pointer_data.by1      = 0;
   mouse_data.pointer_data.bx2      = 639;
   mouse_data.pointer_data.by2      = 399;
   mouse_data.irq_number            = 0;
   mouse_data.update_frequency = ibm_serial_delay_period;

   ibm_serial_parse_args(argc, argv);
   ibm_serial_iobase = 0x2f0 + (ibm_serial_port_number * 0x100);

   /*
    * Get our hardware ports.
    */
   if (enable_io(IBM_RS_LOW_PORT, IBM_RS_HIGH_PORT) < 0) {
      fprintf(stderr, "Mouse: Unable to enable I/O ports for mouse.\n");
      return (-1);
   }

   rs232_init(ibm_serial_port_number, ibm_serial_baud_rate);

   /*
    *  Make doubly sure that we have every ready for polling.
    */
   mouse_data.functions.mouse_interrupt = NULL;
   mouse_data.enable_interrupts = FALSE;
   ibm_serial_update_period(mouse_data.update_frequency);

   return (0);
}
