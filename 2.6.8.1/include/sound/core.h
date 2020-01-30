#ifndef __SOUND_CORE_H
#define __SOUND_CORE_H

/*
 *  Main header file for the ALSA driver
 *  Copyright (c) 1994-2001 by Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <linux/sched.h>		/* wake_up() */
#include <asm/semaphore.h>		/* struct semaphore */
#include <linux/rwsem.h>		/* struct rw_semaphore */
#include <linux/workqueue.h>		/* struct workqueue_struct */

/* Typedef's */
typedef struct timespec snd_timestamp_t;
typedef struct sndrv_interval snd_interval_t;
typedef enum sndrv_card_type snd_card_type;
typedef struct sndrv_xferi snd_xferi_t;
typedef struct sndrv_xfern snd_xfern_t;
typedef struct sndrv_xferv snd_xferv_t;

/* forward declarations */
#ifdef CONFIG_PCI
struct pci_dev;
#endif
#ifdef CONFIG_SBUS
struct sbus_dev;
#endif

/* device allocation stuff */

#define SNDRV_DEV_TYPE_RANGE_SIZE		0x1000

typedef enum {
	SNDRV_DEV_TOPLEVEL =		(0*SNDRV_DEV_TYPE_RANGE_SIZE),
	SNDRV_DEV_LOWLEVEL_PRE,
	SNDRV_DEV_LOWLEVEL_NORMAL =	(1*SNDRV_DEV_TYPE_RANGE_SIZE),
	SNDRV_DEV_PCM,
	SNDRV_DEV_RAWMIDI,
	SNDRV_DEV_TIMER,
	SNDRV_DEV_SEQUENCER,
	SNDRV_DEV_HWDEP,
	SNDRV_DEV_INFO,
	SNDRV_DEV_LOWLEVEL =		(2*SNDRV_DEV_TYPE_RANGE_SIZE)
} snd_device_type_t;

typedef enum {
	SNDRV_DEV_BUILD,
	SNDRV_DEV_REGISTERED,
	SNDRV_DEV_DISCONNECTED
} snd_device_state_t;

typedef enum {
	SNDRV_DEV_CMD_PRE = 0,
	SNDRV_DEV_CMD_NORMAL = 1,
	SNDRV_DEV_CMD_POST = 2
} snd_device_cmd_t;

typedef struct _snd_card snd_card_t;
typedef struct _snd_device snd_device_t;

typedef int (snd_dev_free_t)(snd_device_t *device);
typedef int (snd_dev_register_t)(snd_device_t *device);
typedef int (snd_dev_disconnect_t)(snd_device_t *device);
typedef int (snd_dev_unregister_t)(snd_device_t *device);

typedef struct {
	snd_dev_free_t *dev_free;
	snd_dev_register_t *dev_register;
	snd_dev_disconnect_t *dev_disconnect;
	snd_dev_unregister_t *dev_unregister;
} snd_device_ops_t;

struct _snd_device {
	struct list_head list;		/* list of registered devices */
	snd_card_t *card;		/* card which holds this device */
	snd_device_state_t state;	/* state of the device */
	snd_device_type_t type;		/* device type */
	void *device_data;		/* device structure */
	snd_device_ops_t *ops;		/* operations */
};

#define snd_device(n) list_entry(n, snd_device_t, list)

/* various typedefs */

typedef struct snd_info_entry snd_info_entry_t;
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_str snd_pcm_str_t;
typedef struct _snd_pcm_substream snd_pcm_substream_t;
typedef struct _snd_mixer snd_kmixer_t;
typedef struct _snd_rawmidi snd_rawmidi_t;
typedef struct _snd_ctl_file snd_ctl_file_t;
typedef struct _snd_kcontrol snd_kcontrol_t;
typedef struct _snd_timer snd_timer_t;
typedef struct _snd_timer_instance snd_timer_instance_t;
typedef struct _snd_hwdep snd_hwdep_t;
#if defined(CONFIG_SND_MIXER_OSS) || defined(CONFIG_SND_MIXER_OSS_MODULE)
typedef struct _snd_oss_mixer snd_mixer_oss_t;
#endif

/* monitor files for graceful shutdown (hotplug) */

struct snd_monitor_file {
	struct file *file;
	struct snd_monitor_file *next;
};

struct snd_shutdown_f_ops;	/* define it later */

/* main structure for soundcard */

struct _snd_card {
	int number;			/* number of soundcard (index to snd_cards) */

	char id[16];			/* id string of this card */
	char driver[16];		/* driver name */
	char shortname[32];		/* short name of this soundcard */
	char longname[80];		/* name of this soundcard */
	char mixername[80];		/* mixer name */
	char components[80];		/* card components delimited with space */

	struct module *module;		/* top-level module */

	void *private_data;		/* private data for soundcard */
	void (*private_free) (snd_card_t *card); /* callback for freeing of private data */

	struct list_head devices;	/* devices */

	unsigned int last_numid;	/* last used numeric ID */
	struct rw_semaphore controls_rwsem;	/* controls list lock */
	rwlock_t ctl_files_rwlock;	/* ctl_files list lock */
	int controls_count;		/* count of all controls */
	struct list_head controls;	/* all controls for this card */
	struct list_head ctl_files;	/* active control files */

	snd_info_entry_t *proc_root;	/* root for soundcard specific files */
	snd_info_entry_t *proc_id;	/* the card id */
	struct proc_dir_entry *proc_root_link;	/* number link to real id */

	struct snd_monitor_file *files; /* all files associated to this card */
	struct snd_shutdown_f_ops *s_f_ops; /* file operations in the shutdown state */
	spinlock_t files_lock;		/* lock the files for this card */
	int shutdown;			/* this card is going down */
	wait_queue_head_t shutdown_sleep;
	struct work_struct free_workq;	/* for free in workqueue */
	struct device *dev;

#ifdef CONFIG_PM
	int (*pm_suspend)(snd_card_t *card, unsigned int state);
	int (*pm_resume)(snd_card_t *card, unsigned int state);
	struct pm_dev *pm_dev;		/* for ISA */
	void *pm_private_data;
	unsigned int power_state;	/* power state */
	struct semaphore power_lock;	/* power lock */
	wait_queue_head_t power_sleep;
#endif

#if defined(CONFIG_SND_MIXER_OSS) || defined(CONFIG_SND_MIXER_OSS_MODULE)
	snd_mixer_oss_t *mixer_oss;
	int mixer_oss_change_count;
#endif
};

#ifdef CONFIG_PM
static inline void snd_power_lock(snd_card_t *card)
{
	down(&card->power_lock);
}

static inline void snd_power_unlock(snd_card_t *card)
{
	up(&card->power_lock);
}

int snd_power_wait(snd_card_t *card, unsigned int power_state, struct file *file);

static inline unsigned int snd_power_get_state(snd_card_t *card)
{
	return card->power_state;
}

static inline void snd_power_change_state(snd_card_t *card, unsigned int state)
{
	card->power_state = state;
	wake_up(&card->power_sleep);
}
int snd_card_set_pm_callback(snd_card_t *card,
			     int (*suspend)(snd_card_t *, unsigned int),
			     int (*resume)(snd_card_t *, unsigned int),
			     void *private_data);
int snd_card_set_dev_pm_callback(snd_card_t *card, int type,
				 int (*suspend)(snd_card_t *, unsigned int),
				 int (*resume)(snd_card_t *, unsigned int),
				 void *private_data);
#define snd_card_set_isa_pm_callback(card,suspend,resume,data) \
	snd_card_set_dev_pm_callback(card, PM_ISA_DEV, suspend, resume, data)
#ifndef SND_PCI_PM_CALLBACKS
int snd_card_pci_suspend(struct pci_dev *dev, u32 state);
int snd_card_pci_resume(struct pci_dev *dev);
#define SND_PCI_PM_CALLBACKS \
	.suspend = snd_card_pci_suspend,  .resume = snd_card_pci_resume
#endif
#else
#define snd_power_lock(card)		do { (void)(card); } while (0)
#define snd_power_unlock(card)		do { (void)(card); } while (0)
static inline int snd_power_wait(snd_card_t *card, unsigned int state, struct file *file) { return 0; }
#define snd_power_get_state(card)	SNDRV_CTL_POWER_D0
#define snd_power_change_state(card, state)	do { (void)(card); } while (0)
#define snd_card_set_pm_callback(card,suspend,resume,data) -EINVAL
#define snd_card_set_dev_pm_callback(card,suspend,resume,data) -EINVAL
#define snd_card_set_isa_pm_callback(card,suspend,resume,data) -EINVAL
#define SND_PCI_PM_CALLBACKS
#endif

/* device.c */

struct _snd_minor {
	struct list_head list;		/* list of all minors per card */
	int number;			/* minor number */
	int device;			/* device number */
	const char *comment;		/* for /proc/asound/devices */
	struct file_operations *f_ops;	/* file operations */
	char name[0];			/* device name (keep at the end of structure) */
};

typedef struct _snd_minor snd_minor_t;

/* sound.c */

extern int snd_ecards_limit;

void snd_request_card(int card);

int snd_register_device(int type, snd_card_t *card, int dev, snd_minor_t *reg, const char *name);
int snd_unregister_device(int type, snd_card_t *card, int dev);

#ifdef CONFIG_SND_OSSEMUL
int snd_register_oss_device(int type, snd_card_t *card, int dev, snd_minor_t *reg, const char *name);
int snd_unregister_oss_device(int type, snd_card_t *card, int dev);
#endif

int snd_minor_info_init(void);
int snd_minor_info_done(void);

/* sound_oss.c */

#ifdef CONFIG_SND_OSSEMUL
int snd_minor_info_oss_init(void);
int snd_minor_info_oss_done(void);
int snd_oss_init_module(void);
#else
#define snd_minor_info_oss_init() /*NOP*/
#define snd_minor_info_oss_done() /*NOP*/
#define snd_oss_init_module() /*NOP*/
#endif

/* memory.c */

#ifdef CONFIG_SND_DEBUG_MEMORY
void snd_memory_init(void);
void snd_memory_done(void);
int snd_memory_info_init(void);
int snd_memory_info_done(void);
void *snd_hidden_kmalloc(size_t size, int flags);
void snd_hidden_kfree(const void *obj);
void *snd_hidden_vmalloc(unsigned long size);
void snd_hidden_vfree(void *obj);
#define kmalloc(size, flags) snd_hidden_kmalloc(size, flags)
#define kfree(obj) snd_hidden_kfree(obj)
#define vmalloc(size) snd_hidden_vmalloc(size)
#define vfree(obj) snd_hidden_vfree(obj)
#define kmalloc_nocheck(size, flags) snd_wrapper_kmalloc(size, flags)
#define vmalloc_nocheck(size) snd_wrapper_vmalloc(size)
#define kfree_nocheck(obj) snd_wrapper_kfree(obj)
#define vfree_nocheck(obj) snd_wrapper_vfree(obj)
#else
#define snd_memory_init() /*NOP*/
#define snd_memory_done() /*NOP*/
#define snd_memory_info_init() /*NOP*/
#define snd_memory_info_done() /*NOP*/
#define kmalloc_nocheck(size, flags) kmalloc(size, flags)
#define vmalloc_nocheck(size) vmalloc(size)
#define kfree_nocheck(obj) kfree(obj)
#define vfree_nocheck(obj) vfree(obj)
#endif
void *snd_kcalloc(size_t size, int flags);
char *snd_kmalloc_strdup(const char *string, int flags);
int copy_to_user_fromio(void __user *dst, unsigned long src, size_t count);
int copy_from_user_toio(unsigned long dst, const void __user *src, size_t count);

/* init.c */

extern int snd_cards_count;
extern unsigned int snd_cards_lock;
extern snd_card_t *snd_cards[SNDRV_CARDS];
extern rwlock_t snd_card_rwlock;
#if defined(CONFIG_SND_MIXER_OSS) || defined(CONFIG_SND_MIXER_OSS_MODULE)
#define SND_MIXER_OSS_NOTIFY_REGISTER	0
#define SND_MIXER_OSS_NOTIFY_DISCONNECT	1
#define SND_MIXER_OSS_NOTIFY_FREE	2
extern int (*snd_mixer_oss_notify_callback)(snd_card_t *card, int cmd);
#endif

snd_card_t *snd_card_new(int idx, const char *id,
			 struct module *module, int extra_size);
int snd_card_disconnect(snd_card_t *card);
int snd_card_free(snd_card_t *card);
int snd_card_free_in_thread(snd_card_t *card);
int snd_card_register(snd_card_t *card);
int snd_card_info_init(void);
int snd_card_info_done(void);
int snd_component_add(snd_card_t *card, const char *component);
int snd_card_file_add(snd_card_t *card, struct file *file);
int snd_card_file_remove(snd_card_t *card, struct file *file);

#ifndef snd_card_set_dev
#define snd_card_set_dev(card,devptr) ((card)->dev = (devptr))
#endif

/* device.c */

int snd_device_new(snd_card_t *card, snd_device_type_t type,
		   void *device_data, snd_device_ops_t *ops);
int snd_device_register(snd_card_t *card, void *device_data);
int snd_device_register_all(snd_card_t *card);
int snd_device_disconnect(snd_card_t *card, void *device_data);
int snd_device_disconnect_all(snd_card_t *card);
int snd_device_free(snd_card_t *card, void *device_data);
int snd_device_free_all(snd_card_t *card, snd_device_cmd_t cmd);

/* isadma.c */

#define DMA_MODE_NO_ENABLE	0x0100

void snd_dma_program(unsigned long dma, unsigned long addr, unsigned int size, unsigned short mode);
void snd_dma_disable(unsigned long dma);
unsigned int snd_dma_pointer(unsigned long dma, unsigned int size);

/* misc.c */

int snd_task_name(struct task_struct *task, char *name, size_t size);
#ifdef CONFIG_SND_VERBOSE_PRINTK
void snd_verbose_printk(const char *file, int line, const char *format, ...)
     __attribute__ ((format (printf, 3, 4)));
#endif
#if defined(CONFIG_SND_DEBUG) && defined(CONFIG_SND_VERBOSE_PRINTK)
void snd_verbose_printd(const char *file, int line, const char *format, ...)
     __attribute__ ((format (printf, 3, 4)));
#endif

/* --- */

#ifdef CONFIG_SND_VERBOSE_PRINTK
/**
 * snd_printk - printk wrapper
 * @fmt: format string
 *
 * Works like print() but prints the file and the line of the caller
 * when configured with CONFIG_SND_VERBOSE_PRINTK.
 */
#define snd_printk(fmt, args...) \
	snd_verbose_printk(__FILE__, __LINE__, fmt ,##args)
#else
#define snd_printk(fmt, args...) \
	printk(fmt ,##args)
#endif

#ifdef CONFIG_SND_DEBUG

#define __ASTRING__(x) #x

#ifdef CONFIG_SND_VERBOSE_PRINTK
/**
 * snd_printd - debug printk
 * @format: format string
 *
 * Compiled only when Works like snd_printk() for debugging purpose.
 * Ignored when CONFIG_SND_DEBUG is not set.
 */
#define snd_printd(fmt, args...) \
	snd_verbose_printd(__FILE__, __LINE__, fmt ,##args)
#else
#define snd_printd(fmt, args...) \
	printk(fmt ,##args)
#endif
/**
 * snd_assert - run-time assersion macro
 * @expr: expression
 * @args...: the action
 *
 * This macro checks the expression in run-time and invokes the commands
 * given in the rest arguments if the assertion is failed.
 * When CONFIG_SND_DEBUG is not set, the expression is executed but
 * not checked.
 */
#define snd_assert(expr, args...) do {\
	if (!(expr)) {\
		snd_printk("BUG? (%s) (called from %p)\n", __ASTRING__(expr), __builtin_return_address(0));\
		args;\
	}\
} while (0)
/**
 * snd_runtime_check - run-time assersion macro
 * @expr: expression
 * @args...: the action
 *
 * This macro checks the expression in run-time and invokes the commands
 * given in the rest arguments if the assertion is failed.
 * Unlike snd_assert(), the action commands are executed even if
 * CONFIG_SND_DEBUG is not set but without any error messages.
 */
#define snd_runtime_check(expr, args...) do {\
	if (!(expr)) {\
		snd_printk("ERROR (%s) (called from %p)\n", __ASTRING__(expr), __builtin_return_address(0));\
		args;\
	}\
} while (0)

#else /* !CONFIG_SND_DEBUG */

#define snd_printd(fmt, args...)	/* nothing */
#define snd_assert(expr, args...)	(void)(expr)
#define snd_runtime_check(expr, args...) do { if (!(expr)) { args; } } while (0)

#endif /* CONFIG_SND_DEBUG */

#ifdef CONFIG_SND_DEBUG_DETECT
/**
 * snd_printdd - debug printk
 * @format: format string
 *
 * Compiled only when Works like snd_printk() for debugging purpose.
 * Ignored when CONFIG_SND_DEBUG_DETECT is not set.
 */
#define snd_printdd(format, args...) snd_printk(format, ##args)
#else
#define snd_printdd(format, args...) /* nothing */
#endif

#define snd_BUG() snd_assert(0, )


static inline void snd_timestamp_now(struct timespec *tstamp, int timespec)
{
	struct timeval val;
	/* FIXME: use a linear time source */
	do_gettimeofday(&val);
	tstamp->tv_sec = val.tv_sec;
	tstamp->tv_nsec = val.tv_usec;
	if (timespec)
		tstamp->tv_nsec *= 1000L;
}

static inline void snd_timestamp_zero(struct timespec *tstamp)
{
	tstamp->tv_sec = 0;
	tstamp->tv_nsec = 0;
}

static inline int snd_timestamp_null(struct timespec *tstamp)
{
	return tstamp->tv_sec == 0 && tstamp->tv_nsec == 0;
}

#define SNDRV_OSS_VERSION         ((3<<16)|(8<<8)|(1<<4)|(0))	/* 3.8.1a */

#endif /* __SOUND_CORE_H */
