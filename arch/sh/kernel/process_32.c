/*
 * arch/sh/kernel/process.c
 *
 * This file handles the architecture-dependent parts of process handling..
 *
 *  Copyright (C) 1995  Linus Torvalds
 *
 *  SuperH version:  Copyright (C) 1999, 2000  Niibe Yutaka & Kaz Kojima
 *		     Copyright (C) 2006 Lineo Solutions Inc. support SH4A UBC
 *		     Copyright (C) 2002 - 2008  Paul Mundt
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/elfcore.h>
#include <linux/pm.h>
#include <linux/kallsyms.h>
#include <linux/kexec.h>
#include <linux/kdebug.h>
#include <linux/tick.h>
#include <linux/reboot.h>
#include <linux/fs.h>
#include <linux/ftrace.h>
#include <linux/preempt.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <asm/mmu_context.h>
#include <asm/pgalloc.h>
#include <asm/system.h>
#include <asm/ubc.h>
#include <asm/fpu.h>
#include <asm/watchdog.h>
#include <asm/syscalls.h>
#include <asm/watchdog.h>
#include <asm/restart.h>

#ifdef CONFIG_CC_STACKPROTECTOR
unsigned long __stack_chk_guard __read_mostly;
EXPORT_SYMBOL(__stack_chk_guard);
#endif

int ubc_usercnt = 0;

static void watchdog_trigger_immediate(void)
{
	sh_wdt_write_cnt(0xFF);
	sh_wdt_write_csr(0xC2);
}

LIST_HEAD(restart_prep_handler_list);

void register_prepare_restart_handler(void (*prepare_restart)(void))
{
	struct restart_prep_handler *s = (struct restart_prep_handler *)
				kmalloc(sizeof(struct restart_prep_handler),
				GFP_KERNEL);
	s->prepare_restart = prepare_restart;
	list_add(&(s->list), &(restart_prep_handler_list));
}

void machine_restart(char * __unused)
{
	struct restart_prep_handler *tmp;
	struct list_head *pos, *q;

	/* Run any "prepare restart" handlers */
	list_for_each_safe(pos, q, &restart_prep_handler_list) {
		tmp = list_entry(pos, struct restart_prep_handler, list);
		tmp->prepare_restart();
		list_del(pos);
		kfree(tmp);
	}

	local_irq_disable();

	/* Use watchdog timer to trigger reset */
	watchdog_trigger_immediate();

	while (1)
		cpu_sleep();
}

void machine_halt(void)
{
	local_irq_disable();

	while (1)
		cpu_sleep();
}

void machine_power_off(void)
{
	if (pm_power_off)
		pm_power_off();
}

void show_regs(struct pt_regs * regs)
{
	printk("\n");
	printk("Pid : %d, Comm: \t\t%s\n", task_pid_nr(current), current->comm);
	printk("CPU : %d        \t\t%s  (%s %.*s)\n\n",
	       smp_processor_id(), print_tainted(), init_utsname()->release,
	       (int)strcspn(init_utsname()->version, " "),
	       init_utsname()->version);

	print_symbol("PC is at %s\n", instruction_pointer(regs));
	print_symbol("PR is at %s\n", regs->pr);

	printk("PC  : %08lx SP  : %08lx SR  : %08lx ",
	       regs->pc, regs->regs[15], regs->sr);
#ifdef CONFIG_MMU
	printk("TEA : %08x\n", __raw_readl(MMU_TEA));
#else
	printk("\n");
#endif

	printk("R0  : %08lx R1  : %08lx R2  : %08lx R3  : %08lx\n",
	       regs->regs[0],regs->regs[1],
	       regs->regs[2],regs->regs[3]);
	printk("R4  : %08lx R5  : %08lx R6  : %08lx R7  : %08lx\n",
	       regs->regs[4],regs->regs[5],
	       regs->regs[6],regs->regs[7]);
	printk("R8  : %08lx R9  : %08lx R10 : %08lx R11 : %08lx\n",
	       regs->regs[8],regs->regs[9],
	       regs->regs[10],regs->regs[11]);
	printk("R12 : %08lx R13 : %08lx R14 : %08lx\n",
	       regs->regs[12],regs->regs[13],
	       regs->regs[14]);
	printk("MACH: %08lx MACL: %08lx GBR : %08lx PR  : %08lx\n",
	       regs->mach, regs->macl, regs->gbr, regs->pr);

	show_trace(current, (unsigned long *)regs->regs[15],
#ifdef CONFIG_FRAME_POINTER
	       (unsigned long *)regs->regs[14],
#else
	       0,
#endif
	       regs->pc, regs);
	show_code(regs);
}

/*
 * Create a kernel thread
 */
ATTRIB_NORET void kernel_thread_helper(void *arg, int (*fn)(void *))
{
	do_exit(fn(arg));
}

/* Don't use this in BL=1(cli).  Or else, CPU resets! */
int kernel_thread(int (*fn)(void *), void * arg, unsigned long flags)
{
	struct pt_regs regs;
	int pid;

	memset(&regs, 0, sizeof(regs));
	regs.regs[4] = (unsigned long)arg;
	regs.regs[5] = (unsigned long)fn;

	regs.pc = (unsigned long)kernel_thread_helper;
	regs.sr = SR_MD;
#if defined(CONFIG_SH_FPU)
	regs.sr |= SR_FD;
#endif

	/* Ok, create the new process.. */
	pid = do_fork(flags | CLONE_VM | CLONE_UNTRACED, 0,
		      &regs, 0, NULL, NULL);

	return pid;
}

/*
 * Free current thread data structures etc..
 */
void exit_thread(void)
{
	if (current->thread.ubc_pc) {
		current->thread.ubc_pc = 0;
		ubc_usercnt -= 1;
	}
}

void flush_thread(void)
{
#if defined(CONFIG_SH_FPU)
	struct task_struct *tsk = current;
	/* Forget lazy FPU state */
	clear_fpu(tsk, task_pt_regs(tsk));
	clear_used_math();
#endif
}

void release_thread(struct task_struct *dead_task)
{
	/* do nothing */
}

/* Fill in the fpu structure for a core dump.. */
int dump_fpu(struct pt_regs *regs, elf_fpregset_t *fpu)
{
	int fpvalid = 0;

#if defined(CONFIG_SH_FPU)
	struct task_struct *tsk = current;

	fpvalid = !!tsk_used_math(tsk);
	if (fpvalid)
		fpvalid = !fpregs_get(tsk, NULL, 0,
				      sizeof(struct user_fpu_struct),
				      fpu, NULL);
#endif

	return fpvalid;
}

/*
 * This gets called before we allocate a new thread and copy
 * the current task into it.
 */
void prepare_to_copy(struct task_struct *tsk)
{
	unlazy_fpu(tsk, task_pt_regs(tsk));
}

asmlinkage void ret_from_fork(void);

int copy_thread(unsigned long clone_flags, unsigned long usp,
		unsigned long unused,
		struct task_struct *p, struct pt_regs *regs)
{
	struct thread_info *ti = task_thread_info(p);
	struct pt_regs *childregs;
#if defined(CONFIG_SH_DSP)
	struct task_struct *tsk = current;
#endif

#if defined(CONFIG_SH_DSP)
	if (is_dsp_enabled(tsk)) {
		/* We can use the __save_dsp or just copy the struct:
		 * __save_dsp(p);
		 * p->thread.dsp_status.status |= SR_DSP
		 */
		p->thread.dsp_status = tsk->thread.dsp_status;
	}
#endif

	childregs = task_pt_regs(p);
	*childregs = *regs;

	if (user_mode(regs)) {
		childregs->regs[15] = usp;
		ti->addr_limit = USER_DS;
	} else {
		childregs->regs[15] = (unsigned long)childregs;
		ti->addr_limit = KERNEL_DS;
		ti->status &= ~TS_USEDFPU;
		p->fpu_counter = 0;
	}

	if (clone_flags & CLONE_SETTLS)
		childregs->gbr = childregs->regs[0];

	childregs->regs[0] = 0; /* Set return value for child */

	p->thread.sp = (unsigned long) childregs;
	p->thread.pc = (unsigned long) ret_from_fork;

	p->thread.ubc_pc = 0;

	return 0;
}

/* Tracing by user break controller.  */
static void ubc_set_tracing(int asid, unsigned long pc)
{
#if defined(CONFIG_CPU_SH4A)
	unsigned long val;

	val = (UBC_CBR_ID_INST | UBC_CBR_RW_READ | UBC_CBR_CE);
	val |= (UBC_CBR_AIE | UBC_CBR_AIV_SET(asid));

	__raw_writel(val, UBC_CBR0);
	__raw_writel(pc,  UBC_CAR0);
	__raw_writel(0x0, UBC_CAMR0);
	__raw_writel(0x0, UBC_CBCR);

	val = (UBC_CRR_RES | UBC_CRR_PCB | UBC_CRR_BIE);
	__raw_writel(val, UBC_CRR0);

	/* Read UBC register that we wrote last, for checking update */
	val = __raw_readl(UBC_CRR0);

#else	/* CONFIG_CPU_SH4A */
	__raw_writel(pc, UBC_BARA);

#ifdef CONFIG_MMU
	__raw_writeb(asid, UBC_BASRA);
#endif

	__raw_writeb(0, UBC_BAMRA);

	if (current_cpu_data.type == CPU_SH7729 ||
	    current_cpu_data.type == CPU_SH7710 ||
	    current_cpu_data.type == CPU_SH7712 ||
	    current_cpu_data.type == CPU_SH7203){
		__raw_writew(BBR_INST | BBR_READ | BBR_CPU, UBC_BBRA);
		__raw_writel(BRCR_PCBA | BRCR_PCTE, UBC_BRCR);
	} else {
		__raw_writew(BBR_INST | BBR_READ, UBC_BBRA);
		__raw_writew(BRCR_PCBA, UBC_BRCR);
	}
#endif	/* CONFIG_CPU_SH4A */
}

/*
 *	switch_to(x,y) should switch tasks from x to y.
 *
 */
__notrace_funcgraph struct task_struct *
__switch_to(struct task_struct *prev, struct task_struct *next)
{
	struct thread_struct *next_t = &next->thread;

#if defined CONFIG_CC_STACKPROTECTOR && !defined CONFIG_SMP
	__stack_chk_guard = next->stack_canary;
#endif

#if defined(CONFIG_SH_FPU)
	unlazy_fpu(prev, task_pt_regs(prev));

	/* we're going to use this soon, after a few expensive things */
	if (next->fpu_counter > 5)
		prefetch(&next_t->fpu.hard);
#endif

#ifdef CONFIG_MMU
	/*
	 * Restore the kernel mode register
	 *	k7 (r7_bank1)
	 */
	asm volatile("ldc	%0, r7_bank"
		     : /* no output */
		     : "r" (task_thread_info(next)));
#endif

	/* If no tasks are using the UBC, we're done */
	if (ubc_usercnt == 0)
		/* If no tasks are using the UBC, we're done */;
	else if (next->thread.ubc_pc && next->mm) {
		int asid = 0;
#ifdef CONFIG_MMU
		asid |= cpu_asid(smp_processor_id(), next->mm);
#endif
		ubc_set_tracing(asid, next->thread.ubc_pc);
	} else {
#if defined(CONFIG_CPU_SH4A)
		__raw_writel(UBC_CBR_INIT, UBC_CBR0);
		__raw_writel(UBC_CRR_INIT, UBC_CRR0);
#else
		__raw_writew(0, UBC_BBRA);
		__raw_writew(0, UBC_BBRB);
#endif
	}

#if defined(CONFIG_SH_FPU)
	/* If the task has used fpu the last 5 timeslices, just do a full
	 * restore of the math state immediately to avoid the trap; the
	 * chances of needing FPU soon are obviously high now
	 */
	if (next->fpu_counter > 5) {
		fpu_state_restore(task_pt_regs(next));
	}
#endif

	return prev;
}

asmlinkage int sys_fork(unsigned long r4, unsigned long r5,
			unsigned long r6, unsigned long r7,
			struct pt_regs __regs)
{
#ifdef CONFIG_MMU
	struct pt_regs *regs = RELOC_HIDE(&__regs, 0);
	return do_fork(SIGCHLD, regs->regs[15], regs, 0, NULL, NULL);
#else
	/* fork almost works, enough to trick you into looking elsewhere :-( */
	return -EINVAL;
#endif
}

asmlinkage int sys_clone(unsigned long clone_flags, unsigned long newsp,
			 unsigned long parent_tidptr,
			 unsigned long child_tidptr,
			 struct pt_regs __regs)
{
	struct pt_regs *regs = RELOC_HIDE(&__regs, 0);
	if (!newsp)
		newsp = regs->regs[15];
	return do_fork(clone_flags, newsp, regs, 0,
			(int __user *)parent_tidptr,
			(int __user *)child_tidptr);
}

/*
 * This is trivial, and on the face of it looks like it
 * could equally well be done in user mode.
 *
 * Not so, for quite unobvious reasons - register pressure.
 * In user mode vfork() cannot have a stack frame, and if
 * done by calling the "clone()" system call directly, you
 * do not have enough call-clobbered registers to hold all
 * the information you need.
 */
asmlinkage int sys_vfork(unsigned long r4, unsigned long r5,
			 unsigned long r6, unsigned long r7,
			 struct pt_regs __regs)
{
	struct pt_regs *regs = RELOC_HIDE(&__regs, 0);
	return do_fork(CLONE_VFORK | CLONE_VM | SIGCHLD, regs->regs[15], regs,
		       0, NULL, NULL);
}

/*
 * sys_execve() executes a new program.
 */
asmlinkage int sys_execve(char __user *ufilename, char __user * __user *uargv,
			  char __user * __user *uenvp, unsigned long r7,
			  struct pt_regs __regs)
{
	struct pt_regs *regs = RELOC_HIDE(&__regs, 0);
	int error;
	char *filename;

	filename = getname(ufilename);
	error = PTR_ERR(filename);
	if (IS_ERR(filename))
		goto out;

	error = do_execve(filename, uargv, uenvp, regs);
	putname(filename);
out:
	return error;
}

unsigned long get_wchan(struct task_struct *p)
{
	unsigned long pc;

	if (!p || p == current || p->state == TASK_RUNNING)
		return 0;

	/*
	 * The same comment as on the Alpha applies here, too ...
	 */
	pc = thread_saved_pc(p);

#ifdef CONFIG_FRAME_POINTER
	if (in_sched_functions(pc)) {
		unsigned long schedule_frame = (unsigned long)p->thread.sp;
		return ((unsigned long *)schedule_frame)[21];
	}
#endif

	return pc;
}

asmlinkage void break_point_trap(void)
{
	/* Clear tracing.  */
#if defined(CONFIG_CPU_SH4A)
	__raw_writel(UBC_CBR_INIT, UBC_CBR0);
	__raw_writel(UBC_CRR_INIT, UBC_CRR0);
#else
	__raw_writew(0, UBC_BBRA);
	__raw_writew(0, UBC_BBRB);
	__raw_writel(0, UBC_BRCR);
#endif
	current->thread.ubc_pc = 0;
	ubc_usercnt -= 1;

	force_sig(SIGTRAP, current);
}
