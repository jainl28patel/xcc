#include "../../config.h"
#include "regalloc.h"

#include <assert.h>
#include <limits.h>  // CHAR_BIT
#include <stdlib.h>  // free
#include <string.h>

#include "ir.h"
#include "util.h"

// Register allocator

RegAlloc *new_reg_alloc(const int *reg_param_mapping, int phys_max, int temporary_count) {
  RegAlloc *ra = malloc_or_die(sizeof(*ra));
  assert(phys_max < (int)(sizeof(ra->used_reg_bits) * CHAR_BIT));
  ra->vregs = new_vector();
  ra->intervals = NULL;
  ra->sorted_intervals = NULL;
  ra->reg_param_mapping = reg_param_mapping;
  ra->phys_max = phys_max;
  ra->phys_temporary_count = temporary_count;
  ra->fphys_max = 0;
  ra->fphys_temporary_count = 0;
  ra->used_reg_bits = 0;
  ra->used_freg_bits = 0;
  return ra;
}

VReg *reg_alloc_spawn(RegAlloc *ra, const VRegType *vtype, int flag) {
  int vreg_no = ra->vregs->len;

  VReg *vreg = malloc_or_die(sizeof(*vreg));
  vreg->virt = vreg_no;
  vreg->phys = -1;
  vreg->fixnum = 0;
  vreg->vtype = vtype;
  vreg->flag = flag;
  vreg->reg_param_index = -1;
  vreg->frame.offset = 0;

  vec_push(ra->vregs, vreg);
  return vreg;
}

static int insert_active(LiveInterval **active, int active_count, LiveInterval *li) {
  int j;
  for (j = 0; j < active_count; ++j) {
    LiveInterval *p = active[j];
    if (li->end < p->end)
      break;
  }
  if (j < active_count)
    memmove(&active[j + 1], &active[j], sizeof(LiveInterval*) * (active_count - j));
  active[j] = li;
  return j;
}

static void remove_active(LiveInterval **active, int active_count, int start, int n) {
  if (n <= 0)
    return;
  int tail = active_count - (start + n);
  assert(tail >= 0);

  if (tail > 0)
    memmove(&active[start], &active[start + n], sizeof(LiveInterval*) * tail);
}

static int sort_live_interval(const void *pa, const void *pb) {
  LiveInterval *a = *(LiveInterval**)pa, *b = *(LiveInterval**)pb;
  int d = a->start - b->start;
  if (d == 0)
    d = b->end - a->end;
  return d;
}

static void split_at_interval(RegAlloc *ra, LiveInterval **active, int active_count,
                              LiveInterval *li) {
  assert(active_count > 0);
  LiveInterval *spill = active[active_count - 1];
  if (spill->end > li->end) {
    li->phys = spill->phys;
    spill->phys = ra->phys_max;
    spill->state = LI_SPILL;
    insert_active(active, active_count - 1, li);
  } else {
    li->phys = ra->phys_max;
    li->state = LI_SPILL;
  }
}

typedef struct {
  LiveInterval **active;
  int phys_max;
  int phys_temporary;
  int active_count;
  unsigned long using_bits;
  unsigned long used_bits;
} PhysicalRegisterSet;

static void expire_old_intervals(PhysicalRegisterSet *p, int start) {
  int active_count = p->active_count;
  unsigned long using_bits = p->using_bits;
  int j;
  for (j = 0; j < active_count; ++j) {
    LiveInterval *li = p->active[j];
    if (li->end > start)
      break;
    int phys = li->phys;
    using_bits &= ~(1UL << phys);
  }
  remove_active(p->active, active_count, 0, j);
  p->active_count = active_count - j;
  p->using_bits = using_bits;
}

static void set_inout_interval(Vector *vregs, LiveInterval *intervals, int nip) {
  for (int j = 0; j < vregs->len; ++j) {
    VReg *vreg = vregs->data[j];
    LiveInterval *li = &intervals[vreg->virt];
    if (vreg->flag & VRF_PARAM) {
      // If the vreg is register parameter for function,
      // it is given a priori and keep live interval start as is.
    } else {
      if (li->start < 0 || li->start > nip)
        li->start = nip;
    }
    if (li->end < nip)
      li->end = nip;
  }
}

static void check_live_interval(BBContainer *bbcon, int vreg_count, LiveInterval *intervals) {
  for (int i = 0; i < vreg_count; ++i) {
    LiveInterval *li = &intervals[i];
    li->occupied_reg_bit = 0;
    li->state = LI_NORMAL;
    li->start = li->end = -1;
    li->virt = i;
    li->phys = -1;
  }

  int nip = 0;
  for (int i = 0; i < bbcon->bbs->len; ++i) {
    BB *bb = bbcon->bbs->data[i];

    set_inout_interval(bb->in_regs, intervals, nip);

    for (int j = 0; j < bb->irs->len; ++j, ++nip) {
      IR *ir = bb->irs->data[j];
      VReg *vregs[] = {ir->dst, ir->opr1, ir->opr2};
      for (int k = 0; k < 3; ++k) {
        VReg *vreg = vregs[k];
        if (vreg == NULL)
          continue;
        LiveInterval *li = &intervals[vreg->virt];
        if (li->start < 0 && !(vreg->flag & VRF_PARAM))
          li->start = nip;
        if (li->end < nip)
          li->end = nip;
      }
    }

    set_inout_interval(bb->out_regs, intervals, nip);
  }
}

void occupy_regs(RegAlloc *ra, Vector *actives, unsigned long ioccupy, unsigned long foccupy) {
  for (int k = 0; k < actives->len; ++k) {
    LiveInterval *li = actives->data[k];
    VReg *vreg = ra->vregs->data[li->virt];
    assert(vreg != NULL);
    li->occupied_reg_bit |= (vreg->vtype->flag & VRTF_FLONUM) ? foccupy : ioccupy;
  }
}

static void detect_live_interval_flags(RegAlloc *ra, BBContainer *bbcon, int vreg_count,
                                       LiveInterval **sorted_intervals) {
  Vector *inactives = new_vector();
  Vector *actives = new_vector();
  for (int i = 0; i < vreg_count; ++i) {
    LiveInterval *li = sorted_intervals[i];
    vec_push(li->start < 0 ? actives : inactives, li);
  }

  int nip = 0;
  unsigned long iargset = 0, fargset = 0;
  for (int i = 0; i < bbcon->bbs->len; ++i) {
    BB *bb = bbcon->bbs->data[i];
    for (int j = 0; j < bb->irs->len; ++j, ++nip) {
      IR *ir = bb->irs->data[j];
      if (ir->kind == IR_PUSHARG) {
        VReg *opr1 = ir->opr1;
        if (opr1->vtype->flag & VRTF_FLONUM) {
          int n = ir->pusharg.index;
          // Assume same order on FP-register.
          fargset |= 1UL << n;
        } else {
          int n = ra->reg_param_mapping[ir->pusharg.index];
          if (n >= 0)
            iargset |= 1UL << n;
        }
      }
      if (iargset != 0 || fargset != 0)
        occupy_regs(ra, actives, iargset, fargset);

      // Deactivate registers which end at this ip.
      for (int k = 0; k < actives->len; ++k) {
        LiveInterval *li = actives->data[k];
        if (li->end <= nip)
          vec_remove_at(actives, k--);
      }

      // Call instruction breaks registers which contain in their live interval (start < nip < end).
      if (ir->kind == IR_CALL) {
        // Non-saved registers on calling convention.
        const unsigned long ibroken = (1UL << ra->phys_temporary_count) - 1;
        const unsigned long fbroken = (1UL << ra->fphys_temporary_count) - 1;
        occupy_regs(ra, actives, ibroken, fbroken);
        iargset = fargset = 0;
      }

      // Activate registers after usage checked.
      while (inactives->len > 0) {
        LiveInterval *li = inactives->data[0];
        if (li->start > nip)
          break;
        vec_remove_at(inactives, 0);
        vec_push(actives, li);
      }
    }
  }

  free_vector(inactives);
  free_vector(actives);
}

static void linear_scan_register_allocation(RegAlloc *ra, LiveInterval **sorted_intervals,
                                            int vreg_count) {
  PhysicalRegisterSet iregset = {
    .active = ALLOCA(sizeof(LiveInterval*) * ra->phys_max),
    .phys_max = ra->phys_max,
    .phys_temporary = ra->phys_temporary_count,
    .active_count = 0,
    .using_bits = 0,
    .used_bits = 0,
  };
  PhysicalRegisterSet fregset = {
    .active = ALLOCA(sizeof(LiveInterval*) * ra->fphys_max),
    .phys_max = ra->fphys_max,
    .phys_temporary = ra->fphys_temporary_count,
    .active_count = 0,
    .using_bits = 0,
    .used_bits = 0,
  };

  for (int i = 0; i < vreg_count; ++i) {
    LiveInterval *li = sorted_intervals[i];
    if (ra->vregs->data[li->virt] == NULL)
      continue;
    if (li->state != LI_NORMAL)
      continue;
    expire_old_intervals(&iregset, li->start);
    expire_old_intervals(&fregset, li->start);

    PhysicalRegisterSet *prsp = &iregset;
    if (((VReg*)ra->vregs->data[li->virt])->vtype->flag & VRTF_FLONUM)
      prsp = &fregset;
    int start_index = 0;
    int regno = -1;
    VReg *vreg = ra->vregs->data[li->virt];
    int ip = vreg->reg_param_index;
    unsigned long occupied = prsp->using_bits | li->occupied_reg_bit;
    if (ip >= 0) {
      if (vreg->vtype->flag & VRTF_FLONUM) {
        // Assume floating-pointer parameter registers are same order,
        // and no mapping required.
      } else {
        ip = ra->reg_param_mapping[ip];
      }

      if (ip >= 0 && !(occupied & (1UL << ip)))
        regno = ip;
      else
        start_index = prsp->phys_temporary;
    }
    if (regno < 0) {
      for (int j = start_index; j < prsp->phys_max; ++j) {
        if (!(occupied & (1UL << j))) {
          regno = j;
          break;
        }
      }
    }
    if (regno >= 0) {
      li->phys = regno;
      prsp->using_bits |= 1UL << regno;

      insert_active(prsp->active, prsp->active_count, li);
      ++prsp->active_count;
    } else {
      split_at_interval(ra, prsp->active, prsp->active_count, li);
    }
    prsp->used_bits |= prsp->using_bits;
  }
  ra->used_reg_bits = iregset.used_bits;
  ra->used_freg_bits = fregset.used_bits;
}

static int insert_tmp_reg(RegAlloc *ra, Vector *irs, int j, VReg *spilled) {
  VReg *tmp = reg_alloc_spawn(ra, spilled->vtype, VRF_NO_SPILL);
  IR *ir = irs->data[j];
  VReg *opr = ir->opr1 == spilled ? ir->opr1 : ir->opr2 == spilled ? ir->opr2 : NULL;
  if (opr != NULL) {
    vec_insert(irs, j++, new_ir_load_spilled(tmp, opr));
    if (ir->opr1 == spilled)
      ir->opr1 = tmp;
    if (ir->opr2 == spilled)
      ir->opr2 = tmp;
  }
  if (ir->dst == spilled) {
    vec_insert(irs, ++j, new_ir_store_spilled(ir->dst, tmp));
    ir->dst = tmp;
  }
  return j;
}

static int insert_load_store_spilled_irs(RegAlloc *ra, BBContainer *bbcon) {
  int inserted = 0;
  for (int i = 0; i < bbcon->bbs->len; ++i) {
    BB *bb = bbcon->bbs->data[i];
    Vector *irs = bb->irs;
    for (int j = 0; j < irs->len; ++j) {
      IR *ir = irs->data[j];

      int flag = 7;
      switch (ir->kind) {
      default:
        assert(false);
        // Fallthrough.
      case IR_LOAD:
      case IR_STORE:
      case IR_MOV:
      case IR_ADD:  // binops
      case IR_SUB:
      case IR_MUL:
      case IR_DIV:
      case IR_MOD:
      case IR_BITAND:
      case IR_BITOR:
      case IR_BITXOR:
      case IR_LSHIFT:
      case IR_RSHIFT:
      case IR_CMP:
      case IR_NEG:  // unary ops
      case IR_BITNOT:
      case IR_COND:
      case IR_JMP:
      case IR_TJMP:
      case IR_PUSHARG:
      case IR_CALL:
      case IR_RESULT:
      case IR_PRECALL:
      case IR_ASM:
        break;

      case IR_SUBSP:
      case IR_CAST:
        flag = 5;
        break;

      case IR_BOFS:
      case IR_IOFS:
      case IR_SOFS:
        flag = 4;
        break;

      case IR_LOAD_SPILLED:
      case IR_STORE_SPILLED:
        continue;
      }

      if (ir->opr1 != NULL && (flag & 1) != 0 &&
          !(ir->opr1->flag & VRF_CONST) && (ir->opr1->flag & VRF_SPILLED)) {
        j = insert_tmp_reg(ra, irs, j, ir->opr1);
        ++inserted;
      }

      if (ir->opr2 != NULL && (flag & 2) != 0 &&
          !(ir->opr2->flag & VRF_CONST) && (ir->opr2->flag & VRF_SPILLED)) {
        j = insert_tmp_reg(ra, irs, j, ir->opr2);
        ++inserted;
      }

      if (ir->dst != NULL && (flag & 4) != 0 &&
          !(ir->dst->flag & VRF_CONST) && (ir->dst->flag & VRF_SPILLED)) {
        j = insert_tmp_reg(ra, irs, j, ir->dst);
        ++inserted;
      }
    }
  }
  return inserted;
}

void alloc_physical_registers(RegAlloc *ra, BBContainer *bbcon) {
  assert(ra->phys_max < (int)(sizeof(ra->used_reg_bits) * CHAR_BIT));
  assert(ra->fphys_max < (int)(sizeof(ra->used_freg_bits) * CHAR_BIT));

  int vreg_count = ra->vregs->len;
  LiveInterval *intervals = malloc_or_die(sizeof(LiveInterval) * vreg_count);
  LiveInterval **sorted_intervals = malloc_or_die(sizeof(LiveInterval*) * vreg_count);

  for (;;) {
    check_live_interval(bbcon, vreg_count, intervals);

    for (int i = 0; i < vreg_count; ++i) {
      LiveInterval *li = &intervals[i];
      VReg *vreg = ra->vregs->data[i];
      if (vreg == NULL)
        continue;

      if (vreg->flag & VRF_CONST) {
        li->state = LI_CONST;
        continue;
      }

      if (vreg->flag & VRF_SPILLED) {
        li->state = LI_SPILL;
        li->phys = vreg->phys;
      }
    }

    // Sort by start, end
    for (int i = 0; i < vreg_count; ++i)
      sorted_intervals[i] = &intervals[i];
    qsort(sorted_intervals, vreg_count, sizeof(LiveInterval*), sort_live_interval);
    ra->sorted_intervals = sorted_intervals;

    detect_live_interval_flags(ra, bbcon, vreg_count, sorted_intervals);
    linear_scan_register_allocation(ra, sorted_intervals, vreg_count);

    // Spill vregs.
    for (int i = 0; i < vreg_count; ++i) {
      LiveInterval *li = &intervals[i];
      if (li->state == LI_SPILL) {
        VReg *vreg = ra->vregs->data[i];
        if (vreg->flag & VRF_SPILLED)
          continue;
        spill_vreg(vreg);
      }
    }

    if (insert_load_store_spilled_irs(ra, bbcon) <= 0)
      break;

    if (vreg_count != ra->vregs->len) {
      vreg_count = ra->vregs->len;
      free(intervals);
      free(sorted_intervals);
      intervals = malloc_or_die(sizeof(LiveInterval) * vreg_count);
      sorted_intervals = malloc_or_die(sizeof(LiveInterval*) * vreg_count);
    }
  }

  ra->intervals = intervals;
  ra->sorted_intervals = sorted_intervals;
}
