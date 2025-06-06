/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2017 Andes Technology Corporation */

#ifndef _ASM_RISCV_FTRACE_H
#define _ASM_RISCV_FTRACE_H

/*
 * The graph frame test is not possible if CONFIG_FRAME_POINTER is not enabled.
 * Check arch/riscv/kernel/mcount.S for detail.
 */
#if defined(CONFIG_FUNCTION_GRAPH_TRACER) && defined(CONFIG_FRAME_POINTER)
#define HAVE_FUNCTION_GRAPH_FP_TEST
#endif

#define ARCH_SUPPORTS_FTRACE_OPS 1
#ifndef __ASSEMBLY__

extern void *return_address(unsigned int level);

#define ftrace_return_address(n) return_address(n)

void _mcount(void);
unsigned long ftrace_call_adjust(unsigned long addr);
unsigned long arch_ftrace_get_symaddr(unsigned long fentry_ip);
#define ftrace_get_symaddr(fentry_ip) arch_ftrace_get_symaddr(fentry_ip)

/*
 * Let's do like x86/arm64 and ignore the compat syscalls.
 */
#define ARCH_TRACE_IGNORE_COMPAT_SYSCALLS
static inline bool arch_trace_is_compat_syscall(struct pt_regs *regs)
{
	return is_compat_task();
}

#define ARCH_HAS_SYSCALL_MATCH_SYM_NAME
static inline bool arch_syscall_match_sym_name(const char *sym,
					       const char *name)
{
	/*
	 * Since all syscall functions have __riscv_ prefix, we must skip it.
	 * However, as we described above, we decided to ignore compat
	 * syscalls, so we don't care about __riscv_compat_ prefix here.
	 */
	return !strcmp(sym + 8, name);
}

struct dyn_arch_ftrace {
};
#endif

#ifdef CONFIG_DYNAMIC_FTRACE
/*
 * A general call in RISC-V is a pair of insts:
 * 1) auipc: setting high-20 pc-related bits to ra register
 * 2) jalr: setting low-12 offset to ra, jump to ra, and set ra to
 *          return address (original pc + 4)
 *
 * The first 2 instructions for each tracable function is compiled to 2 nop
 * instructions. Then, the kernel initializes the first instruction to auipc at
 * boot time (<ftrace disable>). The second instruction is patched to jalr to
 * start the trace.
 *
 *<Image>:
 * 0: nop
 * 4: nop
 *
 *<ftrace enable>:
 * 0: auipc  t0, 0x?
 * 4: jalr   t0, ?(t0)
 *
 *<ftrace disable>:
 * 0: auipc  t0, 0x?
 * 4: nop
 *
 * Dynamic ftrace generates probes to call sites, so we must deal with
 * both auipc and jalr at the same time.
 */

#define MCOUNT_ADDR		((unsigned long)_mcount)
#define JALR_SIGN_MASK		(0x00000800)
#define JALR_OFFSET_MASK	(0x00000fff)
#define AUIPC_OFFSET_MASK	(0xfffff000)
#define AUIPC_PAD		(0x00001000)
#define JALR_SHIFT		20
#define JALR_T0			(0x000282e7)
#define AUIPC_T0		(0x00000297)
#define JALR_RANGE		(JALR_SIGN_MASK - 1)

#define to_jalr_t0(offset)						\
	(((offset & JALR_OFFSET_MASK) << JALR_SHIFT) | JALR_T0)

#define to_auipc_t0(offset)						\
	((offset & JALR_SIGN_MASK) ?					\
	(((offset & AUIPC_OFFSET_MASK) + AUIPC_PAD) | AUIPC_T0) :	\
	((offset & AUIPC_OFFSET_MASK) | AUIPC_T0))

#define make_call_t0(caller, callee, call)				\
do {									\
	unsigned int offset =						\
		(unsigned long) (callee) - (unsigned long) (caller);	\
	call[0] = to_auipc_t0(offset);					\
	call[1] = to_jalr_t0(offset);					\
} while (0)

/*
 * Only the jalr insn in the auipc+jalr is patched, so we make it 4
 * bytes here.
 */
#define MCOUNT_INSN_SIZE	4
#define MCOUNT_AUIPC_SIZE	4
#define MCOUNT_JALR_SIZE	4
#define MCOUNT_NOP4_SIZE	4

#ifndef __ASSEMBLY__
struct dyn_ftrace;
int ftrace_init_nop(struct module *mod, struct dyn_ftrace *rec);
#define ftrace_init_nop ftrace_init_nop

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_ARGS
#define arch_ftrace_get_regs(regs) NULL
#define HAVE_ARCH_FTRACE_REGS
struct ftrace_ops;
struct ftrace_regs;
#define arch_ftrace_regs(fregs) ((struct __arch_ftrace_regs *)(fregs))

struct __arch_ftrace_regs {
	unsigned long epc;
	unsigned long ra;
	unsigned long sp;
	unsigned long s0;
	unsigned long t1;
#ifdef CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS
	unsigned long direct_tramp;
#endif
	union {
		unsigned long args[8];
		struct {
			unsigned long a0;
			unsigned long a1;
			unsigned long a2;
			unsigned long a3;
			unsigned long a4;
			unsigned long a5;
			unsigned long a6;
			unsigned long a7;
#ifdef CONFIG_CC_IS_CLANG
			unsigned long t2;
			unsigned long t3;
			unsigned long t4;
			unsigned long t5;
			unsigned long t6;
#endif
		};
	};
};

static __always_inline unsigned long ftrace_regs_get_instruction_pointer(const struct ftrace_regs
									 *fregs)
{
	return arch_ftrace_regs(fregs)->epc;
}

static __always_inline void ftrace_regs_set_instruction_pointer(struct ftrace_regs *fregs,
								unsigned long pc)
{
	arch_ftrace_regs(fregs)->epc = pc;
}

static __always_inline unsigned long ftrace_regs_get_stack_pointer(const struct ftrace_regs *fregs)
{
	return arch_ftrace_regs(fregs)->sp;
}

static __always_inline unsigned long ftrace_regs_get_frame_pointer(const struct ftrace_regs *fregs)
{
	return arch_ftrace_regs(fregs)->s0;
}

static __always_inline unsigned long ftrace_regs_get_argument(struct ftrace_regs *fregs,
							      unsigned int n)
{
	if (n < 8)
		return arch_ftrace_regs(fregs)->args[n];
	return 0;
}

static __always_inline unsigned long ftrace_regs_get_return_value(const struct ftrace_regs *fregs)
{
	return arch_ftrace_regs(fregs)->a0;
}

static __always_inline unsigned long ftrace_regs_get_return_address(const struct ftrace_regs *fregs)
{
	return arch_ftrace_regs(fregs)->ra;
}

static __always_inline void ftrace_regs_set_return_value(struct ftrace_regs *fregs,
							 unsigned long ret)
{
	arch_ftrace_regs(fregs)->a0 = ret;
}

static __always_inline void ftrace_override_function_with_return(struct ftrace_regs *fregs)
{
	arch_ftrace_regs(fregs)->epc = arch_ftrace_regs(fregs)->ra;
}

static __always_inline struct pt_regs *
ftrace_partial_regs(const struct ftrace_regs *fregs, struct pt_regs *regs)
{
	struct __arch_ftrace_regs *afregs = arch_ftrace_regs(fregs);

	memcpy(&regs->a_regs, afregs->args, sizeof(afregs->args));
	regs->epc = afregs->epc;
	regs->ra = afregs->ra;
	regs->sp = afregs->sp;
	regs->s0 = afregs->s0;
	regs->t1 = afregs->t1;
	return regs;
}

int ftrace_regs_query_register_offset(const char *name);

void ftrace_graph_func(unsigned long ip, unsigned long parent_ip,
		       struct ftrace_ops *op, struct ftrace_regs *fregs);
#define ftrace_graph_func ftrace_graph_func

#ifdef CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS
static inline void arch_ftrace_set_direct_caller(struct ftrace_regs *fregs, unsigned long addr)
{
	arch_ftrace_regs(fregs)->t1 = addr;
}
#endif /* CONFIG_DYNAMIC_FTRACE_WITH_DIRECT_CALLS */

#endif /* CONFIG_DYNAMIC_FTRACE_WITH_ARGS */

#endif /* __ASSEMBLY__ */

#endif /* CONFIG_DYNAMIC_FTRACE */

#endif /* _ASM_RISCV_FTRACE_H */
