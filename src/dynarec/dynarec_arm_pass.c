#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

#include "debug.h"
#include "box86context.h"
#include "dynarec.h"
#include "emu/x86emu_private.h"
#include "emu/x86run_private.h"
#include "x86run.h"
#include "x86emu.h"
#include "box86stack.h"
#include "emu/x86run_private.h"
#include "x86trace.h"
#include "dynarec_arm.h"
#include "dynarec_arm_private.h"
#include "arm_printer.h"
#include "dynarec_arm_functions.h"
#include "dynarec_arm_helper.h"
#include "custommem.h"
#include "elfloader.h"

#ifndef STEP
#error No STEP defined
#endif

uintptr_t arm_pass(dynarec_arm_t* dyn, uintptr_t addr)
{
    int ok = 1;
    int ninst = 0;
    int j32;
    uintptr_t ip = addr;
    uintptr_t init_addr = addr;
    MAYUSE(init_addr);
    int need_epilog = 1;
    // Clean up (because there are multiple passes)
    dyn->f.pending = 0;
    dyn->f.dfnone = 0;
    dyn->forward = 0;
    dyn->forward_to = 0;
    dyn->forward_size = 0;
    dyn->forward_ninst = 0;
    fpu_reset(dyn);
    int reset_n = -1;
    int stopblock = 2+(FindElfAddress(my_context, addr)?0:1); // if block is in elf_memory, it can be extended with bligblocks==2, else it needs 3    // ok, go now
    INIT;
    while(ok) {
        ip = addr;
        if (reset_n!=-1) {
            if(reset_n==-2) {
                MESSAGE(LOG_DEBUG, "Reset Caches to zero\n");
                dyn->f.dfnone = 0;
                dyn->f.pending = 0;
                fpu_reset(dyn);
            } else {
                fpu_reset_cache(dyn, ninst, reset_n);
                dyn->f = dyn->insts[reset_n].f_exit;
                if(dyn->insts[ninst].x86.barrier&BARRIER_FLOAT) {
                    MESSAGE(LOG_DEBUG, "Apply Barrier Float\n");
                    fpu_reset(dyn);
                }
                if(dyn->insts[ninst].x86.barrier&BARRIER_FLAGS) {
                    MESSAGE(LOG_DEBUG, "Apply Barrier Flags\n");
                    dyn->f.dfnone = 0;
                    dyn->f.pending = 0;
                }
            }
            reset_n = -1;
        }
        fpu_propagate_stack(dyn, ninst);
        NEW_INST;
        #if STEP == 0
        if(ninst && dyn->insts[ninst-1].x86.barrier_next) {
            BARRIER(dyn->insts[ninst-1].x86.barrier_next);
        }
        #endif
        if(!ninst) {
            GOTEST(x1, x2);
        }
        if(dyn->insts[ninst].pred_sz>1) {SMSTART();}
        fpu_reset_scratch(dyn);
        if((dyn->insts[ninst].x86.need_before&~X_PEND) && !dyn->insts[ninst].pred_sz) {
            READFLAGS(dyn->insts[ninst].x86.need_before&~X_PEND);
        }
        if(box86_dynarec_test)  {
            MESSAGE(LOG_DUMP, "TEST INIT  ----\n");
            fpu_reflectcache(dyn, ninst, x1, x2, x3);
            MOV32(x1, ip);
            STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP));
            STR_IMM9(x1, xEmu, offsetof(x86emu_t, ip));
            MOVW(x2, 1);
            CALL(x86test_init, -1, 0);
            fpu_unreflectcache(dyn, ninst, x1, x2, x3);
            MESSAGE(LOG_DUMP, "----------\n");
        }
#ifdef HAVE_TRACE
        else if(my_context->dec && box86_dynarec_trace) {
        if((trace_end == 0) 
            || ((ip >= trace_start) && (ip < trace_end)))  {
                MESSAGE(LOG_DUMP, "TRACE ----\n");
                fpu_reflectcache(dyn, ninst, x1, x2, x3);
                MOV32(x1, ip);
                STM(xEmu, (1<<xEAX)|(1<<xEBX)|(1<<xECX)|(1<<xEDX)|(1<<xESI)|(1<<xEDI)|(1<<xESP)|(1<<xEBP));
                STR_IMM9(x1, xEmu, offsetof(x86emu_t, ip));
                MOVW(x2, 1);
                CALL(PrintTrace, -1, 0);
                fpu_unreflectcache(dyn, ninst, x1, x2, x3);
                MESSAGE(LOG_DUMP, "----------\n");
            }
        }
#endif

        addr = dynarec00(dyn, addr, ip, ninst, &ok, &need_epilog);

        INST_EPILOG;

        int next = ninst+1;
        #if STEP > 0
        if(!dyn->insts[ninst].x86.has_next && dyn->insts[ninst].x86.jmp && dyn->insts[ninst].x86.jmp_insts!=-1)
            next = dyn->insts[ninst].x86.jmp_insts;
        #endif
        if(dyn->insts[ninst].x86.has_next && dyn->insts[next].x86.barrier) {
            if(dyn->insts[next].x86.barrier&BARRIER_FLOAT)
                fpu_purgecache(dyn, ninst, 0, x1, x2, x3);
            if(dyn->insts[next].x86.barrier&BARRIER_FLAGS) {
                dyn->f.pending = 0;
                dyn->f.dfnone = 0;
            }
        }
        #ifndef PROT_READ
        #define PROT_READ 1
        #endif
        #if STEP != 0
        if(!ok && !need_epilog && (addr < (dyn->start+dyn->isize))) {
            ok = 1;
            // we use the 1st predecessor here
            int ii = ninst+1;
            if(ii<dyn->size && !dyn->insts[ii].pred_sz) {
                while(ii<dyn->size && (!dyn->insts[ii].pred_sz || (dyn->insts[ii].pred_sz==1 && dyn->insts[ii].pred[0]==ii-1))) {
                    // may need to skip opcodes to advance
                    ++ninst;
                    NEW_INST;
                    MESSAGE(LOG_DEBUG, "Skipping unused opcode\n");
                    INST_NAME("Skipped opcode");
                    INST_EPILOG;
                    addr += dyn->insts[ii].x86.size;
                    ++ii;
                }
            }
            if((dyn->insts[ii].x86.barrier&BARRIER_FULL)==BARRIER_FULL)
                reset_n = -2;    // hack to say Barrier!
            else {
                reset_n = getNominalPred(dyn, ii);  // may get -1 if no predecessor are availble
                if(reset_n==-1) {
                    reset_n = -2;
                    MESSAGE(LOG_DEBUG, "Warning, Reset Caches mark not found\n");
                }
            }
        }
        #else
        if(dyn->forward) {
            if(dyn->forward_to == addr && !need_epilog) {
                // we made it!
                if(box86_dynarec_dump) dynarec_log(LOG_NONE, "Forward extend block for %d bytes %s%p -> %p\n", dyn->forward_to-dyn->forward, dyn->insts[dyn->forward_ninst].x86.has_callret?"(opt. call) ":"", (void*)dyn->forward, (void*)dyn->forward_to);
                if(dyn->insts[dyn->forward_ninst].x86.has_callret && !dyn->insts[dyn->forward_ninst].x86.has_next)
                    dyn->insts[dyn->forward_ninst].x86.has_next = 1;  // this block actually continue
                dyn->forward = 0;
                dyn->forward_to = 0;
                dyn->forward_size = 0;
                dyn->forward_ninst = 0;
                ok = 1; // in case it was 0
            } else if ((dyn->forward_to < addr) || !ok) {
                // something when wrong! rollback
                if(box86_dynarec_dump) dynarec_log(LOG_NONE, "Could not forward extend block for %d bytes %p -> %p\n", dyn->forward_to-dyn->forward, (void*)dyn->forward, (void*)dyn->forward_to);
                ok = 0;
                dyn->size = dyn->forward_size;
                ninst = dyn->forward_ninst;
                addr = dyn->forward;
                dyn->forward = 0;
                dyn->forward_to = 0;
                dyn->forward_size = 0;
                dyn->forward_ninst = 0;
            }
            // else just continue
        } else if(!ok && !need_epilog && box86_dynarec_bigblock && (getProtection(addr+3)&~PROT_READ))
            if(*(uint32_t*)addr!=0) {   // check if need to continue (but is next 4 bytes are 0, stop)
                uintptr_t next = get_closest_next(dyn, addr);
                if(next && (
                    (((next-addr)<15) && is_nops(dyn, addr, next-addr)) 
                    /*||(((next-addr)<30) && is_instructions(dyn, addr, next-addr))*/ ))
                {
                    ok = 1;
                    if(dyn->insts[ninst].x86.has_callret && !dyn->insts[ninst].x86.has_next) {
                        dyn->insts[ninst].x86.has_next = 1;  // this block actually continue
                    } else {
                        // need to find back that instruction to copy the caches, as previous version cannot be used anymore
                        // and pred table is not ready yet
                        reset_n = get_first_jump(dyn, next);
                    }
                    if(box86_dynarec_dump) dynarec_log(LOG_NONE, "Extend block %p, %s%p -> %p (ninst=%d, jump from %d)\n", dyn, dyn->insts[ninst].x86.has_callret?"(opt. call) ":"", (void*)addr, (void*)next, ninst, dyn->insts[ninst].x86.has_callret?ninst:reset_n);
                } else if(next && (next-addr)<box86_dynarec_forward && (getProtection(next)&PROT_READ)/*box86_dynarec_bigblock>=stopblock*/) {
                    if(!((box86_dynarec_bigblock<stopblock) && !isJumpTableDefault((void*)next))) {
                        if(dyn->forward) {
                            if(next<dyn->forward_to)
                                dyn->forward_to = next;
                            reset_n = -2;
                            ok = 1;
                        } else {
                            dyn->forward = addr;
                            dyn->forward_to = next;
                            dyn->forward_size = dyn->size;
                            dyn->forward_ninst = ninst;
                            reset_n = -2;
                            ok = 1;
                        }
                    }
                }
            }
        #endif
        if(ok<0)  {
            ok = 0; need_epilog=1; 
            #if STEP == 0
            if(ninst) {
                --ninst;
                if(!dyn->insts[ninst].x86.barrier) {
                    BARRIER(BARRIER_FLOAT);
                }
                dyn->insts[ninst].x86.need_after |= X_PEND;
                ++ninst;
            }
            if(dyn->forward) {
                // stopping too soon
                dyn->size = dyn->forward_size;
                ninst = dyn->forward_ninst+1;
                addr = dyn->forward;
                dyn->forward = 0;
                dyn->forward_to = 0;
                dyn->forward_size = 0;
                dyn->forward_ninst = 0;
            }
            #endif
        }
        ++ninst;
        #if STEP == 0
        if(ok && (((box86_dynarec_bigblock<stopblock) && !isJumpTableDefault((void*)addr)) 
            || (addr>=box86_nodynarec_start && addr<box86_nodynarec_end)))
        #else
        if(ok && (ninst==dyn->size))
        #endif
        {
            #if STEP == 0
            if(dyn->forward) {
                // stopping too soon
                dyn->size = dyn->forward_size;
                ninst = dyn->forward_ninst+1;
                addr = dyn->forward;
                dyn->forward = 0;
                dyn->forward_to = 0;
                dyn->forward_size = 0;
                dyn->forward_ninst = 0;
            }
            #endif
            int j32;
            MAYUSE(j32);
            MESSAGE(LOG_DEBUG, "Stopping block %p (%d / %d)\n",(void*)init_addr, ninst, dyn->size);
            if(!box86_dynarec_dump && addr>=box86_nodynarec_start && addr<box86_nodynarec_end)
                dynarec_log(LOG_INFO, "Stopping block in no-dynarec zone\n");
            --ninst;
            if(!dyn->insts[ninst].x86.barrier) {
                BARRIER(BARRIER_FLOAT);
            }
            #if STEP == 0
            dyn->insts[ninst].x86.need_after |= X_PEND;
            #endif
            ++ninst;
            NOTEST(x3);
            fpu_purgecache(dyn, ninst, 0, x1, x2, x3);
            jump_to_next(dyn, addr, 0, ninst);
            ok=0; need_epilog=0;
        }
    }
    if(need_epilog) {
        NOTEST(x3);
        fpu_purgecache(dyn, ninst, 0, x1, x2, x3);
        jump_to_epilog(dyn, ip, 0, ninst);  // no linker here, it's an unknow instruction
    }
    FINI;
    MESSAGE(LOG_DUMP, "---- END OF BLOCK ---- (%d)\n", dyn->size);
    return addr;
}