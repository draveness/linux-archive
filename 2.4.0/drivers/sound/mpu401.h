/*
 *	uart401.h 
 *
 * Copyright:	Christoph Hellwig <chhellwig@gmx.net>
 *
 */

/*	From uart401.c */
int probe_uart401 (struct address_info *hw_config, struct module *owner);
void unload_uart401 (struct address_info *hw_config);

void uart401intr (int irq, void *dev_id, struct pt_regs * dummy);

/*	From mpu401.c */
int probe_mpu401(struct address_info *hw_config);
void attach_mpu401(struct address_info * hw_config, struct module *owner);
void unload_mpu401(struct address_info *hw_info);

int intchk_mpu401(void *dev_id);
void mpuintr(int irq, void *dev_id, struct pt_regs * dummy);
