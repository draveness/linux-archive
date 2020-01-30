#ifndef _VT_KERN_H
#define _VT_KERN_H

/*
 * this really is an extension of the vc_cons structure in console.c, but
 * with information needed by the vt package
 */

#include <linux/vt.h>
#include <linux/kd.h>
#include <linux/tty.h>
#include <linux/mutex.h>
#include <linux/console_struct.h>
#include <linux/mm.h>

/*
 * Presently, a lot of graphics programs do not restore the contents of
 * the higher font pages.  Defining this flag will avoid use of them, but
 * will lose support for PIO_FONTRESET.  Note that many font operations are
 * not likely to work with these programs anyway; they need to be
 * fixed.  The linux/Documentation directory includes a code snippet
 * to save and restore the text font.
 */
#ifdef CONFIG_VGA_CONSOLE
#define BROKEN_GRAPHICS_PROGRAMS 1
#endif

extern void kd_mksound(unsigned int hz, unsigned int ticks);
extern int kbd_rate(struct kbd_repeat *rep);
extern int fg_console, last_console, want_console;

/* console.c */

int vc_allocate(unsigned int console);
int vc_cons_allocated(unsigned int console);
int vc_resize(struct vc_data *vc, unsigned int cols, unsigned int lines);
int vc_lock_resize(struct vc_data *vc, unsigned int cols, unsigned int lines);
void vc_deallocate(unsigned int console);
void reset_palette(struct vc_data *vc);
void do_blank_screen(int entering_gfx);
void do_unblank_screen(int leaving_gfx);
void unblank_screen(void);
void poke_blanked_console(void);
int con_font_op(struct vc_data *vc, struct console_font_op *op);
int con_set_cmap(unsigned char __user *cmap);
int con_get_cmap(unsigned char __user *cmap);
void scrollback(struct vc_data *vc, int lines);
void scrollfront(struct vc_data *vc, int lines);
void update_region(struct vc_data *vc, unsigned long start, int count);
void redraw_screen(struct vc_data *vc, int is_switch);
#define update_screen(x) redraw_screen(x, 0)
#define switch_screen(x) redraw_screen(x, 1)

struct tty_struct;
int tioclinux(struct tty_struct *tty, unsigned long arg);

/* consolemap.c */

struct unimapinit;
struct unipair;

int con_set_trans_old(unsigned char __user * table);
int con_get_trans_old(unsigned char __user * table);
int con_set_trans_new(unsigned short __user * table);
int con_get_trans_new(unsigned short __user * table);
int con_clear_unimap(struct vc_data *vc, struct unimapinit *ui);
int con_set_unimap(struct vc_data *vc, ushort ct, struct unipair __user *list);
int con_get_unimap(struct vc_data *vc, ushort ct, ushort __user *uct, struct unipair __user *list);
int con_set_default_unimap(struct vc_data *vc);
void con_free_unimap(struct vc_data *vc);
void con_protect_unimap(struct vc_data *vc, int rdonly);
int con_copy_unimap(struct vc_data *dst_vc, struct vc_data *src_vc);

/* vt.c */
int vt_waitactive(int vt);
void change_console(struct vc_data *new_vc);
void reset_vc(struct vc_data *vc);
extern int unbind_con_driver(const struct consw *csw, int first, int last,
			     int deflt);

/*
 * vc_screen.c shares this temporary buffer with the console write code so that
 * we can easily avoid touching user space while holding the console spinlock.
 */

#define CON_BUF_SIZE (CONFIG_BASE_SMALL ? 256 : PAGE_SIZE)
extern char con_buf[CON_BUF_SIZE];
extern struct mutex con_buf_mtx;
extern char vt_dont_switch;

struct vt_spawn_console {
	spinlock_t lock;
	struct pid *pid;
	int sig;
};
extern struct vt_spawn_console vt_spawn_con;

#endif /* _VT_KERN_H */
