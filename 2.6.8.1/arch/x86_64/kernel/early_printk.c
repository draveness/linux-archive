#include <linux/console.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/string.h>
#include <asm/io.h>
#include <asm/processor.h>

/* Simple VGA output */

#ifdef __i386__
#define VGABASE		__pa(__PAGE_OFFSET + 0xb8000UL)
#else
#define VGABASE		0xffffffff800b8000UL
#endif

#define MAX_YPOS	25
#define MAX_XPOS	80

static int current_ypos = 1, current_xpos = 0; 

static void early_vga_write(struct console *con, const char *str, unsigned n)
{
	char c;
	int  i, k, j;

	while ((c = *str++) != '\0' && n-- > 0) {
		if (current_ypos >= MAX_YPOS) {
			/* scroll 1 line up */
			for (k = 1, j = 0; k < MAX_YPOS; k++, j++) {
				for (i = 0; i < MAX_XPOS; i++) {
					writew(readw(VGABASE + 2*(MAX_XPOS*k + i)),
					       VGABASE + 2*(MAX_XPOS*j + i));
				}
			}
			for (i = 0; i < MAX_XPOS; i++)
				writew(0x720, VGABASE + 2*(MAX_XPOS*j + i));
			current_ypos = MAX_YPOS-1;
		}
		if (c == '\n') {
			current_xpos = 0;
			current_ypos++;
		} else if (c != '\r')  {
			writew(((0x7 << 8) | (unsigned short) c),
			       VGABASE + 2*(MAX_XPOS*current_ypos +
						current_xpos++));
			if (current_xpos >= MAX_XPOS) {
				current_xpos = 0;
				current_ypos++;
			}
		}
	}
}

static struct console early_vga_console = {
	.name =		"earlyvga",
	.write =	early_vga_write,
	.flags =	CON_PRINTBUFFER,
	.index =	-1,
};

/* Serial functions loosely based on a similar package from Klaus P. Gerlicher */ 

int early_serial_base = 0x3f8;  /* ttyS0 */ 

#define XMTRDY          0x20

#define DLAB		0x80

#define TXR             0       /*  Transmit register (WRITE) */
#define RXR             0       /*  Receive register  (READ)  */
#define IER             1       /*  Interrupt Enable          */
#define IIR             2       /*  Interrupt ID              */
#define FCR             2       /*  FIFO control              */
#define LCR             3       /*  Line control              */
#define MCR             4       /*  Modem control             */
#define LSR             5       /*  Line Status               */
#define MSR             6       /*  Modem Status              */
#define DLL             0       /*  Divisor Latch Low         */
#define DLH             1       /*  Divisor latch High        */

static int early_serial_putc(unsigned char ch) 
{ 
	unsigned timeout = 0xffff; 
	while ((inb(early_serial_base + LSR) & XMTRDY) == 0 && --timeout) 
		cpu_relax();
	outb(ch, early_serial_base + TXR);
	return timeout ? 0 : -1;
} 

static void early_serial_write(struct console *con, const char *s, unsigned n)
{
	while (*s && n-- > 0) { 
		early_serial_putc(*s); 
		if (*s == '\n') 
			early_serial_putc('\r'); 
		s++; 
	} 
} 

#define DEFAULT_BAUD 9600

static __init void early_serial_init(char *opt)
{
	unsigned char c; 
	unsigned divisor;
	unsigned baud = DEFAULT_BAUD;
	char *s, *e;

	if (*opt == ',') 
		++opt;

	s = strsep(&opt, ","); 
	if (s != NULL) { 
		unsigned port; 
		if (!strncmp(s,"0x",2)) {
			early_serial_base = simple_strtoul(s, &e, 16);
		} else {
			static int bases[] = { 0x3f8, 0x2f8 };

			if (!strncmp(s,"ttyS",4))
				s += 4;
			port = simple_strtoul(s, &e, 10);
			if (port > 1 || s == e)
				port = 0;
			early_serial_base = bases[port];
		}
	}

	outb(0x3, early_serial_base + LCR);	/* 8n1 */
	outb(0, early_serial_base + IER);	/* no interrupt */
	outb(0, early_serial_base + FCR);	/* no fifo */
	outb(0x3, early_serial_base + MCR);	/* DTR + RTS */

	s = strsep(&opt, ","); 
	if (s != NULL) { 
		baud = simple_strtoul(s, &e, 0); 
		if (baud == 0 || s == e) 
			baud = DEFAULT_BAUD;
	} 
	
	divisor = 115200 / baud; 
	c = inb(early_serial_base + LCR); 
	outb(c | DLAB, early_serial_base + LCR); 
	outb(divisor & 0xff, early_serial_base + DLL); 
	outb((divisor >> 8) & 0xff, early_serial_base + DLH); 
	outb(c & ~DLAB, early_serial_base + LCR);
}

static struct console early_serial_console = {
	.name =		"earlyser",
	.write =	early_serial_write,
	.flags =	CON_PRINTBUFFER,
	.index =	-1,
};

/* Direct interface for emergencies */
struct console *early_console = &early_vga_console;
static int early_console_initialized = 0;

void early_printk(const char *fmt, ...)
{ 
	char buf[512]; 
	int n; 
	va_list ap;

	va_start(ap,fmt); 
	n = vscnprintf(buf,512,fmt,ap);
	early_console->write(early_console,buf,n);
	va_end(ap); 
} 

static int keep_early; 

int __init setup_early_printk(char *opt) 
{  
	char *space;
	char buf[256]; 

	if (early_console_initialized)
		return -1;

	opt = strchr(opt, '=') + 1;

	strlcpy(buf,opt,sizeof(buf)); 
	space = strchr(buf, ' '); 
	if (space)
		*space = 0; 

	if (strstr(buf,"keep"))
		keep_early = 1; 

	if (!strncmp(buf, "serial", 6)) { 
		early_serial_init(buf + 6);
		early_console = &early_serial_console;
	} else if (!strncmp(buf, "ttyS", 4)) { 
		early_serial_init(buf);
		early_console = &early_serial_console;		
	} else if (!strncmp(buf, "vga", 3)) {
		early_console = &early_vga_console; 
	} else {
		early_console = NULL; 		
		return -1; 
	}
	early_console_initialized = 1;
	register_console(early_console);       
	return 0;
}

void __init disable_early_printk(void)
{ 
	if (!early_console_initialized || !early_console)
		return;
	if (!keep_early) {
		printk("disabling early console\n");
		unregister_console(early_console);
		early_console_initialized = 0;
	} else { 
		printk("keeping early console\n");
	}
} 

__setup("earlyprintk=", setup_early_printk);
