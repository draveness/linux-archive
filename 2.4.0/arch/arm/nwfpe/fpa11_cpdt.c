/*
    NetWinder Floating Point Emulator
    (c) Rebel.com, 1998-1999
    (c) Philip Blundell, 1998

    Direct questions, comments to Scott Bambrough <scottb@netwinder.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

#include "softfloat.h"
#include "fpopcode.h"
#include "fpa11.h"
#include "fpmodule.h"
#include "fpmodule.inl"

#include <asm/uaccess.h>

extern __inline__
void loadSingle(const unsigned int Fn,const unsigned int *pMem)
{
   fpa11->fType[Fn] = typeSingle;
   get_user(fpa11->fpreg[Fn].fSingle, pMem);
}

extern __inline__
void loadDouble(const unsigned int Fn,const unsigned int *pMem)
{
   unsigned int *p;
   p = (unsigned int*)&fpa11->fpreg[Fn].fDouble;
   fpa11->fType[Fn] = typeDouble;
   get_user(p[0], &pMem[1]);
   get_user(p[1], &pMem[0]); /* sign & exponent */
}   

extern __inline__
void loadExtended(const unsigned int Fn,const unsigned int *pMem)
{
   unsigned int *p;
   p = (unsigned int*)&fpa11->fpreg[Fn].fExtended;
   fpa11->fType[Fn] = typeExtended;
   get_user(p[0], &pMem[0]);  /* sign & exponent */
   get_user(p[1], &pMem[2]);  /* ls bits */
   get_user(p[2], &pMem[1]);  /* ms bits */
}   

extern __inline__
void loadMultiple(const unsigned int Fn,const unsigned int *pMem)
{
   register unsigned int *p;
   unsigned long x;

   p = (unsigned int*)&(fpa11->fpreg[Fn]);
   get_user(x, &pMem[0]);
   fpa11->fType[Fn] = (x >> 14) & 0x00000003;
   
   switch (fpa11->fType[Fn])
   {
      case typeSingle:
      case typeDouble:
      {
         get_user(p[0], &pMem[2]);  /* Single */
         get_user(p[1], &pMem[1]);  /* double msw */
         p[2] = 0;        /* empty */
      }
      break; 
   
      case typeExtended:
      {
         get_user(p[1], &pMem[2]);
         get_user(p[2], &pMem[1]);  /* msw */
         p[0] = (x & 0x80003fff);      
      }
      break;
   }
}

extern __inline__
void storeSingle(const unsigned int Fn,unsigned int *pMem)
{
   float32 val;
   register unsigned int *p = (unsigned int*)&val;
   
   switch (fpa11->fType[Fn])
   {
      case typeDouble: 
         val = float64_to_float32(fpa11->fpreg[Fn].fDouble);
      break;

      case typeExtended: 
         val = floatx80_to_float32(fpa11->fpreg[Fn].fExtended);
      break;

      default: val = fpa11->fpreg[Fn].fSingle;
   }
  
   put_user(p[0], pMem);
}   

extern __inline__
void storeDouble(const unsigned int Fn,unsigned int *pMem)
{
   float64 val;
   register unsigned int *p = (unsigned int*)&val;

   switch (fpa11->fType[Fn])
   {
      case typeSingle: 
         val = float32_to_float64(fpa11->fpreg[Fn].fSingle);
      break;

      case typeExtended:
         val = floatx80_to_float64(fpa11->fpreg[Fn].fExtended);
      break;

      default: val = fpa11->fpreg[Fn].fDouble;
   }
   put_user(p[1], &pMem[0]);	/* msw */
   put_user(p[0], &pMem[1]);	/* lsw */
}   

extern __inline__
void storeExtended(const unsigned int Fn,unsigned int *pMem)
{
   floatx80 val;
   register unsigned int *p = (unsigned int*)&val;
   
   switch (fpa11->fType[Fn])
   {
      case typeSingle: 
         val = float32_to_floatx80(fpa11->fpreg[Fn].fSingle);
      break;

      case typeDouble: 
         val = float64_to_floatx80(fpa11->fpreg[Fn].fDouble);
      break;

      default: val = fpa11->fpreg[Fn].fExtended;
   }
   
   put_user(p[0], &pMem[0]); /* sign & exp */
   put_user(p[1], &pMem[2]);
   put_user(p[2], &pMem[1]); /* msw */
}   

extern __inline__
void storeMultiple(const unsigned int Fn,unsigned int *pMem)
{
   register unsigned int nType, *p;
   
   p = (unsigned int*)&(fpa11->fpreg[Fn]);
   nType = fpa11->fType[Fn];
   
   switch (nType)
   {
      case typeSingle:
      case typeDouble:
      {
	 put_user(p[0], &pMem[2]); /* single */
	 put_user(p[1], &pMem[1]); /* double msw */
	 put_user(nType << 14, &pMem[0]);
      }
      break; 
   
      case typeExtended:
      {
	 put_user(p[2], &pMem[1]); /* msw */
	 put_user(p[1], &pMem[2]);
	 put_user((p[0] & 0x80003fff) | (nType << 14), &pMem[0]);
      }
      break;
   }
}

unsigned int PerformLDF(const unsigned int opcode)
{
   unsigned int *pBase, *pAddress, *pFinal, nRc = 1,
     write_back = WRITE_BACK(opcode);

   //printk("PerformLDF(0x%08x), Fd = 0x%08x\n",opcode,getFd(opcode));

   pBase = (unsigned int*)readRegister(getRn(opcode));
   if (REG_PC == getRn(opcode))
   {
     pBase += 2;
     write_back = 0;
   }

   pFinal = pBase;
   if (BIT_UP_SET(opcode))
     pFinal += getOffset(opcode);
   else
     pFinal -= getOffset(opcode);

   if (PREINDEXED(opcode)) pAddress = pFinal; else pAddress = pBase;

   switch (opcode & MASK_TRANSFER_LENGTH)
   {
      case TRANSFER_SINGLE  : loadSingle(getFd(opcode),pAddress);   break;
      case TRANSFER_DOUBLE  : loadDouble(getFd(opcode),pAddress);   break;
      case TRANSFER_EXTENDED: loadExtended(getFd(opcode),pAddress); break;
      default: nRc = 0;
   }
   
   if (write_back) writeRegister(getRn(opcode),(unsigned int)pFinal);
   return nRc;
}

unsigned int PerformSTF(const unsigned int opcode)
{
   unsigned int *pBase, *pAddress, *pFinal, nRc = 1,
     write_back = WRITE_BACK(opcode);
   
   //printk("PerformSTF(0x%08x), Fd = 0x%08x\n",opcode,getFd(opcode));
   SetRoundingMode(ROUND_TO_NEAREST);
   
   pBase = (unsigned int*)readRegister(getRn(opcode));
   if (REG_PC == getRn(opcode))
   {
     pBase += 2;
     write_back = 0;
   }

   pFinal = pBase;
   if (BIT_UP_SET(opcode))
     pFinal += getOffset(opcode);
   else
     pFinal -= getOffset(opcode);

   if (PREINDEXED(opcode)) pAddress = pFinal; else pAddress = pBase;

   switch (opcode & MASK_TRANSFER_LENGTH)
   {
      case TRANSFER_SINGLE  : storeSingle(getFd(opcode),pAddress);   break;
      case TRANSFER_DOUBLE  : storeDouble(getFd(opcode),pAddress);   break;
      case TRANSFER_EXTENDED: storeExtended(getFd(opcode),pAddress); break;
      default: nRc = 0;
   }
   
   if (write_back) writeRegister(getRn(opcode),(unsigned int)pFinal);
   return nRc;
}

unsigned int PerformLFM(const unsigned int opcode)
{
   unsigned int i, Fd, *pBase, *pAddress, *pFinal,
     write_back = WRITE_BACK(opcode);

   pBase = (unsigned int*)readRegister(getRn(opcode));
   if (REG_PC == getRn(opcode))
   {
     pBase += 2;
     write_back = 0;
   }

   pFinal = pBase;
   if (BIT_UP_SET(opcode))
     pFinal += getOffset(opcode);
   else
     pFinal -= getOffset(opcode);

   if (PREINDEXED(opcode)) pAddress = pFinal; else pAddress = pBase;

   Fd = getFd(opcode);
   for (i=getRegisterCount(opcode);i>0;i--)
   {
     loadMultiple(Fd,pAddress);
     pAddress += 3; Fd++;
     if (Fd == 8) Fd = 0;
   }

   if (write_back) writeRegister(getRn(opcode),(unsigned int)pFinal);
   return 1;
}

unsigned int PerformSFM(const unsigned int opcode)
{
   unsigned int i, Fd, *pBase, *pAddress, *pFinal,
     write_back = WRITE_BACK(opcode);
   
   pBase = (unsigned int*)readRegister(getRn(opcode));
   if (REG_PC == getRn(opcode))
   {
     pBase += 2;
     write_back = 0;
   }
   
   pFinal = pBase;
   if (BIT_UP_SET(opcode))
     pFinal += getOffset(opcode);
   else
     pFinal -= getOffset(opcode);

   if (PREINDEXED(opcode)) pAddress = pFinal; else pAddress = pBase;

   Fd = getFd(opcode);
   for (i=getRegisterCount(opcode);i>0;i--)
   {
     storeMultiple(Fd,pAddress);
     pAddress += 3; Fd++;
     if (Fd == 8) Fd = 0;
   }

   if (write_back) writeRegister(getRn(opcode),(unsigned int)pFinal);
   return 1;
}

#if 1
unsigned int EmulateCPDT(const unsigned int opcode)
{
  unsigned int nRc = 0;

  //printk("EmulateCPDT(0x%08x)\n",opcode);
  
  if (LDF_OP(opcode))
  {
    nRc = PerformLDF(opcode);
  }
  else if (LFM_OP(opcode))
  {
    nRc = PerformLFM(opcode);
  }
  else if (STF_OP(opcode))
  {
    nRc = PerformSTF(opcode);
  } 
  else if (SFM_OP(opcode))
  {
    nRc = PerformSFM(opcode);
  }
  else
  {
    nRc = 0;
  }
  
  return nRc;
}
#endif
