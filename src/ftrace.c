/*
Vendor Reset - Vendor Specific Reset
Copyright (C) 2020 Geoffrey McRae <geoff@hostfission.com>
Copyright (C) 2020 Adam Madsen <adam@ajmadsen.com>

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/ftrace.h>
#include <linux/kprobes.h>
#include <linux/pci.h>
#include <linux/version.h>

#include "ftrace.h"

static int resolve_hook_address(struct ftrace_hook *hook)
{
  struct kprobe kp = { .symbol_name = hook->name };

  if (register_kprobe(&kp))
  {
    pr_warn("vendor_reset_ftrace: unresolved symbol %s\n", hook->name);
    return -ENOENT;
  }
  hook->address = (unsigned long)kp.addr;
  unregister_kprobe(&kp);

  *((unsigned long *)hook->original) = hook->address;
  return 0;
}

#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0))
static void notrace fh_trace_thunk(unsigned long ip, unsigned long parent_ip, struct ftrace_ops *ops, struct pt_regs *regs)
{
  struct ftrace_hook *hook = to_ftrace_hook(ops);

  if (!within_module(parent_ip, THIS_MODULE))
    regs->ip = (unsigned long)hook->function;
}
#else
static void notrace fh_trace_thunk(unsigned long ip, unsigned long parent_ip, struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
  struct pt_regs *regs = ftrace_get_regs(fregs);
  struct ftrace_hook *hook = to_ftrace_hook(ops);

  if (regs && !within_module(parent_ip, THIS_MODULE))
    regs->ip = (unsigned long)hook->function;
}
#endif

int fh_install_hook(struct ftrace_hook *hook)
{
  int err;

  err = resolve_hook_address(hook);
  if (err)
    return err;

  hook->ops.func = fh_trace_thunk;
  hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS |
                    FTRACE_OPS_FL_IPMODIFY;

  err = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
  if (err)
    return err;

  err = register_ftrace_function(&hook->ops);
  if (err)
  {
    ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
    return err;
  }

  return 0;
}

void fh_remove_hook(struct ftrace_hook *hook)
{
  unregister_ftrace_function(&hook->ops);
  ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
}

int fh_install_hooks(struct ftrace_hook *hooks)
{
  int ret;
  struct ftrace_hook *hook;

  for (hook = hooks; hook->name; ++hook)
    if ((ret = fh_install_hook(hook)))
      return ret;

  return 0;
}

void fh_remove_hooks(struct ftrace_hook *hooks)
{
  struct ftrace_hook *hook;

  for (hook = hooks; hook->name; ++hook)
    fh_remove_hook(hook);
}
