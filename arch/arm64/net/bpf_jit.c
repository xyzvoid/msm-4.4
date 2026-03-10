// SPDX-License-Identifier: GPL-2.0
/* ARM64 eBPF JIT compiler - msm-4.4 backport
 * Copyright (C) 2014-2016 Zi Shen Lim <zlim.lnx@gmail.com> */
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <asm/byteorder.h>
#include <asm/cacheflush.h>
#include <asm/debug-monitors.h>
#include <asm/insn.h>
#include <asm/set_memory.h>

/* ARM64 instruction helpers */
#define AARCH64_INSN_IMM_MOVNZ  AARCH64_INSN_IMM_MOVKZ
#define A64_R(x)  AARCH64_INSN_REG_##x
#define A64_LR    AARCH64_INSN_REG_LR

#define A64_NOP      aarch64_insn_gen_nop()
#define A64_ADD_I(d,n,imm12) 	aarch64_insn_gen_add_sub_imm(d, n, imm12, 		AARCH64_INSN_VARIANT_64BIT, AARCH64_INSN_ADSB_ADD)
#define A64_SUB_I(d,n,imm12) 	aarch64_insn_gen_add_sub_imm(d, n, imm12, 		AARCH64_INSN_VARIANT_64BIT, AARCH64_INSN_ADSB_SUB)
#define A64_MOV(d,n) 	aarch64_insn_gen_move_reg(d, n, AARCH64_INSN_VARIANT_64BIT)
#define A64_RET  aarch64_insn_gen_ret(AARCH64_INSN_REG_LR)

/* BPF register mapping to arm64 registers */
static const int bpf2a64[] = {
	/* r0  = return value */   [BPF_REG_0]  = A64_R(20),
	/* r1  = 1st arg */        [BPF_REG_1]  = A64_R(0),
	/* r2  = 2nd arg */        [BPF_REG_2]  = A64_R(1),
	/* r3  = 3rd arg */        [BPF_REG_3]  = A64_R(2),
	/* r4  = 4th arg */        [BPF_REG_4]  = A64_R(3),
	/* r5  = 5th arg */        [BPF_REG_5]  = A64_R(4),
	/* r6  = callee saved */   [BPF_REG_6]  = A64_R(19),
	/* r7  = callee saved */   [BPF_REG_7]  = A64_R(21),
	/* r8  = callee saved */   [BPF_REG_8]  = A64_R(22),
	/* r9  = callee saved */   [BPF_REG_9]  = A64_R(23),
	/* r10 = frame pointer */  [BPF_REG_10] = A64_R(25),
	/* ephemeral regs */       [TMP_REG_1]  = A64_R(10),
	                           [TMP_REG_2]  = A64_R(11),
};

#define TMP_REG_1 (MAX_BPF_JIT_REG + 0)
#define TMP_REG_2 (MAX_BPF_JIT_REG + 1)
#define TCALL_CNT (MAX_BPF_JIT_REG + 2)
#define TMP_REG_3 (MAX_BPF_JIT_REG + 3)

struct jit_ctx {
	const struct bpf_prog *prog;
	int idx, epilogue_offset;
	int *offset;
	__le32 *image;
	u32 stack_size;
};

static inline void emit(const u32 insn, struct jit_ctx *ctx)
{
	if (ctx->image != NULL)
		ctx->image[ctx->idx] = cpu_to_le32(insn);
	ctx->idx++;
}

static void build_prologue(struct jit_ctx *ctx, bool was_classic)
{
	const int r6 = bpf2a64[BPF_REG_6];
	const int r7 = bpf2a64[BPF_REG_7];
	const int r8 = bpf2a64[BPF_REG_8];
	const int r9 = bpf2a64[BPF_REG_9];
	const int fp = bpf2a64[BPF_REG_10];
	const int tcc = bpf2a64[TCALL_CNT];

	/* Save callee-saved registers and build stack frame */
	emit(aarch64_insn_gen_load_store_pair(r6, r7, A64_SP, -16,
					      AARCH64_INSN_VARIANT_64BIT,
					      AARCH64_INSN_LDST_STORE_PAIR_PRE_INDEX), ctx);
	emit(aarch64_insn_gen_load_store_pair(r8, r9, A64_SP, 16,
					      AARCH64_INSN_VARIANT_64BIT,
					      AARCH64_INSN_LDST_STORE_PAIR_OFFSET), ctx);
	emit(aarch64_insn_gen_load_store_pair(fp, tcc, A64_SP, 32,
					      AARCH64_INSN_VARIANT_64BIT,
					      AARCH64_INSN_LDST_STORE_PAIR_OFFSET), ctx);

	/* Point FP to top of BPF stack */
	emit(A64_ADD_I(fp, A64_SP, STACK_SIZE), ctx);
	/* Zero out tail call counter */
	emit(aarch64_insn_gen_logical_immediate(AARCH64_INSN_LOGIC_AND, A64_VARIANT_64, tcc, tcc, 0), ctx);
}

static void build_epilogue(struct jit_ctx *ctx)
{
	const int r0 = bpf2a64[BPF_REG_0];
	const int r6 = bpf2a64[BPF_REG_6];
	const int r7 = bpf2a64[BPF_REG_7];
	const int r8 = bpf2a64[BPF_REG_8];
	const int r9 = bpf2a64[BPF_REG_9];
	const int fp = bpf2a64[BPF_REG_10];
	const int tcc = bpf2a64[TCALL_CNT];

	/* Move return value to x0 */
	emit(A64_MOV(A64_R(0), r0), ctx);

	/* Restore callee-saved registers */
	emit(aarch64_insn_gen_load_store_pair(r6, r7, A64_SP, 16,
					      AARCH64_INSN_VARIANT_64BIT,
					      AARCH64_INSN_LDST_LOAD_PAIR_OFFSET), ctx);
	emit(aarch64_insn_gen_load_store_pair(r8, r9, A64_SP, 32,
					      AARCH64_INSN_VARIANT_64BIT,
					      AARCH64_INSN_LDST_LOAD_PAIR_OFFSET), ctx);
	emit(aarch64_insn_gen_load_store_pair(fp, tcc, A64_SP, 48,
					      AARCH64_INSN_VARIANT_64BIT,
					      AARCH64_INSN_LDST_LOAD_PAIR_POST_INDEX), ctx);
	emit(A64_RET, ctx);
}

static int build_insn(const struct bpf_insn *insn, struct jit_ctx *ctx,
		      bool extra_pass)
{
	const u8 code = insn->code;
	const u8 dst  = insn->dst_reg;
	const u8 src  = insn->src_reg;
	const s16 off = insn->off;
	const s32 imm = insn->imm;
	const int i = insn - ctx->prog->insnsi;
	const bool is64 = BPF_CLASS(code) == BPF_ALU64;
	const int a64_dst = bpf2a64[dst];
	const int a64_src = bpf2a64[src];
	int jmp_offset;

	switch (code) {
	/* dst = dst OP src */
	case BPF_ALU | BPF_ADD | BPF_X:
	case BPF_ALU64 | BPF_ADD | BPF_X:
		emit(aarch64_insn_gen_add_sub_shifted_reg(a64_dst, a64_dst, a64_src, 0,
			is64 ? AARCH64_INSN_VARIANT_64BIT : AARCH64_INSN_VARIANT_32BIT,
			AARCH64_INSN_ADSB_ADD), ctx);
		break;
	case BPF_ALU | BPF_SUB | BPF_X:
	case BPF_ALU64 | BPF_SUB | BPF_X:
		emit(aarch64_insn_gen_add_sub_shifted_reg(a64_dst, a64_dst, a64_src, 0,
			is64 ? AARCH64_INSN_VARIANT_64BIT : AARCH64_INSN_VARIANT_32BIT,
			AARCH64_INSN_ADSB_SUB), ctx);
		break;
	case BPF_ALU64 | BPF_MOV | BPF_X:
		emit(A64_MOV(a64_dst, a64_src), ctx);
		break;
	case BPF_ALU64 | BPF_MOV | BPF_K:
		emit(aarch64_insn_gen_movewide(a64_dst, (u16)imm, 0,
			AARCH64_INSN_VARIANT_64BIT, AARCH64_INSN_MOVEWIDE_ZERO), ctx);
		break;
	case BPF_JMP | BPF_EXIT:
		if (i != ctx->prog->len - 1) {
			jmp_offset = ctx->epilogue_offset - (i + 1);
			emit(aarch64_insn_gen_branch_imm(0, jmp_offset << 2,
				AARCH64_INSN_BRANCH_NOLINK), ctx);
		}
		break;
	default:
		/* Emit NOP for unhandled; extend switch for full coverage */
		emit(A64_NOP, ctx);
	}
	return 0;
}

static int validate_code(struct jit_ctx *ctx)
{
	int i;
	for (i = 0; i < ctx->idx; i++)
		if (ctx->image[i] == AARCH64_BREAK_FAULT)
			return -1;
	return 0;
}

/* Main JIT compilation entry */
struct bpf_prog *bpf_int_jit_compile(struct bpf_prog *prog)
{
	struct bpf_prog *tmp, *orig_prog = prog;
	struct bpf_binary_header *header;
	struct jit_ctx ctx = { .prog = prog };
	unsigned int image_size;
	int pass, i;

	if (!prog->jit_requested)
		return orig_prog;

	tmp = bpf_jit_blind_constants(prog);
	if (IS_ERR(tmp))
		return orig_prog;
	if (tmp != prog) prog = tmp;

	ctx.offset = kcalloc(prog->len + 1, sizeof(int), GFP_KERNEL);
	if (!ctx.offset) { prog = orig_prog; goto out; }

	/* Two passes: dry-run to count, then emit */
	for (pass = 0; pass < 3; pass++) {
		ctx.idx = 0;
		build_prologue(&ctx, false);
		for (i = 0; i < prog->len; i++) {
			ctx.offset[i] = ctx.idx;
			if (build_insn(prog->insnsi + i, &ctx, pass == 2)) {
				prog = orig_prog; goto out_off;
			}
		}
		ctx.epilogue_offset = ctx.idx;
		build_epilogue(&ctx);
		ctx.offset[i] = ctx.idx;
		if (pass == 1) {
			image_size = sizeof(u32) * ctx.idx;
			header = bpf_jit_binary_alloc(image_size, (u8 **)&ctx.image,
						     sizeof(u32), jit_fill_hole);
			if (!header) { prog = orig_prog; goto out_off; }
		}
	}

	if (bpf_jit_enable > 1)
		bpf_jit_dump(prog->len, image_size, pass, ctx.image);

	if (validate_code(&ctx)) {
		bpf_jit_binary_free(header);
		prog = orig_prog; goto out_off;
	}

	bpf_flush_icache(header, ctx.image + ctx.idx);
	if (!bpf_jit_binary_lock_ro(header)) {
		prog->bpf_func = (void *)ctx.image;
		prog->jited    = 1;
		prog->jited_len = image_size;
	} else {
		bpf_jit_binary_free(header);
		prog = orig_prog;
	}

out_off:
	kfree(ctx.offset);
out:
	if (prog != orig_prog) {
		bpf_jit_prog_release_other(prog, prog == orig_prog ? tmp : orig_prog);
	}
	return prog;
}
