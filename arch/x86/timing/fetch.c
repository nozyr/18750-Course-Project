/*
 *  Multi2Sim
 *  Copyright (C) 2012  Rafael Ubal (ubal@ece.neu.edu)
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
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include <arch/x86/emu/context.h>
#include <arch/x86/emu/regs.h>
 #include <arch/x86/emu/emu.h>
#include <lib/esim/trace.h>
#include <lib/util/debug.h>
#include <lib/util/list.h>
#include <lib/util/string.h>
#include <mem-system/mmu.h>
#include <mem-system/module.h>

#include "bpred.h"
#include "core.h"
#include "cpu.h"
#include "event-queue.h"
#include "fetch.h"
#include "fetch-queue.h"
#include "thread.h"
#include "trace-cache.h"
#include "uop.h"



/*
 * Class 'X86Thread'
 */


static int X86ThreadCanFetch(X86Thread *self)
{
	X86Cpu *cpu = self->cpu;
	X86Context *ctx = self->ctx;

	unsigned int phy_addr;
	unsigned int block;

	/* Context must be running */
	if (!ctx || !X86ContextGetState(ctx, X86ContextRunning))
		return 0;
	
	/* Fetch stalled or context evict signal activated */
	if (self->fetch_stall_until >= asTiming(cpu)->cycle || ctx->evict_signal)
		return 0;
	
	/* Fetch queue must have not exceeded the limit of stored bytes
	 * to be able to store new macro-instructions. */
	if (self->fetchq_occ >= x86_fetch_queue_size)
		return 0;
	
	/* If the next fetch address belongs to a new block, cache system
	 * must be accessible to read it. */
	block = self->fetch_neip & ~(self->inst_mod->block_size - 1);
	if (block != self->fetch_block)
	{
		phy_addr = mmu_translate(self->ctx->address_space_index,
			self->fetch_neip);
		if (!mod_can_access(self->inst_mod, phy_addr))
			return 0;
	}

        /* Pallavi- Check for exisiting LL uop pending on behalf of the thread to update the
           instruction based predictor */ 	
        if (X86ThreadLongLatencyInEventQueue(self))
        {
               //fprintf(stderr, "Pallavi- X86ThreadCanFetch - found LL event pending for thread:%s\n", self->name);
        }
	
	/* We can fetch */
	return 1;
}

/* New function to process instructions from fetch queue
 * Process this uop in I-predictor of thread
 */                
static int X86ThreadIPredictorProcess(X86Thread *self, struct x86_uop_t *uop)
{
	X86Cpu *cpu = self->cpu;
	struct x86_bpred_t *bpred = self->bpred;
	unsigned int bht_index;
	unsigned int bhr;  
	bht_index = uop->eip & (x86_bpred_twolevel_l1size - 1);
	bhr = bpred->twolevel_bht[bht_index];
	unsigned int eip = uop->eip^bhr;

	struct x86_inst_pred_t *pred = 0;

	// Find the instr ptr
	for (int i =0; i<MAX_PRED_BUF; i++)
	{
		if (self->ipred[i].pc == eip)
		{ 
		    pred = &(self->ipred[i]);
		}
   }
        
   if (pred == 0)
   { 
        return 0;
   } 

   int curr_pattern_ind = pred->curr_pattern_index;
   struct x86_inst_pattern_t *pattern = &pred->pattern_history[curr_pattern_ind]; 
   
   enum x86_uinst_flag_t flags = uop->flags;
   
   if (flags & X86_UINST_INT)
       pattern->uinst_int_count ++;
   if (flags & X86_UINST_LOGIC)
       pattern->uinst_logic_count ++;
   if (flags & X86_UINST_FP)
       pattern->uinst_fp_count ++;
   if (flags & X86_UINST_MEM)
       pattern->uinst_mem_count ++;
   if (flags & X86_UINST_CTRL)
       pattern->uinst_ctrl_count ++;
   
   pattern->uinst_total ++;

/*
fprintf(stderr, "Thread[%s]- found PC[%d] uop.eip[%d] bhr[%d] in ipred in fetch queue: [%lld,%lld,%lld,%lld,%lld]\n", self->name, 
pred->pc,
uop->eip,
bhr,
pattern->uinst_int_count,
pattern->uinst_logic_count,
pattern->uinst_fp_count,
pattern->uinst_mem_count,
pattern->uinst_ctrl_count
);
*/

       /*Ready the predictor for next memory instruction*/
   int total_inst_count = 0;
   float avg_inst_count = 0;
   for (int i=0; i <= curr_pattern_ind; i++)
   {
        total_inst_count += pred->pattern_history[i].uinst_total;
   }
   if (curr_pattern_ind)
        avg_inst_count = total_inst_count/curr_pattern_ind;

   int distance = avg_inst_count - pattern->uinst_total;
   //pred->next_pred_mem_inst_distance = distance;
   
   if (x86_tracing())
   {
   x86_trace("X86ThreadIPredictorProcess: %d,%d,%d,%d,%f,%lld,%lld\n",
   pred->next_pred_mem_inst_distance,
       pred->curr_pattern_index,
       curr_pattern_ind,
       total_inst_count,
       avg_inst_count,
       pattern->uinst_total,
       pred->total_pattern_processed
       );  
   }

   // Since this instruction is now in the fetch queue - update the prediction of 
   // thread.
   if (self->ctx->ll_pred_remaining_cycles == 0)
   {
       self->ctx->ll_pred_remaining_cycles = pred->remaining_cycles;
       self->ctx->when_predicted = pred->when_predicted;
       self->ctx->confidence = pred->confidence;
   }
   else 
   {
       long long time1 = self->ctx->ll_pred_remaining_cycles + self->ctx->when_predicted;
       long long time2 = pred->remaining_cycles + pred->when_predicted;
       int time1_valid =  time1 > (asTiming(cpu)->cycle) ? 1 : 0;
       int time2_valid =  time2 > (asTiming(cpu)->cycle) ? 1 : 0;
       //if (self->ctx->ll_pred_remaining_cycles > pred->remaining_cycles)
       if (time1_valid && time2_valid)
       {
		   if (time2 < time1)
		   {
			   self->ctx->ll_pred_remaining_cycles = pred->remaining_cycles;
                           self->ctx->when_predicted = pred->when_predicted;
			   self->ctx->confidence = pred->confidence;
		   } 
		   else if (pred->confidence > self->ctx->confidence)
		   {
			   self->ctx->ll_pred_remaining_cycles = pred->remaining_cycles;
                           self->ctx->when_predicted = pred->when_predicted;
			   self->ctx->confidence = pred->confidence;
		   }
       }

       if (!time1_valid) 
       {
		   self->ctx->ll_pred_remaining_cycles = pred->remaining_cycles;
		   self->ctx->when_predicted = pred->when_predicted;
		   self->ctx->confidence = pred->confidence;
       } 
   }

#define EVICTION_THRESHOLD_CYCLES 1000
   int pred_distance = (self->ctx->when_predicted + self->ctx->ll_pred_remaining_cycles) - (asTiming(cpu)->cycle);
   
   if (pred_distance>0 && pred_distance < EVICTION_THRESHOLD_CYCLES && self->ctx->confidence > 1 && !self->next_ctx)
   {
           X86Emu *emu = self->ctx->emu;
           // self->ctx->evict_signal = 1;
           emu->schedule_signal = 1;
           fprintf(stderr," Send reschd signal curr cycle:%lld ctx %d on thread %s pred cycles:%lld dist:%d\n",
                           asTiming(cpu)->cycle, self->ctx->pid, self->name, (self->ctx->when_predicted + self->ctx->ll_pred_remaining_cycles), pred_distance);
   }

   return 1;
} 

/* Run the emulation of one x86 macro-instruction and create its uops.
 * If any of the uops is a control uop, this uop will be the return value of
 * the function. Otherwise, the first decoded uop is returned. */
static struct x86_uop_t *X86ThreadFetchInst(X86Thread *self, int fetch_trace_cache)
{
	X86Cpu *cpu = self->cpu;
	X86Core *core = self->core;
	X86Context *ctx = self->ctx;

	struct x86_uop_t *uop;
	struct x86_uop_t *ret_uop;

	struct x86_uinst_t *uinst;
	int uinst_count;
	int uinst_index;

	/* Functional simulation */
	self->fetch_eip = self->fetch_neip;
	X86ContextSetEip(ctx, self->fetch_eip);
	X86ContextExecute(ctx);
	self->fetch_neip = self->fetch_eip + ctx->inst.size;

	/* If no micro-instruction was generated by this instruction, create a
	 * 'nop' micro-instruction. This makes sure that there is always a micro-
	 * instruction representing the regular control flow of macro-instructions
	 * of the program. It is important for the traces stored in the trace
	 * cache. */
	if (!x86_uinst_list->count)
		x86_uinst_new(ctx, x86_uinst_nop, 0, 0, 0, 0, 0, 0, 0);

	/* Micro-instructions created by the x86 instructions can be found now
	 * in 'x86_uinst_list'. */
	uinst_count = list_count(x86_uinst_list);
	uinst_index = 0;
	ret_uop = NULL;
	while (list_count(x86_uinst_list))
	{
		/* Get uinst from head of list */
		uinst = list_remove_at(x86_uinst_list, 0);

		/* Create uop */
		uop = x86_uop_create();
		uop->uinst = uinst;
		assert(uinst->opcode >= 0 && uinst->opcode < x86_uinst_opcode_count);
		uop->flags = x86_uinst_info[uinst->opcode].flags;
		uop->id = cpu->uop_id_counter++;
		uop->id_in_core = core->uop_id_counter++;

		uop->ctx = ctx;
		uop->thread = self;

		uop->mop_count = uinst_count;
		uop->mop_size = ctx->inst.size;
		uop->mop_id = uop->id - uinst_index;
		uop->mop_index = uinst_index;

		uop->eip = self->fetch_eip;
		uop->in_fetch_queue = 1;
		uop->trace_cache = fetch_trace_cache;
		uop->specmode = X86ContextGetState(ctx, X86ContextSpecMode);
		uop->fetch_address = self->fetch_address;
		uop->fetch_access = self->fetch_access;
		uop->neip = ctx->regs->eip;
		uop->pred_neip = self->fetch_neip;
		uop->target_neip = ctx->target_eip;

		/* Process uop dependences and classify them in integer, floating-point,
		 * flags, etc. */
		x86_uop_count_deps(uop);

		/* Calculate physical address of a memory access */
		if (uop->flags & X86_UINST_MEM)
			uop->phy_addr = mmu_translate(self->ctx->address_space_index,
				uinst->address);

		/* Trace */
		if (x86_tracing())
		{
			char str[MAX_STRING_SIZE];
			char inst_name[MAX_STRING_SIZE];
			char uinst_name[MAX_STRING_SIZE];

			char *str_ptr;

			int str_size;

			str_ptr = str;
			str_size = sizeof str;

			/* Command */
			str_printf(&str_ptr, &str_size, "x86.new_inst id=%lld core=%d",
				uop->id_in_core, core->id);

			/* Speculative mode */
			if (uop->specmode)
				str_printf(&str_ptr, &str_size, " spec=\"t\"");

			/* Macro-instruction name */
			if (!uinst_index)
			{
				x86_inst_dump_buf(&ctx->inst, inst_name, sizeof inst_name);
				str_printf(&str_ptr, &str_size, " asm=\"%s\"", inst_name);
			}

			/* Rest */
			x86_uinst_dump_buf(uinst, uinst_name, sizeof uinst_name);
			str_printf(&str_ptr, &str_size, " uasm=\"%s\" stg=\"fe\"", uinst_name);

			/* Dump */
			x86_trace("%s\n", str);
		}

		/* Select as returned uop */
		if (!ret_uop || (uop->flags & X86_UINST_CTRL))
			ret_uop = uop;

		/* Insert into fetch queue */
		list_add(self->fetch_queue, uop);

                /* Process this uop in ipred */
                X86ThreadIPredictorProcess(self, uop); 

		if (fetch_trace_cache)
			self->trace_cache_queue_occ++;

		/* Statistics */
		cpu->num_fetched_uinst++;
		self->num_fetched_uinst++;
		if (fetch_trace_cache)
			self->trace_cache->num_fetched_uinst++;

		/* Next uinst */
		uinst_index++;
	}

	/* Increase fetch queue occupancy if instruction does not come from
	 * trace cache, and return. */
	if (ret_uop && !fetch_trace_cache)
		self->fetchq_occ += ret_uop->mop_size;
	return ret_uop;
}


/* Try to fetch instruction from trace cache.
 * Return true if there was a hit and fetching succeeded. */
static int X86ThreadFetchTraceCache(X86Thread *self)
{
	struct x86_uop_t *uop;

	int mpred;
	int hit;
	int mop_count;
	int i;

	unsigned int eip_branch;  /* next branch address */
	unsigned int *mop_array;
	unsigned int neip;

	/* No room in trace cache queue */
	assert(x86_trace_cache_present);
	if (self->trace_cache_queue_occ >= x86_trace_cache_queue_size)
		return 0;
	
	/* Access BTB, branch predictor, and trace cache */
	eip_branch = X86ThreadGetNextBranch(self,
			self->fetch_neip, self->inst_mod->block_size);
	mpred = eip_branch ? X86ThreadLookupBranchPredMultiple(self,
			eip_branch, x86_trace_cache_branch_max) : 0;
	hit = X86ThreadLookupTraceCache(self, self->fetch_neip, mpred,
			&mop_count, &mop_array, &neip);
	if (!hit)
		return 0;
	
	/* Fetch instruction in trace cache line. */
	for (i = 0; i < mop_count; i++)
	{
		/* If instruction caused context to suspend or finish */
		if (!X86ContextGetState(self->ctx, X86ContextRunning))
			break;
		
		/* Insert decoded uops into the trace cache queue. In the simulation,
		 * the uop is inserted into the fetch queue, but its occupancy is not
		 * increased. */
		self->fetch_neip = mop_array[i];
		uop = X86ThreadFetchInst(self, 1);
		if (!uop)  /* no uop was produced by this macroinst */
			continue;

		/* If instruction is a branch, access branch predictor just in order
		 * to have the necessary information to update it at commit. */
		if (uop->flags & X86_UINST_CTRL)
		{
			X86ThreadLookupBranchPred(self, uop);
			uop->pred_neip = i == mop_count - 1 ? neip :
				mop_array[i + 1];
		}
	}

	/* Set next fetch address as returned by the trace cache, and exit. */
	self->fetch_neip = neip;
	return 1;
}


static void X86ThreadFetch(X86Thread *self)
{
	X86Context *ctx = self->ctx;
	struct x86_uop_t *uop;

	unsigned int phy_addr;
	unsigned int block;
	unsigned int target;

	int taken;

	/* Try to fetch from trace cache first */
	if (x86_trace_cache_present && X86ThreadFetchTraceCache(self))
		return;
	
	/* If new block to fetch is not the same as the previously fetched (and stored)
	 * block, access the instruction cache. */
	block = self->fetch_neip & ~(self->inst_mod->block_size - 1);
	if (block != self->fetch_block)
	{
		phy_addr = mmu_translate(self->ctx->address_space_index, self->fetch_neip);
		self->fetch_block = block;
		self->fetch_address = phy_addr;
		self->fetch_access = mod_access(self->inst_mod,
			mod_access_load, phy_addr, NULL, NULL, NULL, NULL);
		self->btb_reads++;

		/* MMU statistics */
		if (*mmu_report_file_name)
			mmu_access_page(phy_addr, mmu_access_execute);
	}

	/* Fetch all instructions within the block up to the first predict-taken branch. */
	while ((self->fetch_neip & ~(self->inst_mod->block_size - 1)) == block)
	{
		/* If instruction caused context to suspend or finish */
		if (!X86ContextGetState(ctx, X86ContextRunning))
			break;
	
		/* If fetch queue full, stop fetching */
		if (self->fetchq_occ >= x86_fetch_queue_size)
			break;
		
		/* Insert macro-instruction into the fetch queue. Since the macro-instruction
		 * information is only available at this point, we use it to decode
		 * instruction now and insert uops into the fetch queue. However, the
		 * fetch queue occupancy is increased with the macro-instruction size. */
		uop = X86ThreadFetchInst(self, 0);
		if (!ctx->inst.size)  /* x86_isa_inst invalid - no forward progress in loop */
			break;
		if (!uop)  /* no uop was produced by this macro-instruction */
			continue;

		/* Instruction detected as branches by the BTB are checked for branch
		 * direction in the branch predictor. If they are predicted taken,
		 * stop fetching from this block and set new fetch address. */
		if (uop->flags & X86_UINST_CTRL)
		{
			target = X86ThreadLookupBTB(self, uop);
			taken = target && X86ThreadLookupBranchPred(self, uop);
			if (taken)
			{
				self->fetch_neip = target;
				uop->pred_neip = target;
				break;
			}
		}
	}
}




/*
 * Class 'X86Core'
 */

static void X86CoreFetch(X86Core *self)
{
	X86Cpu *cpu = self->cpu;
	X86Thread *thread;

	int i;

	switch (x86_cpu_fetch_kind)
	{

	case x86_cpu_fetch_kind_shared:
	{
		/* Fetch from all threads */
		for (i = 0; i < x86_cpu_num_threads; i++)
			if (X86ThreadCanFetch(self->threads[i]))
				X86ThreadFetch(self->threads[i]);
		break;
	}

	case x86_cpu_fetch_kind_timeslice:
	{
		/* Round-robin fetch */
		for (i = 0; i < x86_cpu_num_threads; i++)
		{
			self->fetch_current = (self->fetch_current + 1) % x86_cpu_num_threads;
			thread = self->threads[self->fetch_current];
			if (X86ThreadCanFetch(thread))
			{
				X86ThreadFetch(thread);
				break;
			}
		}
		break;
	}
	
	case x86_cpu_fetch_kind_switchonevent:
	{
		int must_switch;
		int must_switch_ll;
		int new_index;

		X86Thread *new_thread;

		/* If current thread is stalled, it means that we just switched to it.
		 * No fetching and no switching either. */
		thread = self->threads[self->fetch_current];
		if (thread->fetch_stall_until >= asTiming(cpu)->cycle)
			break;

		/* Switch thread if:
		 * - Quantum expired for current thread.
		 * - Long latency instruction is in progress. */
		must_switch = !X86ThreadCanFetch(thread);
		must_switch = must_switch || asTiming(cpu)->cycle - self->fetch_switch_when >
			x86_cpu_thread_quantum + x86_cpu_thread_switch_penalty;
                
                /*X86ThreadLongLatencyInEventQueue predicts the next 
                 *Thread as well and marks the same in the thread context!
                 */
                must_switch_ll = X86ThreadLongLatencyInEventQueue(thread);  
		must_switch = must_switch || must_switch_ll;

		/* Switch thread */
		if (must_switch)
		{
			/* Find a new thread to switch to */
			for (new_index = (thread->id_in_core + 1) % x86_cpu_num_threads;
					new_index != thread->id_in_core;
					new_index = (new_index + 1) % x86_cpu_num_threads)
			{
				/* Do not choose it if it is not eligible for fetching */
				new_thread = self->threads[new_index];
				if (!X86ThreadCanFetch(new_thread))
					continue;
					
				/* Choose it if we need to switch */
				if (must_switch)
					break;

				/* Do not choose it if it is unfair */
				if (new_thread->num_committed_uinst_array >
						thread->num_committed_uinst_array + 100000)
					continue;

				/* Choose it if it is not stalled */
				if (!X86ThreadLongLatencyInEventQueue(new_thread))
					break;
			}
				
			/* Thread switch successful? */
			if (new_index != thread->id_in_core)
			{
				if (x86_tracing())
				{
					x86_trace("Thread is switched: \n");
				}
				self->fetch_current = new_index;
				self->fetch_switch_when = asTiming(cpu)->cycle;
				new_thread->fetch_stall_until = asTiming(cpu)->cycle +
						x86_cpu_thread_switch_penalty - 1;
			}
		}

		/* Fetch */
		thread = self->threads[self->fetch_current];
		if (X86ThreadCanFetch(thread))
			X86ThreadFetch(thread);
		break;
	}

	default:
		
		panic("%s: wrong fetch policy", __FUNCTION__);
	}
}




/*
 * Class 'X86Cpu'
 */

void X86CpuFetch(X86Cpu *self)
{
	int i;

	self->stage = "fetch";
	for (i = 0; i < x86_cpu_num_cores; i++)
		X86CoreFetch(self->cores[i]);
}
