/*
 *  Copyright (C) 2002-2015  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */


#include <string.h>
#include "dosbox.h"
#include "mem.h"
#include "inout.h"
#include "dma.h"
#include "pic.h"
#include "paging.h"
#include "setup.h"
#include "control.h"

#ifdef _MSC_VER
# define MIN(a,b) ((a) < (b) ? (a) : (b))
# define MAX(a,b) ((a) > (b) ? (a) : (b))
#else
# define MIN(a,b) std::min(a,b)
# define MAX(a,b) std::max(a,b)
#endif

bool has_pcibus_enable(void);

DmaController *DmaControllers[2]={NULL};
unsigned char dma_extra_page_registers[16]={0}; /* 0x80-0x8F */
bool enable_dma_extra_page_registers = true;
bool dma_page_register_writeonly = false;

#define EMM_PAGEFRAME4K	((0xE000*16)/4096)
Bit32u ems_board_mapping[LINK_START];

static Bit32u dma_wrapping = 0xffff;

bool enable_1st_dma = true;
bool enable_2nd_dma = true;
bool allow_decrement_mode = true;
int isadma128k = -1;

static void UpdateEMSMapping(void) {
	/* if EMS is not present, this will result in a 1:1 mapping */
	Bitu i;
	for (i=0;i<0x10;i++) {
		ems_board_mapping[EMM_PAGEFRAME4K+i]=paging.firstmb[EMM_PAGEFRAME4K+i];
	}
}

enum {
    DMA_INCREMENT,
    DMA_DECREMENT
};

/* DMA block anti-copypasta common code */
template <const unsigned int dma_mode> static inline void DMA_BlockReadCommonSetup(
    /*output*/PhysPt &o_xfer,unsigned int &o_size,
     /*input*/PhysPt const spage,PhysPt offset,Bitu size,const Bit8u dma16,const Bit32u DMA16_ADDRMASK) {
    assert(size != 0u);

	const Bitu highpart_addr_page = spage>>12;
	size <<= dma16;
	offset <<= dma16;
	const Bit32u dma_wrap = (((0xfffful << dma16) + dma16)&DMA16_ADDRMASK) | dma_wrapping;
    offset &= dma_wrap;
    Bitu page = highpart_addr_page+(offset >> 12); /* page */
    offset &= 0xFFFu; /* 4KB offset in page */
    /* care for EMS pageframe etc. */
    if (page < EMM_PAGEFRAME4K) page = paging.firstmb[page];
    else if (page < EMM_PAGEFRAME4K+0x10) page = ems_board_mapping[page];
    else if (page < LINK_START) page = paging.firstmb[page];
    /* check our work, should not cross 4KB, then transfer linearly. Do not remove {} curly braces, assert() may compile to nothing. */
    if (dma_mode == DMA_INCREMENT)
        { assert((offset + size - (1u << dma16)) < 4096); } /* should stop with offset at or before first byte of next page */
    else if (dma_mode == DMA_DECREMENT)
        { assert(offset >= (size - (1u << dma16))); } /* should stop with offset at or after the last byte of the previous page */
    /* result */
    o_xfer = (page * 4096u) + offset;
    o_size = (unsigned int)size;
}

/* read a block from physical memory.
 * This code assumes new DMA read code that transfers 4KB at a time (4KB byte or 2KB word) therefore it is safe
 * to compute the page and offset once and transfer in a tight loop. */
template <const unsigned int dma_mode> static void DMA_BlockRead4KB(const PhysPt spage,const PhysPt offset,void * data,const Bitu size,const Bit8u dma16,const Bit32u DMA16_ADDRMASK) {
	Bit8u *write = (Bit8u*)data;
    unsigned int o_size;
    PhysPt xfer;

    DMA_BlockReadCommonSetup<dma_mode>(/*&*/xfer,/*&*/o_size,spage,offset,size,dma16,DMA16_ADDRMASK);
    if (!dma16) { // 8-bit
        for ( ; o_size ; o_size--, (dma_mode == DMA_DECREMENT ? (xfer--) : (xfer++)) ) *write++ = phys_readb(xfer);
    }
    else { // 16-bit
        assert((o_size & 1u) == 0);
        assert((xfer   & 1u) == 0);
        for ( ; o_size ; o_size -= 2, (dma_mode == DMA_DECREMENT ? (xfer -= 2) : (xfer += 2)), write += 2 ) host_writew(write,phys_readw(xfer));
    }
}

/* write a block into physical memory.
 * assume caller has already clipped the transfer to stay within a 4KB page. */
template <const unsigned int dma_mode> static void DMA_BlockWrite4KB(PhysPt spage,PhysPt offset,const void * data,Bitu size,Bit8u dma16,const Bit32u DMA16_ADDRMASK) {
    const Bit8u *read = (const Bit8u*)data;
    unsigned int o_size;
    PhysPt xfer;

    DMA_BlockReadCommonSetup<dma_mode>(/*&*/xfer,/*&*/o_size,spage,offset,size,dma16,DMA16_ADDRMASK);
    if (!dma16) { // 8-bit
        for ( ; o_size ; o_size--, (dma_mode == DMA_DECREMENT ? (xfer--) : (xfer++)) ) phys_writeb(xfer,*read++);
    }
    else { // 16-bit
        assert((o_size & 1u) == 0);
        assert((xfer   & 1u) == 0);
        for ( ; o_size ; o_size -= 2, (dma_mode == DMA_DECREMENT ? (xfer -= 2) : (xfer += 2)), read += 2 ) phys_writew(xfer,host_readw(read));
    }
}

DmaChannel * GetDMAChannel(Bit8u chan) {
	if (chan<4) {
		/* channel on first DMA controller */
		if (DmaControllers[0]) return DmaControllers[0]->GetChannel(chan);
	} else if (chan<8) {
		/* channel on second DMA controller */
		if (DmaControllers[1]) return DmaControllers[1]->GetChannel(chan-4);
	}
	return NULL;
}

/* remove the second DMA controller (ports are removed automatically) */
void CloseSecondDMAController(void) {
	if (DmaControllers[1]) {
		delete DmaControllers[1];
		DmaControllers[1]=NULL;
	}
}

/* check availability of second DMA controller, needed for SB16 */
bool SecondDMAControllerAvailable(void) {
	if (DmaControllers[1]) return true;
	else return false;
}

static void DMA_Write_Port(Bitu port,Bitu val,Bitu /*iolen*/) {
    if (IS_PC98_ARCH) {
        // I/O port translation
        if (port < 0x20u)
            port >>= 1u;
        else if (port < 0x28) {/* "bank" registers at 21h, 23h, 25h, 27h */
            switch ((port>>1u)&3u) {
                case 0:/* 21h DMA channel 1 */  port=0x83; break;
                case 1:/* 23h DMA channel 2 */  port=0x81; break;
                case 2:/* 25h DMA channel 3 */  port=0x82; break;
                case 3:/* 27h DMA channel 0 */  port=0x87; break;
                default: abort(); break;
            }
        }
        else {
            abort();
        }
    }

	if (port<0x10) {
		/* write to the first DMA controller (channels 0-3) */
		DmaControllers[0]->WriteControllerReg(port,val,1);
	} else if (port>=0xc0 && port <=0xdf) {
		/* write to the second DMA controller (channels 4-7) */
		DmaControllers[1]->WriteControllerReg((port-0xc0) >> 1,val,1);
	} else {
		UpdateEMSMapping();
		dma_extra_page_registers[port&0xF] = val;
		switch (port) {
			/* write DMA page register */
			case 0x81:GetDMAChannel(2)->SetPage((Bit8u)val);break;
			case 0x82:GetDMAChannel(3)->SetPage((Bit8u)val);break;
			case 0x83:GetDMAChannel(1)->SetPage((Bit8u)val);break;
			case 0x87:GetDMAChannel(0)->SetPage((Bit8u)val);break;
			case 0x89:GetDMAChannel(6)->SetPage((Bit8u)val);break;
			case 0x8a:GetDMAChannel(7)->SetPage((Bit8u)val);break;
			case 0x8b:GetDMAChannel(5)->SetPage((Bit8u)val);break;
			case 0x8f:GetDMAChannel(4)->SetPage((Bit8u)val);break;
			default:
				  if (!enable_dma_extra_page_registers)
					  LOG(LOG_DMACONTROL,LOG_NORMAL)("Trying to write undefined DMA page register %x",(int)port);
				  break;
		}
	}
}

static Bitu DMA_Read_Port(Bitu port,Bitu iolen) {
    if (IS_PC98_ARCH) {
        // I/O port translation
        if (port < 0x20u)
            port >>= 1u;
        else if (port < 0x28) {/* "bank" registers at 21h, 23h, 25h, 27h */
            switch ((port>>1u)&3u) {
                case 0:/* 21h DMA channel 1 */  port=0x83; break;
                case 1:/* 23h DMA channel 2 */  port=0x81; break;
                case 2:/* 25h DMA channel 3 */  port=0x82; break;
                case 3:/* 27h DMA channel 0 */  port=0x87; break;
                default: abort(); break;
            }
        }
        else {
            abort();
        }
    }

	if (port<0x10) {
		/* read from the first DMA controller (channels 0-3) */
		return DmaControllers[0]->ReadControllerReg(port,iolen);
	} else if (port>=0xc0 && port <=0xdf) {
		/* read from the second DMA controller (channels 4-7) */
		return DmaControllers[1]->ReadControllerReg((port-0xc0) >> 1,iolen);
	} else {
		/* if we're emulating PC/XT DMA controller behavior, then the page registers
		 * are write-only and cannot be read */
		if (dma_page_register_writeonly)
			return ~0UL;

		switch (port) {
			/* read DMA page register */
			case 0x81:return GetDMAChannel(2)->pagenum;
			case 0x82:return GetDMAChannel(3)->pagenum;
			case 0x83:return GetDMAChannel(1)->pagenum;
			case 0x87:return GetDMAChannel(0)->pagenum;
			case 0x89:return GetDMAChannel(6)->pagenum;
			case 0x8a:return GetDMAChannel(7)->pagenum;
			case 0x8b:return GetDMAChannel(5)->pagenum;
			case 0x8f:return GetDMAChannel(4)->pagenum;
			default:
				  if (enable_dma_extra_page_registers)
					return dma_extra_page_registers[port&0xF];
 
				  LOG(LOG_DMACONTROL,LOG_NORMAL)("Trying to read undefined DMA page register %x",(int)port);
				  break;
		}
	}

	return ~0UL;
}

void DmaController::WriteControllerReg(Bitu reg,Bitu val,Bitu /*len*/) {
	DmaChannel * chan;
	switch (reg) {
	/* set base address of DMA transfer (1st byte low part, 2nd byte high part) */
	case 0x0:case 0x2:case 0x4:case 0x6:
		UpdateEMSMapping();
		chan=GetChannel((Bit8u)(reg >> 1));
		flipflop=!flipflop;
		if (flipflop) {
			chan->baseaddr=(chan->baseaddr&0xff00)|val;
			chan->curraddr=(chan->curraddr&0xff00)|val;
		} else {
			chan->baseaddr=(chan->baseaddr&0x00ff)|(val << 8);
			chan->curraddr=(chan->curraddr&0x00ff)|(val << 8);
		}
		break;
	/* set DMA transfer count (1st byte low part, 2nd byte high part) */
	case 0x1:case 0x3:case 0x5:case 0x7:
		UpdateEMSMapping();
		chan=GetChannel((Bit8u)(reg >> 1));
		flipflop=!flipflop;
		if (flipflop) {
			chan->basecnt=(chan->basecnt&0xff00)|val;
			chan->currcnt=(chan->currcnt&0xff00)|val;
		} else {
			chan->basecnt=(chan->basecnt&0x00ff)|(val << 8);
			chan->currcnt=(chan->currcnt&0x00ff)|(val << 8);
		}
		break;
	case 0x8:		/* Comand reg not used */
		break;
	case 0x9:		/* Request registers, memory to memory */
		//TODO Warning?
		break;
	case 0xa:		/* Mask Register */
		if ((val & 0x4)==0) UpdateEMSMapping();
		chan=GetChannel(val & 3);
		chan->SetMask((val & 0x4)>0);
		break;
	case 0xb:		/* Mode Register */
		UpdateEMSMapping();
		chan=GetChannel(val & 3);
		chan->autoinit=(val & 0x10) > 0;
		chan->increment=(!allow_decrement_mode) || ((val & 0x20) == 0); /* 0=increment 1=decrement */
		//TODO Maybe other bits? Like bits 6-7 to select demand/single/block/cascade mode? */
		break;
	case 0xc:		/* Clear Flip/Flip */
		flipflop=false;
		break;
	case 0xd:		/* Master Clear/Reset */
		for (Bit8u ct=0;ct<4;ct++) {
			chan=GetChannel(ct);
			chan->SetMask(true);
			chan->tcount=false;
		}
		flipflop=false;
		break;
	case 0xe:		/* Clear Mask register */		
		UpdateEMSMapping();
		for (Bit8u ct=0;ct<4;ct++) {
			chan=GetChannel(ct);
			chan->SetMask(false);
		}
		break;
	case 0xf:		/* Multiple Mask register */
		UpdateEMSMapping();
		for (Bit8u ct=0;ct<4;ct++) {
			chan=GetChannel(ct);
			chan->SetMask(val & 1);
			val>>=1;
		}
		break;
	}
}

Bitu DmaController::ReadControllerReg(Bitu reg,Bitu /*len*/) {
	DmaChannel * chan;Bitu ret;
	switch (reg) {
	/* read base address of DMA transfer (1st byte low part, 2nd byte high part) */
	case 0x0:case 0x2:case 0x4:case 0x6:
		chan=GetChannel((Bit8u)(reg >> 1));
		flipflop=!flipflop;
		if (flipflop) {
			return chan->curraddr & 0xff;
		} else {
			return (chan->curraddr >> 8) & 0xff;
		}
	/* read DMA transfer count (1st byte low part, 2nd byte high part) */
	case 0x1:case 0x3:case 0x5:case 0x7:
		chan=GetChannel((Bit8u)(reg >> 1));
		flipflop=!flipflop;
		if (flipflop) {
			return chan->currcnt & 0xff;
		} else {
			return (chan->currcnt >> 8) & 0xff;
		}
	case 0x8:		/* Status Register */
		ret=0;
		for (Bit8u ct=0;ct<4;ct++) {
			chan=GetChannel(ct);
			if (chan->tcount) ret |= 1U << ct;
			chan->tcount=false;
			if (chan->request) ret |= 1U << (4U + ct);
		}
		return ret;
	case 0xc:		/* Clear Flip/Flip (apparently most motherboards will treat read OR write as reset) */
		flipflop=false;
		break;
	default:
		LOG(LOG_DMACONTROL,LOG_NORMAL)("Trying to read undefined DMA port %x",(int)reg);
		break;
	}
	return 0xffffffff;
}

DmaChannel::DmaChannel(Bit8u num, bool dma16) {
	masked = true;
	callback = NULL;
	channum = num;
	DMA16 = dma16 ? 0x1 : 0x0;

    if (isadma128k >= 0)
        Set128KMode(isadma128k > 0); // user's choice
    else
        Set128KMode(true); // most hardware seems to implement the 128K case

    LOG(LOG_DMACONTROL,LOG_DEBUG)("DMA channel %u. DMA16_PAGESHIFT=%u DMA16_ADDRMASK=0x%lx",
        (unsigned int)channum,(unsigned int)DMA16_PAGESHIFT,(unsigned long)DMA16_ADDRMASK);
    pagenum = 0;
	pagebase = 0;
	baseaddr = 0;
	curraddr = 0;
	basecnt = 0;
	currcnt = 0;
	increment = true;
	autoinit = false;
	tcount = false;
	request = false;
}

Bitu DmaChannel::Read(Bitu want, Bit8u * buffer) {
	Bitu done=0;
	curraddr &= dma_wrapping;

	/* ISA devices cannot cycle DMA if the controller has masked the channel! Fix your code! */
	if (masked) {
		LOG(LOG_DMACONTROL,LOG_WARN)("BUG: Attempted DMA channel read while channel masked");
		return 0;
	}

    /* TODO: Reject Read() if DMA mode byte was programmed in any mode other than read */

    /* WARNING: "want" is expressed in DMA transfer units.
     *          For 8-bit DMA, want is in bytes.
     *          For 16-bit DMA, want is in 16-bit WORDs.
     *          Keep that in mind when writing code to call this function!
     *          Perhaps a future modification could provide an alternate
     *          Read function that expresses the count in bytes so the caller
     *          cannot accidentally cause buffer overrun issues that cause
     *          mystery crashes. */

    const Bit32u addrmask = 0xFFFu >> DMA16; /* 16-bit ISA style DMA needs 0x7FFF, else 0xFFFF. Use 0x7FF/0xFFF (4KB) for simplicity reasons. */
    while (want > 0) {
        const Bit32u addr =
            curraddr & addrmask;
        const Bitu wrapdo =
            increment ?
                /*inc*/((addrmask + 1u) - addr) :   /* how many transfer units until (end of 4KB page) + 1 */
                /*dec*/(addr + 1u);                 /* how many transfer units until (start of 4KB page) - 1 */
        const Bitu cando =
            MIN(MIN(want,Bitu(currcnt+1u)),wrapdo);
        assert(wrapdo != (Bitu)0);
        assert(cando != (Bitu)0);
        assert(cando <= want);
        assert(cando <= (addrmask + 1u));

        if (increment) {
            assert((curraddr & (~addrmask)) == ((curraddr + (cando - 1u)) & (~addrmask)));//check our work, must not cross a 4KB boundary
            DMA_BlockRead4KB<DMA_INCREMENT>(pagebase,curraddr,buffer,cando,DMA16,DMA16_ADDRMASK);
            curraddr += cando;
        }
        else {
            assert((curraddr & (~addrmask)) == ((curraddr - (cando - 1u)) & (~addrmask)));//check our work, must not cross a 4KB boundary
            DMA_BlockRead4KB<DMA_DECREMENT>(pagebase,curraddr,buffer,cando,DMA16,DMA16_ADDRMASK);
            curraddr -= cando;
        }

        curraddr &= dma_wrapping;
        buffer += cando << DMA16;
        currcnt -= cando;
        want -= cando;
        done += cando;

        if (currcnt == 0xFFFF) {
            ReachedTC();
            if (autoinit) {
                currcnt = basecnt;
                curraddr = baseaddr;
                UpdateEMSMapping();
            } else {
                masked = true;
                UpdateEMSMapping();
                DoCallBack(DMA_TRANSFEREND);
                break;
            }
        }
    }

	return done;
}

Bitu DmaChannel::Write(Bitu want, Bit8u * buffer) {
	Bitu done=0;
	curraddr &= dma_wrapping;

	/* ISA devices cannot cycle DMA if the controller has masked the channel! Fix your code! */
	if (masked) {
		LOG(LOG_DMACONTROL,LOG_WARN)("BUG: Attempted DMA channel write while channel masked");
		return 0;
	}

    /* TODO: Reject Write() if DMA mode byte was programmed in any mode other than write */

    /* WARNING: "want" is expressed in DMA transfer units.
     *          For 8-bit DMA, want is in bytes.
     *          For 16-bit DMA, want is in 16-bit WORDs.
     *          Keep that in mind when writing code to call this function!
     *          Perhaps a future modification could provide an alternate
     *          Read function that expresses the count in bytes so the caller
     *          cannot accidentally cause buffer overrun issues that cause
     *          mystery crashes. */

    const Bit32u addrmask = 0xFFFu >> DMA16; /* 16-bit ISA style DMA needs 0x7FFF, else 0xFFFF. Use 0x7FF/0xFFF (4KB) for simplicity reasons. */
    while (want > 0) {
        const Bit32u addr =
            curraddr & addrmask;
        const Bitu wrapdo =
            increment ?
                /*inc*/((addrmask + 1u) - addr) :   /* how many transfer units until (end of 4KB page) + 1 */
                /*dec*/(addr + 1u);                 /* how many transfer units until (start of 4KB page) - 1 */
        const Bitu cando =
            MIN(MIN(want,Bitu(currcnt+1u)),wrapdo);
        assert(wrapdo != (Bitu)0);
        assert(cando != (Bitu)0);
        assert(cando <= want);
        assert(cando <= (addrmask + 1u));

        if (increment) {
            assert((curraddr & (~addrmask)) == ((curraddr + (cando - 1u)) & (~addrmask)));//check our work, must not cross a 4KB boundary
            DMA_BlockWrite4KB<DMA_INCREMENT>(pagebase,curraddr,buffer,cando,DMA16,DMA16_ADDRMASK);
            curraddr += cando;
        }
        else {
            assert((curraddr & (~addrmask)) == ((curraddr - (cando - 1u)) & (~addrmask)));//check our work, must not cross a 4KB boundary
            DMA_BlockWrite4KB<DMA_DECREMENT>(pagebase,curraddr,buffer,cando,DMA16,DMA16_ADDRMASK);
            curraddr -= cando;
        }

        curraddr &= dma_wrapping;
        buffer += cando << DMA16;
        currcnt -= cando;
        want -= cando;
        done += cando;

        if (currcnt == 0xFFFF) {
            ReachedTC();
            if (autoinit) {
                currcnt = basecnt;
                curraddr = baseaddr;
                UpdateEMSMapping();
            } else {
                masked = true;
                UpdateEMSMapping();
                DoCallBack(DMA_TRANSFEREND);
                break;
            }
        }
    }

	return done;
}

void DMA_SetWrapping(Bitu wrap) {
	dma_wrapping = wrap;
}

void DMA_FreeControllers() {
	if (DmaControllers[0]) {
		delete DmaControllers[0];
		DmaControllers[0]=NULL;
	}
	if (DmaControllers[1]) {
		delete DmaControllers[1];
		DmaControllers[1]=NULL;
	}
}

void DMA_Destroy(Section* /*sec*/) {
	DMA_FreeControllers();
}

void DMA_Reset(Section* /*sec*/) {
	Bitu i;

	DMA_FreeControllers();

	// LOG
	LOG(LOG_MISC,LOG_DEBUG)("DMA_Reset(): reinitializing DMA controller(s)");

	Section_prop * section=static_cast<Section_prop *>(control->GetSection("dosbox"));
	assert(section != NULL);

	DMA_SetWrapping(0xffff);

	/* NTS: parsing on reset means a reboot of the VM (and possibly other conditions) can permit
	 *      the user to change DMA emulation settings and have them take effect on VM reboot. */
	enable_2nd_dma = section->Get_bool("enable 2nd dma controller");
	enable_1st_dma = enable_2nd_dma || section->Get_bool("enable 1st dma controller");
	enable_dma_extra_page_registers = section->Get_bool("enable dma extra page registers");
	dma_page_register_writeonly = section->Get_bool("dma page registers write-only");
	allow_decrement_mode = section->Get_bool("allow dma address decrement");

    if (IS_PC98_ARCH) // DMA 4-7 do not exist on PC-98
        enable_2nd_dma = false;

    if (machine == MCH_PCJR) {
        LOG(LOG_MISC,LOG_DEBUG)("DMA is disabled in PCjr mode");
        enable_1st_dma = false;
        enable_2nd_dma = false;
        return;
    }

    {
        std::string s = section->Get_string("enable 128k capable 16-bit dma");

        if (s == "true" || s == "1")
            isadma128k = 1;
        else if (s == "false" || s == "0")
            isadma128k = 0;
        else
            isadma128k = -1;
    }

	if (enable_1st_dma)
		DmaControllers[0] = new DmaController(0);
	else
		DmaControllers[0] = NULL;

	if (enable_2nd_dma)
		DmaControllers[1] = new DmaController(1);
	else
		DmaControllers[1] = NULL;

	for (i=0;i<0x10;i++) {
		Bitu mask = IO_MB;
		if (i < 8) mask |= IO_MW;

		if (enable_1st_dma) {
			/* install handler for first DMA controller ports */
			DmaControllers[0]->DMA_WriteHandler[i].Install(IS_PC98_ARCH ? ((i * 2u) + 1u) : i,DMA_Write_Port,mask);
			DmaControllers[0]->DMA_ReadHandler[i].Install(IS_PC98_ARCH ? ((i * 2u) + 1u) : i,DMA_Read_Port,mask);
		}
		if (enable_2nd_dma) {
            assert(!IS_PC98_ARCH);
			/* install handler for second DMA controller ports */
			DmaControllers[1]->DMA_WriteHandler[i].Install(0xc0+i*2,DMA_Write_Port,mask);
			DmaControllers[1]->DMA_ReadHandler[i].Install(0xc0+i*2,DMA_Read_Port,mask);
		}
	}

	if (enable_1st_dma) {
        if (IS_PC98_ARCH) {
            /* install handlers for ports 0x21-0x27 odd */
            for (unsigned int i=0;i < 4;i++) {
                DmaControllers[0]->DMA_WriteHandler[0x10+i].Install(0x21+(i*2u),DMA_Write_Port,IO_MB,1);
                DmaControllers[0]->DMA_ReadHandler[0x10+i].Install(0x21+(i*2u),DMA_Read_Port,IO_MB,1);
            }
        }
        else {
            /* install handlers for ports 0x81-0x83 (on the first DMA controller) */
            DmaControllers[0]->DMA_WriteHandler[0x10].Install(0x80,DMA_Write_Port,IO_MB,8);
            DmaControllers[0]->DMA_ReadHandler[0x10].Install(0x80,DMA_Read_Port,IO_MB,8);
        }
	}

	if (enable_2nd_dma) {
        assert(!IS_PC98_ARCH);
        /* install handlers for ports 0x81-0x83 (on the second DMA controller) */
		DmaControllers[1]->DMA_WriteHandler[0x10].Install(0x88,DMA_Write_Port,IO_MB,8);
		DmaControllers[1]->DMA_ReadHandler[0x10].Install(0x88,DMA_Read_Port,IO_MB,8);
	}

	/* FIXME: This should be in a separate EMS board init */
	for (i=0;i < LINK_START;i++) ems_board_mapping[i] = i;
}

void Init_DMA() {
	LOG(LOG_MISC,LOG_DEBUG)("Initializing DMA controller emulation");

	AddExitFunction(AddExitFunctionFuncPair(DMA_Destroy));
	AddVMEventFunction(VM_EVENT_RESET,AddVMEventFunctionFuncPair(DMA_Reset));
}

