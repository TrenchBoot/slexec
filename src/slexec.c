/*
 * slexec.c: main entry point and pre-launch code for Trenchboot
 *
 * Used to be:
 * slexec.c: main entry point and "generic" routines for measured launch
 *          support
 *
 * Copyright (c) 2006-2010, Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Intel Corporation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <types.h>
#include <stdbool.h>
#include <slexec.h>
#include <stdarg.h>
#include <string.h>
#include <printk.h>
#include <loader.h>
#include <processor.h>
#include <misc.h>
#include <e820.h>
#include <linux.h>
#include <tpm.h>
#include <cmdline.h>
#include <txt/smx.h>
#include <txt/txt.h>
#include <skinit/skl.h>
#include <skinit/skinit.h>

uint32_t g_architecture = SL_ARCH_NONE;
uint32_t apic_base;

/* loader context struct saved so that post_launch() can use it */
loader_ctx g_loader_ctx = { NULL, 0 };
loader_ctx *g_ldr_ctx = &g_loader_ctx;

static uint32_t g_default_error_action = SL_SHUTDOWN_REBOOT;
static bool is_powercycle_required = true;

unsigned long get_slexec_mem_end(void)
{
    return PAGE_UP((unsigned long)&_end);
}

uint32_t get_apic_base(void)
{
    return apic_base;
}

static void shutdown_system(uint32_t shutdown_type)
{
    static const char *types[] = { "SL_SHUTDOWN_REBOOT", "SL_SHUTDOWN_SHUTDOWN",
                                   "SL_SHUTDOWN_HALT" };
    char type[32];

    /* NOTE: the TPM close and open current locality is not needed here since */
    /* since that only makes sense if this is called post laucnh which is */
    /* the case in SLEXEC */

    if ( shutdown_type >= ARRAY_SIZE(types) )
        sl_snprintf(type, sizeof(type), "unknown: %u", shutdown_type);
    else {
        sl_strncpy(type, types[shutdown_type], sizeof(type));
        type[sizeof(type) - 1] = '\0';
    }
    printk(SLEXEC_INFO"shutdown_system() called for shutdown_type: %s\n", type);

    switch( shutdown_type ) {
        case SL_SHUTDOWN_REBOOT:
            if ( is_powercycle_required ) {
                /* powercycle by writing 0x0a+0x0e to port 0xcf9 */
                /* (supported by all TXT-capable chipsets) */
                outb(0xcf9, 0x0a);
                outb(0xcf9, 0x0e);
            }
            else {
                /* soft reset by writing 0xfe to keyboard reset vector 0x64 */
                /* BIOSes (that are not performing some special operation, */
                /* such as update) will turn this into a platform reset as */
                /* expected. */
                outb(0x64, 0xfe);
                /* fall back to soft reset by writing 0x06 to port 0xcf9 */
                /* (supported by all TXT-capable chipsets) */
                outb(0xcf9, 0x06);
            }
            break;
        case SL_SHUTDOWN_SHUTDOWN:
            /* TODO implement S5 */
            break;
        /* FALLTHROUGH */
        case SL_SHUTDOWN_HALT:
        default:
            while ( true )
                halt();
    }
}

void error_action(int error)
{
    if ( error == SL_ERR_NONE )
        return;

    printk(SLEXEC_ERR"error action invoked for: %x\n", error);
    shutdown_system(g_default_error_action);
}

static bool prepare_cpu(void)
{
    unsigned long eflags, cr0;
    uint64_t mcg_cap, mcg_stat;
    bool preserve_mce = false;

    /* must be running at CPL 0 => this is implicit in even getting this far */
    /* since our bootstrap code loads a GDT, etc. */

    cr0 = read_cr0();

    /* must be in protected mode */
    if ( !(cr0 & CR0_PE) ) {
        printk(SLEXEC_ERR"ERR: not in protected mode\n");
        return false;
    }

    /* cache must be enabled (CR0.CD = CR0.NW = 0) */
    if ( cr0 & CR0_CD ) {
        printk(SLEXEC_INFO"CR0.CD set\n");
        cr0 &= ~CR0_CD;
    }
    if ( cr0 & CR0_NW ) {
        printk(SLEXEC_INFO"CR0.NW set\n");
        cr0 &= ~CR0_NW;
    }

    /* native FPU error reporting must be enabled for proper */
    /* interaction behavior */
    if ( !(cr0 & CR0_NE) ) {
        printk(SLEXEC_INFO"CR0.NE not set\n");
        cr0 |= CR0_NE;
    }

    write_cr0(cr0);

    /* cannot be in virtual-8086 mode (EFLAGS.VM=1) */
    eflags = read_eflags();
    if ( eflags & X86_EFLAGS_VM ) {
        printk(SLEXEC_INFO"EFLAGS.VM set\n");
        write_eflags(eflags | ~X86_EFLAGS_VM);
    }

    printk(SLEXEC_INFO"CR0 and EFLAGS OK\n");

    /*
     * verify all machine check status registers are clear (unless
     * support preserving them)
     */

    /* no machine check in progress (IA32_MCG_STATUS.MCIP=1) */
    mcg_stat = rdmsr(MSR_MCG_STATUS);
    if ( mcg_stat & 0x04 ) {
        printk(SLEXEC_ERR"machine check in progress\n");
        return false;
    }

    if (g_architecture == SL_ARCH_TXT) {
        getsec_parameters_t params;
        if ( !smx_get_parameters(&params) ) {
            printk(SLEXEC_ERR"smx_get_parameters() failed\n");
            return false;
        }

        if ( params.preserve_mce )
            printk(SLEXEC_INFO"TXT supports preserving machine check errors\n");
        else
            printk(SLEXEC_INFO"TXT no machine check errors\n");

        if ( params.proc_based_scrtm )
            printk(SLEXEC_INFO"TXT CPU support processor-based S-CRTM\n");

        preserve_mce = !!(params.preserve_mce);
    }

    /* check if all machine check regs are clear */
    mcg_cap = rdmsr(MSR_MCG_CAP);
    for ( unsigned int i = 0; i < (mcg_cap & 0xff); i++ ) {
        mcg_stat = rdmsr(MSR_MC0_STATUS + 4*i);
        if ( mcg_stat & (1ULL << 63) ) {
            printk(SLEXEC_ERR"MCG[%u] = %Lx ERROR\n", i, mcg_stat);
            if ( !preserve_mce )
                return false;
        }
    }

    printk(SLEXEC_INFO"Machine Check OK\n");

    return true;
}

static bool platform_architecture(void)
{
    unsigned long f1, f2;
     /* eax: regs[0], ebx: regs[1], ecx: regs[2], edx: regs[3] */
    uint32_t regs[4];

    /* is CPUID supported? */
    /* (it's supported if ID flag in EFLAGS can be set and cleared) */
    asm volatile ("pushf\n\t"
                  "pushf\n\t"
                  "pop %0\n\t"
                  "mov %0,%1\n\t"
                  "xor %2,%0\n\t"
                  "push %0\n\t"
                  "popf\n\t"
                  "pushf\n\t"
                  "pop %0\n\t"
                  "popf\n\t"
                  : "=&r" (f1), "=&r" (f2)
        :          "ir" (X86_EFLAGS_ID));
    if ( ((f1^f2) & X86_EFLAGS_ID) == 0 ) {
        printk(SLEXEC_ERR"CPUID instruction is not supported.\n");
        return false;
    }

    do_cpuid(CPUID_X86_MANUFACTURER_LEAF, regs);

    if ( regs[1] == 0x756e6547        /* "Genu" */
         && regs[2] == 0x6c65746e     /* "ntel" */
         && regs[3] == 0x49656e69 ) { /* "ineI" */
        printk(SLEXEC_INFO"Platform is Intel\n");
        g_architecture = SL_ARCH_TXT;
    }
    else if ( regs[1] == 0x68747541   /* "Auth" */
         && regs[2] == 0x444d4163     /* "cAMD" */
         && regs[3] == 0x69746e65 ) { /* "enti" */
        printk(SLEXEC_INFO"Platform is AMD\n");
        g_architecture = SL_ARCH_SKINIT;
    }
    else {
        printk(SLEXEC_ERR"Error: platform is neither Intel or AMD\n");
        return false;
    }

    return true;
}

void begin_launch(void *addr, uint32_t magic)
{
    const char *cmdline;
    int err;

    /* this is the SLEXEC module loader type, either MB1 or MB2 */
    determine_loader_type(addr, magic);

    cmdline = get_cmdline(g_ldr_ctx);
    sl_memset(g_cmdline, '\0', sizeof(g_cmdline));
    if ( cmdline )
        sl_strncpy(g_cmdline, cmdline, sizeof(g_cmdline)-1);

    /* always parse cmdline */
    slexec_parse_cmdline();

    g_default_error_action = get_error_shutdown();

    /* initialize all logging targets */
    printk_init();

    printk(SLEXEC_INFO"******************* SLEXEC *******************\n");
    printk(SLEXEC_INFO"   %s -- @ %p\n", SLEXEC_CHANGESET, _start);
    printk(SLEXEC_INFO"*********************************************\n");

    printk(SLEXEC_INFO"command line: %s\n", g_cmdline);

    if ( !platform_architecture() )
        error_action(SL_ERR_FATAL);

    /* we should only be executing on the BSP */
    apic_base = (uint32_t)rdmsr(MSR_APICBASE);
    if ( !(apic_base & APICBASE_BSP) ) {
        printk(SLEXEC_INFO"entry processor is not BSP\n");
        error_action(SL_ERR_FATAL);
    }
    printk(SLEXEC_INFO"BSP is cpu %u APIC base MSR: 0x%x\n", get_apicid(), apic_base);

    /* make copy of e820 map that we will use and adjust */
    if ( !copy_e820_map(g_ldr_ctx) )
        error_action(SL_ERR_FATAL);

    /* make TPM ready for measured launch */
    if ( !tpm_detect() )
       error_action(SL_ERR_TPM_NOT_READY);

    if (g_architecture == SL_ARCH_TXT) {
        /* TODO NOTE CPUID for features already moved out of read processorfunction.*/ 

        /* we need to make sure this is a (TXT-) capable platform before using */
        /* any of the features, incl. those required to check if the environment */
        /* has already been launched */

        /* need to verify that platform supports TXT before we can check error */
        /* (this includes TPM support). despite the name this function also */
        /* enables SMX mode in CR4. it needs to be done before attempting to */
        /* verify the ACMOD */

        /* TODO call supports_txt(). find a new name for it too */

        /* TODO call verify_IA32_se_svn_status */

        /* print any errors on last boot, which must be from TXT launch */
        txt_display_errors();
        if (txt_has_error() && !get_ignore_prev_err())
            error_action(SL_ERR_PREV_TXT_ERROR);

        /* TODO call verify_platform */
    }
    else {
        /* we need to make sure this is a (SKINIT) capable platform before using */
        /* any of the features, incl. those required to check if the environment */
        /* has already been launched */
        err = supports_skinit();
        error_action(err);
    }

    /* ensure there are modules */
    if ( !verify_loader_context(g_ldr_ctx) )
        error_action(SL_ERR_FATAL);

    /* make the CPU ready for secure launch */
    if ( !prepare_cpu() )
        error_action(SL_ERR_FATAL);

    if ( !prepare_tpm() )
        error_action(SL_ERR_TPM_NOT_READY);

    /* else ------------ */
    /* locate and load SKL module */
    if ( !find_skl_module(g_ldr_ctx) )
        error_action(SL_ERR_NO_SKL);
    /* TODO need to refactor the copy_sinit stuff here. It should
       be ok to do it here out of order with original code. */

    relocate_skl_module();
    print_skl_module();
    /* else ------------ */

    /* locate and prepare the secure launch kernel */
    if ( !prepare_intermediate_loader() )
        error_action(SL_ERR_FATAL);

    /* prepare the bootloader data area in the SKL */
    if ( !prepare_skl_bootloader_data() )
        error_action(SL_ERR_FATAL);

    /* launch the secure environment */
    skinit_launch_environment();
    /* No return */
}

void handle_exception(void)
{
    printk(SLEXEC_INFO"Received exception; shutting down...\n");
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
