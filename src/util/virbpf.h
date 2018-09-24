/*
 * virbpf.h: methods for eBPF
 *
 * Copyright (C) 2018 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef __VIR_BPF_H__
# define __VIR_BPF_H__

# include <linux/bpf.h>

/* ALU ops on registers, bpf_add|sub|...: dst_reg += src_reg */

# define VIR_BPF_ALU64_REG(op, dst, src) \
    ((struct bpf_insn) { \
     .code = BPF_ALU64 | BPF_OP(op) | BPF_X, \
     .dst_reg = dst, \
     .src_reg = src, \
     .off = 0, \
     .imm = 0, \
     })

/* ALU ops on immediates, bpf_add|sub|...: dst_reg += imm32 */

# define VIR_BPF_ALU64_IMM(op, dst, immval) \
    ((struct bpf_insn) { \
     .code = BPF_ALU64 | BPF_OP(op) | BPF_K, \
     .dst_reg = dst, \
     .src_reg = 0, \
     .off = 0, \
     .imm = immval, \
     })

/* Short form of mov, dst_reg = src_reg */

# define VIR_BPF_MOV64_REG(dst, src) \
    ((struct bpf_insn) { \
     .code = BPF_ALU64 | BPF_MOV | BPF_X, \
     .dst_reg = dst, \
     .src_reg = src, \
     .off = 0, \
     .imm = 0, \
     })

/* Short form of mov, dst_reg = imm32 */

# define VIR_BPF_MOV64_IMM(dst, immval) \
    ((struct bpf_insn) { \
     .code = BPF_ALU64 | BPF_MOV | BPF_K, \
     .dst_reg = dst, \
     .src_reg = 0, \
     .off = 0, \
     .imm = immval, \
     })

# define VIR_BPF_MOV32_IMM(dst, immval) \
    ((struct bpf_insn) { \
     .code = BPF_ALU | BPF_MOV | BPF_K, \
     .dst_reg = dst, \
     .src_reg = 0, \
     .off = 0, \
     .imm = immval, \
     })

/* BPF_LD_IMM64 macro encodes single 'load 64-bit immediate' insn */
# define VIR_BPF_LD_IMM64(dst, imm) \
    BPF_LD_IMM64_RAW(dst, 0, imm)

# define VIR_BPF_LD_IMM64_RAW(dst, src, immval) \
    ((struct bpf_insn) { \
     .code = BPF_LD | BPF_DW | BPF_IMM, \
     .dst_reg = dst, \
     .src_reg = src, \
     .off = 0, \
     .imm = (__u32)immval, \
     }), \
    ((struct bpf_insn) { \
     .code = 0, \
     .dst_reg = 0, \
     .src_reg = 0, \
     .off = 0, \
     .imm = ((__u64)immval) >> 32, \
     })

# ifndef VIR_BPF_PSEUDO_MAP_FD
#  define VIR_BPF_PSEUDO_MAP_FD	1
# endif

/* pseudo VIR_BPF_LD_IMM64 insn used to refer to process-local map_fd */
# define VIR_BPF_LD_MAP_FD(dst, mapfd) \
    VIR_BPF_LD_IMM64_RAW(dst, VIR_BPF_PSEUDO_MAP_FD, mapfd)

/* Memory load, dst_reg = *(uint *) (src_reg + off16) */

# define VIR_BPF_LDX_MEM(size, dst, src, offval) \
    ((struct bpf_insn) { \
     .code = BPF_LDX | BPF_SIZE(size) | BPF_MEM, \
     .dst_reg = dst, \
     .src_reg = src, \
     .off = offval, \
     .imm = 0, \
     })

/* Memory store, *(uint *) (dst_reg + off16) = src_reg */

# define VIR_BPF_STX_MEM(size, dst, src, offval) \
    ((struct bpf_insn) { \
     .code = BPF_STX | BPF_SIZE(size) | BPF_MEM, \
     .dst_reg = dst, \
     .src_reg = src, \
     .off = offval, \
     .imm = 0, \
     })

/* Conditional jumps against registers, if (dst_reg 'op' src_reg) goto pc + off16 */

# define VIR_BPF_JMP_REG(op, dst, src, offval) \
    ((struct bpf_insn) { \
     .code = BPF_JMP | BPF_OP(op) | BPF_X, \
     .dst_reg = dst, \
     .src_reg = src, \
     .off = offval, \
     .imm = 0, \
     })

/* Conditional jumps against immediates, if (dst_reg 'op' imm32) goto pc + off16 */

# define VIR_BPF_JMP_IMM(op, dst, immval, offval) \
    ((struct bpf_insn) { \
     .code = BPF_JMP | BPF_OP(op) | BPF_K, \
     .dst_reg = dst, \
     .src_reg = 0, \
     .off = offval, \
     .imm = immval, \
     })

/* Call eBPF function */

# define VIR_BPF_CALL_INSN(func) \
    ((struct bpf_insn) { \
     .code = BPF_JMP | BPF_CALL, \
     .dst_reg = 0, \
     .src_reg = 0, \
     .off = 0, \
     .imm = func, \
     })

/* Program exit */

# define VIR_BPF_EXIT_INSN() \
    ((struct bpf_insn) { \
     .code = BPF_JMP | BPF_EXIT, \
     .dst_reg = 0, \
     .src_reg = 0, \
     .off = 0, \
     .imm = 0, \
     })

int
virBPFCreateMap(unsigned int mapType,
                unsigned int keySize,
                unsigned int valSize,
                unsigned int maxEntries);

int
virBPFLoadProg(struct bpf_insn *insns,
               int progType,
               unsigned int insnCnt);

int
virBPFAttachProg(int progfd,
                 int targetfd,
                 int attachType);

int
virBPFQueryProg(int targetfd,
                unsigned int maxprogids,
                int attachType,
                unsigned int *progcnt,
                void *progids);

int
virBPFGetProg(unsigned int id);

int
virBPFGetProgInfo(int progfd,
                  struct bpf_prog_info *info,
                  unsigned int **mapIDs);

int
virBPFGetMap(unsigned int id);

int
virBPFLookupElem(int mapfd,
                 void *key,
                 void *val);

int
virBPFGetNextElem(int mapfd,
                  void *key);

int
virBPFUpdateElem(int mapfd,
                 void *key,
                 void *val);

int
virBPFDeleteElem(int mapfd,
                 void *key);

#endif /* __VIR_BPF_H__ */
