// SPDX-License-Identifier: GPL-2.0
/*
 * arch/arm64/kvm/fpsimd.c: Guest/host FPSIMD context coordination helpers
 *
 * Copyright 2018 Arm Limited
 * Author: Dave Martin <Dave.Martin@arm.com>
 */
#include <linux/irqflags.h>
#include <linux/sched.h>
#include <linux/kvm_host.h>
#include <asm/fpsimd.h>
#include <asm/kvm_asm.h>
#include <asm/kvm_hyp.h>
#include <asm/kvm_mmu.h>
#include <asm/sysreg.h>

/*
 * Called on entry to KVM_RUN unless this vcpu previously ran at least
 * once and the most recent prior KVM_RUN for this vcpu was called from
 * the same task as current (highly likely).
 *
 * This is guaranteed to execute before kvm_arch_vcpu_load_fp(vcpu),
 * such that on entering hyp the relevant parts of current are already
 * mapped.
 */
int kvm_arch_vcpu_run_map_fp(struct kvm_vcpu *vcpu)
{
	struct user_fpsimd_state *fpsimd = &current->thread.uw.fpsimd_state;
	int ret;

	/* pKVM has its own tracking of the host fpsimd state. */
	if (is_protected_kvm_enabled())
		return 0;

	/* Make sure the host task fpsimd state is visible to hyp: */
	ret = kvm_share_hyp(fpsimd, fpsimd + 1);
	if (ret)
		return ret;

	vcpu->arch.host_fpsimd_state = kern_hyp_va(fpsimd);

	return 0;
}

/*
 * Prepare vcpu for saving the host's FPSIMD state and loading the guest's.
 * The actual loading is done by the FPSIMD access trap taken to hyp.
 *
 * Here, we just set the correct metadata to indicate that the FPSIMD
 * state in the cpu regs (if any) belongs to current on the host.
 */
void kvm_arch_vcpu_load_fp(struct kvm_vcpu *vcpu)
{
	BUG_ON(!current->mm);

	if (!system_supports_fpsimd())
		return;

	/*
	 * Ensure that any host FPSIMD/SVE/SME state is saved and unbound such
	 * that the host kernel is responsible for restoring this state upon
	 * return to userspace, and the hyp code doesn't need to save anything.
	 *
	 * When the host may use SME, fpsimd_save_and_flush_cpu_state() ensures
	 * that PSTATE.{SM,ZA} == {0,0}.
	 */
	fpsimd_save_and_flush_cpu_state();
	vcpu->arch.fp_state = FP_STATE_FREE;

	vcpu_clear_flag(vcpu, HOST_SVE_ENABLED);
	if (read_sysreg(cpacr_el1) & CPACR_EL1_ZEN_EL0EN)
		vcpu_set_flag(vcpu, HOST_SVE_ENABLED);

	if (system_supports_sme()) {
		vcpu_clear_flag(vcpu, HOST_SME_ENABLED);
		if (read_sysreg(cpacr_el1) & CPACR_EL1_SMEN_EL0EN)
			vcpu_set_flag(vcpu, HOST_SME_ENABLED);
	}

	/*
	 * If normal guests gain this support, maintain this behavior
	 * for pKVM guests, which don't support SME.
	 */
	BUG_ON(is_protected_kvm_enabled() && system_supports_sme() &&
	       read_sysreg_s(SYS_SVCR));
}

/*
 * Called just before entering the guest once we are no longer preemptable
 * and interrupts are disabled. If we have managed to run anything using
 * FP while we were preemptible (such as off the back of an interrupt),
 * then neither the host nor the guest own the FP hardware (and it was the
 * responsibility of the code that used FP to save the existing state).
 */
void kvm_arch_vcpu_ctxflush_fp(struct kvm_vcpu *vcpu)
{
	if (test_thread_flag(TIF_FOREIGN_FPSTATE))
		vcpu->arch.fp_state = FP_STATE_FREE;
}

/*
 * Called just after exiting the guest. If the guest FPSIMD state
 * was loaded, update the host's context tracking data mark the CPU
 * FPSIMD regs as dirty and belonging to vcpu so that they will be
 * written back if the kernel clobbers them due to kernel-mode NEON
 * before re-entry into the guest.
 */
void kvm_arch_vcpu_ctxsync_fp(struct kvm_vcpu *vcpu)
{
	struct cpu_fp_state fp_state;

	WARN_ON_ONCE(!irqs_disabled());

	if (vcpu->arch.fp_state == FP_STATE_GUEST_OWNED) {

		/*
		 * Currently we do not support SME guests so SVCR is
		 * always 0 and we just need a variable to point to.
		 */
		fp_state.st = &vcpu->arch.ctxt.fp_regs;
		fp_state.sve_state = vcpu->arch.sve_state;
		fp_state.sve_vl = vcpu->arch.sve_max_vl;
		fp_state.sme_state = NULL;
		fp_state.svcr = &vcpu->arch.svcr;
		fp_state.fp_type = &vcpu->arch.fp_type;

		if (vcpu_has_sve(vcpu))
			fp_state.to_save = FP_STATE_SVE;
		else
			fp_state.to_save = FP_STATE_FPSIMD;

		fpsimd_bind_state_to_cpu(&fp_state);

		clear_thread_flag(TIF_FOREIGN_FPSTATE);
	}
}

/*
 * Write back the vcpu FPSIMD regs if they are dirty, and invalidate the
 * cpu FPSIMD regs so that they can't be spuriously reused if this vcpu
 * disappears and another task or vcpu appears that recycles the same
 * struct fpsimd_state.
 */
void kvm_arch_vcpu_put_fp(struct kvm_vcpu *vcpu)
{
	unsigned long flags;

	local_irq_save(flags);

	/*
	 * If we have VHE then the Hyp code will reset CPACR_EL1 to
	 * the default value and we need to reenable SME.
	 */
	if (has_vhe() && system_supports_sme()) {
		/* Also restore EL0 state seen on entry */
		if (vcpu_get_flag(vcpu, HOST_SME_ENABLED))
			sysreg_clear_set(CPACR_EL1, 0,
					 CPACR_EL1_SMEN_EL0EN |
					 CPACR_EL1_SMEN_EL1EN);
		else
			sysreg_clear_set(CPACR_EL1,
					 CPACR_EL1_SMEN_EL0EN,
					 CPACR_EL1_SMEN_EL1EN);
		isb();
	}

	if (vcpu->arch.fp_state == FP_STATE_GUEST_OWNED) {
		if (vcpu_has_sve(vcpu)) {
			__vcpu_sys_reg(vcpu, ZCR_EL1) = read_sysreg_el1(SYS_ZCR);

			/*
			 * Restore the VL that was saved when bound to the CPU,
			 * which is the maximum VL for the guest. Because
			 * the layout of the data when saving the sve state
			 * depends on the VL, we need to use a consistent VL.
			 * Note that this means that at guest exit ZCR_EL1 is
			 * not necessarily the same as on guest entry.
			 *
			 * Flushing the cpu state sets the TIF_FOREIGN_FPSTATE
			 * bit for the context, which lets the kernel restore
			 * the sve state, including ZCR_EL1 later.
			 */
			if (!has_vhe())
				sve_cond_update_zcr_vq(vcpu_sve_max_vq(vcpu) - 1,
						       SYS_ZCR_EL1);
		}

		fpsimd_save_and_flush_cpu_state();
	} else if (has_vhe() && system_supports_sve()) {
		/*
		 * The FPSIMD/SVE state in the CPU has not been touched, and we
		 * have SVE (and VHE): CPACR_EL1 (alias CPTR_EL2) has been
		 * reset by kvm_reset_cptr_el2() in the Hyp code, disabling SVE
		 * for EL0.  To avoid spurious traps, restore the trap state
		 * seen by kvm_arch_vcpu_load_fp():
		 */
		if (vcpu_get_flag(vcpu, HOST_SVE_ENABLED))
			sysreg_clear_set(CPACR_EL1, 0, CPACR_EL1_ZEN_EL0EN);
		else
			sysreg_clear_set(CPACR_EL1, CPACR_EL1_ZEN_EL0EN, 0);
	}

	local_irq_restore(flags);
}
