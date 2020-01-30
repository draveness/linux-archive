
/*
 *
 * Copyright (C) Eicon Technology Corporation, 2000.
 *
 * This source file is supplied for the exclusive use with Eicon
 * Technology Corporation's range of DIVA Server Adapters.
 *
 * Eicon File Revision :    1.6  
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY OF ANY KIND WHATSOEVER INCLUDING ANY 
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */


/*
 * Interface to Unix specific code for performing card I/O
 */

#if !defined(UXIO_H)
#define UXIO_H

#include "sys.h"
#include "adapter.h"


struct pt_regs;

/* user callback, returns zero if interrupt was from this card */
typedef	void		isr_fn_t(void *);
struct ux_diva_card_s
{
	word	in_use;
	int		io_base;
	int		reset_base;
	int		card_type;
	byte		*mapped;
	int		bus_num;
	int		func_num;
	int		slot;
	int		irq;
	byte		*pDRAM;
	byte		*pDEVICES;
	byte		*pCONFIG;
	byte		*pSHARED;
	byte		*pCONTROL;
	word		features;
	void		*user_isr_arg;
	isr_fn_t	*user_isr;
};

void bcopy(void *pSource, void *pDest, dword dwLength);
void bzero(void *pDataArea, dword dwLength);


/*
 * Get a card handle to enable card to be accessed
 */

int		UxCardHandleGet(	ux_diva_card_t	**card,
							dia_card_t		*cfg);

/*
 * Free a card handle as no longer needed
 */

void	UxCardHandleFree(ux_diva_card_t *card);

/*
 * Lock and unlock access to a card
 */

int		UxCardLock(ux_diva_card_t *card);
void	UxCardUnlock(ux_diva_card_t *card, int ipl);

/*
 * Set the mapping address for PCI cards
 */

int		UxCardAddrMappingSet(ux_diva_card_t	*card,
							int				id,
							void			*address,
							int				size);

/*
 * Attach card to memory to enable it to be accessed
 * Returns the mapped address
 */

void	*UxCardMemAttach(ux_diva_card_t *card, int id);

/*
 * map card out of memory after completion of access
 */

void	UxCardMemDetach(ux_diva_card_t *card, void *address);

/*
 * input functions for memory-mapped cards
 */

byte	UxCardMemIn(ux_diva_card_t *card, void *address);

word	UxCardMemInW(ux_diva_card_t *card, void *address);

dword	UxCardMemInD(ux_diva_card_t *card, void *address);

void	UxCardMemInBuffer(	ux_diva_card_t *card,
							void			*address,
							void			*buffer,
							int				length);

/*
 * output functions for memory-mapped cards
 */

void UxCardMemOut(ux_diva_card_t *card, void *address, byte data);

void UxCardMemOutW(ux_diva_card_t *card, void *address, word data);

void UxCardMemOutD(ux_diva_card_t *card, void *address, dword data);

void UxCardMemOutBuffer(	ux_diva_card_t	*card,
							void			*address,
							void			*buffer,
							int				length);

/*
 * input functions for I/O-mapped cards
 */

byte	UxCardIoIn(ux_diva_card_t *card, void *, void *address);

word	UxCardIoInW(ux_diva_card_t *card, void *, void *address);

dword	UxCardIoInD(ux_diva_card_t *card, void *, void *address);

void	UxCardIoInBuffer(	ux_diva_card_t *card,
							void *, void			*address,
							void			*buffer,
							int				length);

/*
 * output functions for I/O-mapped cards
 */

void UxCardIoOut(ux_diva_card_t *card, void *, void *address, byte data);

void UxCardIoOutW(ux_diva_card_t *card, void *, void *address, word data);

void UxCardIoOutD(ux_diva_card_t *card, void *, void *address, dword data);

void UxCardIoOutBuffer(	ux_diva_card_t	*card,
							void *, void			*address,
							void			*buffer,
							int				length);

/*
 * Get specified PCI config
 */

void	UxPciConfigRead(ux_diva_card_t	*card, 
						int				size,
						int				offset,
						void			*value);

/*
 * Set specified PCI config
 */

void	UxPciConfigWrite(ux_diva_card_t	*card, 
						int				size,
						int				offset,
						void			*value);

/* allocate memory, returning NULL if none available */

void	*UxAlloc(unsigned int size);

void	UxFree(void *);

/*
 * Pause for specified number of milli-seconds 
 */

void	UxPause(long ms);

/*
 * Install an ISR for the specified card
 */

int		UxIsrInstall(ux_diva_card_t *card, isr_fn_t *isr_fn, void *isr_arg);

/*
 * Remove an ISR for the specified card
 */
void	UxIsrRemove(ux_diva_card_t *card, void *);

/*
 * DEBUG function to turn logging ON or OFF
 */

void	UxCardLog(int turn_on);

long	UxInterlockedIncrement(ux_diva_card_t *card, long *dst);
long	UxInterlockedDecrement(ux_diva_card_t *card, long *dst);

#endif /* of UXIO_H */
