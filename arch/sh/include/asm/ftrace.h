/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_SH_FTRACE_H
#define __ASM_SH_FTRACE_H

#ifdef CONFIG_FUNCTION_TRACER

#define MCOUNT_INSN_SIZE	4 /* sizeof mcount call */
#define FTRACE_SYSCALL_MAX	NR_syscalls

#ifndef __ASSEMBLER__
extern void mcount(void);

#define MCOUNT_ADDR		((unsigned long)(mcount))

#ifdef CONFIG_DYNAMIC_FTRACE
#define CALL_ADDR		((long)(ftrace_call))
#define STUB_ADDR		((long)(ftrace_stub))
#define GRAPH_ADDR		((long)(ftrace_graph_call))
#define CALLER_ADDR		((long)(ftrace_caller))

#define MCOUNT_INSN_OFFSET	((STUB_ADDR - CALL_ADDR) - 4)
#define GRAPH_INSN_OFFSET	((CALLER_ADDR - GRAPH_ADDR) - 4)

struct dyn_arch_ftrace {
	/* No extra data needed on sh */
};

#endif /* CONFIG_DYNAMIC_FTRACE */

static inline unsigned long ftrace_call_adjust(unsigned long addr)
{
	/* 'addr' is the memory table address. */
	return addr;
}

void prepare_ftrace_return(unsigned long *parent, unsigned long self_addr);

#endif /* __ASSEMBLER__ */
#endif /* CONFIG_FUNCTION_TRACER */

#ifndef __ASSEMBLER__

/* arch/sh/kernel/return_address.c */
extern void *return_address(unsigned int);

#define ftrace_return_address(n) return_address(n)

#ifdef CONFIG_DYNAMIC_FTRACE
extern void arch_ftrace_nmi_enter(void);
extern void arch_ftrace_nmi_exit(void);
#else
static inline void arch_ftrace_nmi_enter(void) { }
static inline void arch_ftrace_nmi_exit(void) { }
#endif

#endif /* __ASSEMBLER__ */

#endif /* __ASM_SH_FTRACE_H */
