/*
 * Copyright 2014 Patrick Dowling
 *
 * Author: Patrick Dowling for preenfm2 optimization
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef DWT_H_
#define DWT_H_

#include "RingBuffer.h"

#define REG_DWT_CONTROL 0xE0001000
#define REG_DWT_CYCCNT 0xE0001004
#define REG_SCB_DEMCR 0xE000EDFC

#define SHOW_CPU_USAGE 1

#define DWT_CONTROL ((volatile unsigned int *)REG_DWT_CONTROL)
#define DWT_CYCCNT ((volatile unsigned int *)REG_DWT_CYCCNT)
#define SCB_DEMCT ((volatile unsigned int *)REG_SCB_DEMCR)

#define RESET_DWT_CYCCNT()			\
        do {						\
            *SCB_DEMCT = *SCB_DEMCT | 0x01000000;		\
            *DWT_CYCCNT = 0;				\
            *DWT_CONTROL = *DWT_CONTROL | 1;		\
        } while ( 0 )

#define READ_DWT_CYCCNT()			\
        *(DWT_CYCCNT)


typedef RingBuffer<uint32_t, 32> CYCCNT_buffer;

/**
 * Utitity to automatically track cycles spent within a scope
 * Doesn't support nested measurements!
 */
class scoped_cyccnt
{
public:
    scoped_cyccnt( CYCCNT_buffer &_buffer )
: buffer( _buffer ) {
        RESET_DWT_CYCCNT();
    }

    ~scoped_cyccnt() {
        buffer.insert( READ_DWT_CYCCNT() );
    }

private:
    CYCCNT_buffer &buffer;
};

#define MACRO_CONCAT_(x,y) x##y
#define MACRO_CONCAT(x,y) MACRO_CONCAT_(x,y)

// Under PFM3_HOST the DWT/SCB hardware registers are unreachable on a host
// CPU — scoped_cyccnt's ctor runs RESET_DWT_CYCCNT() (writes SCB_DEMCR
// 0xE000EDFC, DWT_CYCCNT 0xE0001004, DWT_CONTROL 0xE0001000) and its dtor
// reads DWT_CYCCNT via READ_DWT_CYCCNT(); any of these memory-mapped ARM
// accesses would fault. CPU-load profiling is meaningless on host anyway, so
// the macros become no-ops (reusing the SHOW_CPU_USAGE=0 branch below). Inert
// under the Arm build (PFM3_HOST is never defined there). See tests/SEAM.md
// (refined rule: PFM3_HOST in firmware headers is allowed for genuinely
// host-incompatible constructs — libc redecls, ARM asm, section attrs, and
// hardware-register reads).
#if defined(SHOW_CPU_USAGE) && !defined(PFM3_HOST)
#define CYCLE_MEASURE_START( x )			\
        {							\
            scoped_cyccnt MACRO_CONCAT(CYCNT_,__COUNTER__)( x );	\
            do {} while(0)

#define CYCLE_MEASURE_END()			\
        }
#else
#define CYCLE_MEASURE_START( x ) do {} while(0)
#define CYCLE_MEASURE_END() do {} while(0)
#endif


#endif /* DWT_H_ */

