#include "wcc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>  // malloc
#include <string.h>

#include "ast.h"
#include "lexer.h"  // parse_error
#include "parser.h"  // curfunc
#include "table.h"
#include "type.h"
#include "util.h"
#include "var.h"
#include "wasm_util.h"

#define ADD_CODE(...)  do { unsigned char buf[] = {__VA_ARGS__}; add_code(buf, sizeof(buf)); } while (0)
#define ADD_LEB128(x)  emit_leb128(code, code->len, x)
#define ADD_ULEB128(x) emit_uleb128(code, code->len, x)

// TODO: Endian.
#define ADD_F32(x)     do { float f = (x); add_code((unsigned char*)&f, sizeof(f)); } while (0)
#define ADD_F64(x)     do { double d = (x); add_code((unsigned char*)&d, sizeof(d)); } while (0)

DataStorage *code;

static void add_code(const unsigned char* buf, size_t size) {
  data_append(code, buf, size);
}

void emit_leb128(DataStorage *data, size_t pos, int64_t val) {
  unsigned char buf[5], *p = buf;
  const int64_t MAX = 1 << 6;
  for (;;) {
    if (val < MAX && val >= -MAX) {
      *p++ = val & 0x7f;
      data_insert(data, pos, buf, p - buf);
      return;
    }
    *p++ = (val & 0x7f) | 0x80;
    val >>= 7;
  }
}

void emit_uleb128(DataStorage *data, size_t pos, uint64_t val) {
  unsigned char buf[5], *p = buf;
  const uint64_t MAX = 1 << 7;
  for (;;) {
    if (val < MAX) {
      *p++ = val & 0x7f;
      data_insert(data, pos, buf, p - buf);
      return;
    }
    *p++ = (val & 0x7f) | 0x80;
    val >>= 7;
  }
}

////////////////////////////////////////////////

// Local variable information for WASM
struct VReg {
  uint32_t local_index;
};

static void gen_stmt(Stmt *stmt);
static void gen_stmts(Vector *stmts);
static void gen_expr_stmt(Expr *expr);
static void gen_expr(Expr *expr);

static int cur_depth;

unsigned char to_wtype(const Type *type) {
  switch (type->kind) {
  case TY_FIXNUM: return type_size(type) <= I32_SIZE ? WT_I32 : WT_I64;
#ifndef __NO_FLONUM
  case TY_FLONUM: return type->flonum.kind == FL_FLOAT ? WT_F32 : WT_F64;
#endif
  default: assert(!"Illegal"); break;
  }
  return WT_I32;
}

static void gen_arith(enum ExprKind kind, const Type *type) {
  assert(is_number(type));
  int index = 0;
#ifndef __NO_FLONUM
  if (is_flonum(type)) {
    assert(kind < EX_MOD);
    index = type->flonum.kind != FL_FLOAT ? 3 : 2;
  } else
#endif
  {
    index = type_size(type) > I32_SIZE ? 1 : 0;
  }

  // unsigned?
  static const unsigned char kOpTable[][EX_RSHIFT - EX_ADD + 1] = {
    {OP_I32_ADD, OP_I32_SUB, OP_I32_MUL, OP_I32_DIV_S, OP_I32_REM_S, OP_I32_AND, OP_I32_OR, OP_I32_XOR, OP_I32_SHL, OP_I32_SHR_S},
    {OP_I64_ADD, OP_I64_SUB, OP_I64_MUL, OP_I64_DIV_S, OP_I64_REM_S, OP_I64_AND, OP_I64_OR, OP_I64_XOR, OP_I64_SHL, OP_I64_SHR_S},
    {OP_F32_ADD, OP_F32_SUB, OP_F32_MUL, OP_F32_DIV, OP_NOP, OP_NOP, OP_NOP, OP_NOP, OP_NOP, OP_NOP},
    {OP_F64_ADD, OP_F64_SUB, OP_F64_MUL, OP_F64_DIV, OP_NOP, OP_NOP, OP_NOP, OP_NOP, OP_NOP, OP_NOP},
  };

  assert(EX_ADD <= kind && kind <= EX_RSHIFT);
  assert(kOpTable[index][kind - EX_ADD] != OP_NOP);
  ADD_CODE(kOpTable[index][kind - EX_ADD]);
}

static void gen_cast(const Type *dst, const Type *src) {
  if (dst->kind == TY_VOID) {
    ADD_CODE(OP_DROP);
    return;
  }

  switch (dst->kind) {
  case TY_FIXNUM:
    switch (src->kind) {
    case TY_FIXNUM:
      {
        int d = type_size(dst);
        int s = type_size(src);
        switch ((d > I32_SIZE ? 2 : 0) + (s > I32_SIZE ? 1 : 0)) {
        case 1: ADD_CODE(OP_I32_WRAP_I64); break;
        case 2: ADD_CODE(OP_I64_EXTEND_I32_S); break;
        }
      }
      return;
#ifndef __NO_FLONUM
    case TY_FLONUM:
      {
        static const unsigned char OpTable[] = {
          OP_I32_TRUNC_F32_S, OP_I32_TRUNC_F64_S,
          OP_I64_TRUNC_F32_S, OP_I64_TRUNC_F64_S,
        };
        int d = type_size(dst);
        int index = (d > I32_SIZE ? 2 : 0) + (src->flonum.kind != FL_FLOAT ? 1 : 0);
        ADD_CODE(OpTable[index]);
      }
      return;
#endif
    default: break;
    }
    break;
#ifndef __NO_FLONUM
  case TY_FLONUM:
    switch (src->kind) {
    case TY_FIXNUM:
      {
        static const unsigned char OpTable[] = {
          OP_F32_CONVERT_I32_S, OP_F32_CONVERT_I64_S,
          OP_F64_CONVERT_I32_S, OP_F64_CONVERT_I64_S,
        };
        int s = type_size(src);
        int index = (dst->flonum.kind != FL_FLOAT ? 2 : 0) + (s > I32_SIZE ? 1 : 0);
        ADD_CODE(OpTable[index]);
      }
      return;
    case TY_FLONUM:
      {
        assert(dst->flonum.kind != src->flonum.kind);
        switch (dst->flonum.kind) {
        case FL_FLOAT:  ADD_CODE(OP_F32_DEMOTE_F64); break;
        case FL_DOUBLE: ADD_CODE(OP_F64_PROMOTE_F32); break;
        }
      }
      return;
    default: break;
    }
    break;
#endif
  default: break;
  }
  assert(!"Cast not handled");
}

static void gen_funcall(Expr *expr) {
  Expr *func = expr->funcall.func;
  assert(func->kind == EX_VAR);
  Vector *args = expr->funcall.args;
  int arg_count = args != NULL ? args->len : 0;

  FuncInfo *info = table_get(&func_info_table, func->var.name);
  assert(info != NULL);
  uint32_t func_index = info->index;

  for (int i = 0; i < arg_count; ++i) {
    Expr *arg = args->data[i];
    gen_expr(arg);
  }
  ADD_CODE(OP_CALL);
  ADD_ULEB128(func_index);
}

static void gen_expr(Expr *expr) {
  switch (expr->kind) {
  case EX_FIXNUM:
    {
      if (type_size(expr->type) <= I32_SIZE) {
        ADD_CODE(OP_I32_CONST);
        ADD_LEB128((int32_t)expr->fixnum);
      } else {
        ADD_CODE(OP_I64_CONST);
        ADD_LEB128(expr->fixnum);
      }
    }
    break;

#ifndef __NO_FLONUM
  case EX_FLONUM:
    switch (expr->type->flonum.kind) {
    case FL_FLOAT:
      ADD_CODE(OP_F32_CONST);
      ADD_F32(expr->flonum);
      break;
    case FL_DOUBLE:
      ADD_CODE(OP_F64_CONST);
      ADD_F64(expr->flonum);
      break;
    }
    break;
#endif

  case EX_VAR:
    {
      assert(is_number(expr->type));
      Scope *scope;
      const VarInfo *varinfo = scope_find(expr->var.scope, expr->var.name, &scope);
      assert(varinfo != NULL && scope == expr->var.scope);
      if (!is_global_scope(scope) && !(varinfo->storage & (VS_STATIC | VS_EXTERN))) {
        VReg *vreg = varinfo->local.reg;
        assert(vreg != NULL);
        ADD_CODE(OP_LOCAL_GET);
        ADD_ULEB128(vreg->local_index);
      } else {
        GVarInfo *info = get_gvar_info(expr);
        assert(info != NULL);
        ADD_CODE(OP_GLOBAL_GET);
        ADD_ULEB128(info->index);
      }
    }
    break;

  case EX_ADD:
  case EX_SUB:
  case EX_MUL:
  case EX_DIV:
  case EX_MOD:
  case EX_LSHIFT:
  case EX_RSHIFT:
  case EX_BITAND:
  case EX_BITOR:
  case EX_BITXOR:
    gen_expr(expr->bop.lhs);
    gen_expr(expr->bop.rhs);
    gen_arith(expr->kind, expr->type);
    break;

  case EX_POS:
    gen_expr(expr->unary.sub);
    break;

  case EX_NEG:
    switch (expr->type->kind) {
    case TY_FIXNUM:
      ADD_CODE(type_size(expr->type) <= I32_SIZE ? OP_I32_CONST : OP_I64_CONST);
      ADD_LEB128(0);
      break;
#ifndef __NO_FLONUM
    case TY_FLONUM:
      switch (expr->type->flonum.kind) {
      case FL_FLOAT:
        ADD_CODE(OP_F32_CONST);
        ADD_F32(0);
        break;
      case FL_DOUBLE:
        ADD_CODE(OP_F64_CONST);
        ADD_F64(0);
        break;
      default: assert(false); break;
      break;
      }
      break;
#endif
    default: assert(false); break;
    }

    gen_expr(expr->unary.sub);
    gen_arith(EX_SUB, expr->type);
    break;

  case EX_BITNOT:
    {
      gen_expr(expr->unary.sub);
      unsigned char wt = to_wtype(expr->type);
      switch (wt) {
      case WT_I32:  ADD_CODE(OP_I32_CONST); break;
      case WT_I64:  ADD_CODE(OP_I64_CONST); break;
      default: assert(false); break;
      }
      ADD_LEB128(-1);
      gen_arith(EX_BITXOR, expr->type);
    }
    break;

  case EX_PREINC:
  case EX_PREDEC:
    {
      assert(is_fixnum(expr->type->kind));
      Expr *sub = expr->unary.sub;
      assert(sub->kind == EX_VAR);
      gen_expr(sub);
      ADD_CODE(OP_I32_CONST, 1,
               expr->kind == EX_PREINC ? OP_I32_ADD : OP_I32_SUB);

      Scope *scope;
      const VarInfo *varinfo = scope_find(sub->var.scope, sub->var.name, &scope);
      assert(varinfo != NULL);
      if (!is_global_scope(scope) && !(varinfo->storage & (VS_STATIC | VS_EXTERN))) {
        VReg *vreg = varinfo->local.reg;
        assert(vreg != NULL);
        ADD_CODE(OP_LOCAL_TEE);
        ADD_ULEB128(vreg->local_index);
      } else {
        GVarInfo *info = get_gvar_info(sub);
        assert(info != NULL);
        ADD_CODE(OP_GLOBAL_SET);
        ADD_ULEB128(info->index);
        ADD_CODE(OP_GLOBAL_GET);
        ADD_ULEB128(info->index);
      }
    }
    break;

  case EX_POSTINC:
  case EX_POSTDEC:
    {
      assert(is_fixnum(expr->type->kind));
      Expr *sub = expr->unary.sub;
      assert(sub->kind == EX_VAR);
      gen_expr(sub);  // Push the result first.
      gen_expr(sub);
      ADD_CODE(OP_I32_CONST, 1,
               expr->kind == EX_POSTINC ? OP_I32_ADD : OP_I32_SUB);

      Scope *scope;
      const VarInfo *varinfo = scope_find(sub->var.scope, sub->var.name, &scope);
      assert(varinfo != NULL);
      if (!is_global_scope(scope) && !(varinfo->storage & (VS_STATIC | VS_EXTERN))) {
        VReg *vreg = varinfo->local.reg;
        assert(vreg != NULL);
        ADD_CODE(OP_LOCAL_SET);
        ADD_ULEB128(vreg->local_index);
      } else {
        GVarInfo *info = get_gvar_info(sub);
        assert(info != NULL);
        ADD_CODE(OP_GLOBAL_SET);
        ADD_ULEB128(info->index);
      }
    }
    break;

  case EX_ASSIGN:
    {
      Expr *lhs = expr->bop.lhs;
      switch (lhs->type->kind) {
      case TY_FIXNUM:
      case TY_PTR:
#ifndef __NO_FLONUM
      case TY_FLONUM:
#endif
        if (expr->bop.lhs->kind == EX_VAR) {
          Scope *scope;
          const VarInfo *varinfo = scope_find(lhs->var.scope, lhs->var.name, &scope);
          assert(varinfo != NULL);
          if (!is_global_scope(scope) && !(varinfo->storage & (VS_STATIC | VS_EXTERN))) {
            VReg *vreg = varinfo->local.reg;
            assert(vreg != NULL);
            gen_expr(expr->bop.rhs);
            ADD_CODE(OP_LOCAL_TEE);
            ADD_ULEB128(vreg->local_index);
          } else {
            GVarInfo *info = get_gvar_info(lhs);
            assert(info != NULL);
            ADD_CODE(OP_GLOBAL_SET);
            ADD_ULEB128(info->index);
            ADD_CODE(OP_GLOBAL_GET);
            ADD_ULEB128(info->index);
          }
          break;
        }
        assert(!"Not implemented");
        break;
      default: assert(false); break;
      }
    }
    break;

  case EX_MODIFY:
    {
      Expr *sub = expr->unary.sub;
      Expr *lhs = sub->bop.lhs;
      switch (lhs->type->kind) {
      case TY_FIXNUM:
      case TY_PTR:
#ifndef __NO_FLONUM
      case TY_FLONUM:
#endif
        if (lhs->kind == EX_VAR) {
          gen_expr(lhs);
          gen_expr(sub->bop.rhs);
          gen_arith(sub->kind, expr->type);

          Scope *scope;
          const VarInfo *varinfo = scope_find(lhs->var.scope, lhs->var.name, &scope);
          assert(varinfo != NULL);
          if (!is_global_scope(scope) && !(varinfo->storage & (VS_STATIC | VS_EXTERN))) {
            assert(varinfo->local.reg != NULL);
            ADD_CODE(OP_LOCAL_TEE);
            ADD_ULEB128(varinfo->local.reg->local_index);
          } else {
            GVarInfo *info = get_gvar_info(lhs);
            assert(info != NULL);
            ADD_CODE(OP_GLOBAL_SET);
            ADD_ULEB128(info->index);
            ADD_CODE(OP_GLOBAL_GET);
            ADD_ULEB128(info->index);
          }
          return;
        }
        assert(!"Not implemented");
        break;
      default: assert(false); break;
      }
    }
    break;

  case EX_CAST:
    gen_expr(expr->unary.sub);
    gen_cast(expr->type, expr->unary.sub->type);
    break;

  case EX_FUNCALL:
    gen_funcall(expr);
    break;

  default: assert(!"Not implemeneted"); break;
  }
}

static void gen_compare_expr(enum ExprKind kind, Expr *lhs, Expr *rhs) {
  assert(lhs->type->kind == rhs->type->kind);
  assert(is_number(lhs->type));

  int index = 0;
#ifndef __NO_FLONUM
  if (is_flonum(lhs->type)) {
    index = lhs->type->flonum.kind != FL_FLOAT ? 3 : 2;
  } else
#endif
  {
    index = type_size(lhs->type) > I32_SIZE ? 1 : 0;
  }

  // unsigned?
  static const unsigned char OpTable[][6] = {
    {OP_I32_EQ, OP_I32_NE, OP_I32_LT_S, OP_I32_LE_S, OP_I32_GE_S, OP_I32_GT_S},
    {OP_I64_EQ, OP_I64_NE, OP_I64_LT_S, OP_I64_LE_S, OP_I64_GE_S, OP_I64_GT_S},
    {OP_F32_EQ, OP_F32_NE, OP_F32_LT, OP_F32_LE, OP_F32_GE, OP_F32_GT},
    {OP_F64_EQ, OP_F64_NE, OP_F64_LT, OP_F64_LE, OP_F64_GE, OP_F64_GT},
  };

  gen_expr(lhs);
  gen_expr(rhs);
  ADD_CODE(OpTable[index][kind - EX_EQ]);
}

static void gen_cond(Expr *cond, bool tf) {
  enum ExprKind ck = cond->kind;
  switch (ck) {
  case EX_FIXNUM:
    {
      Expr *zero = new_expr_fixlit(&tyInt, NULL, 0);
      gen_compare_expr(tf ? EX_NE : EX_EQ, cond, zero);
    }
    break;
  case EX_EQ:
  case EX_NE:
  case EX_LT:
  case EX_GT:
  case EX_LE:
  case EX_GE:
    if (!tf) {
      if (ck <= EX_NE)
        ck = (EX_EQ + EX_NE) - ck;
      else
        ck = EX_LT + ((ck - EX_LT) ^ 2);
    }
    gen_compare_expr(ck, cond->bop.lhs, cond->bop.rhs);
    break;
  case EX_LOGAND:
    if (tf) {
      gen_cond(cond->bop.lhs, true);
      ADD_CODE(OP_IF, WT_I32);
      ++cur_depth;
      gen_cond(cond->bop.rhs, true);
      ADD_CODE(OP_ELSE);
      ADD_CODE(OP_I32_CONST, 0);  // false
      ADD_CODE(OP_END);
      --cur_depth;
    } else {
      gen_cond(cond->bop.lhs, false);
      ADD_CODE(OP_IF, WT_I32);
      ++cur_depth;
      ADD_CODE(OP_I32_CONST, 1);  // true
      ADD_CODE(OP_ELSE);
      gen_cond(cond->bop.rhs, false);
      ADD_CODE(OP_END);
      --cur_depth;
    }
    break;
  case EX_LOGIOR:
    if (tf) {
      gen_cond(cond->bop.lhs, true);
      ADD_CODE(OP_IF, WT_I32);
      ++cur_depth;
      ADD_CODE(OP_I32_CONST, 1);  // true
      ADD_CODE(OP_ELSE);
      gen_cond(cond->bop.rhs, true);
      ADD_CODE(OP_END);
      --cur_depth;
    } else {
      gen_cond(cond->bop.lhs, false);
      ADD_CODE(OP_IF, WT_I32);
      ++cur_depth;
      gen_cond(cond->bop.rhs, false);
      ADD_CODE(OP_ELSE);
      ADD_CODE(OP_I32_CONST, 0);  // false
      ADD_CODE(OP_END);
      --cur_depth;
    }
    break;
  default:
    assert(false);
    break;
  }
}

static void gen_cond_jmp(Expr *cond, bool tf, uint32_t depth) {
  gen_cond(cond, tf);
  ADD_CODE(OP_BR_IF);
  ADD_ULEB128(depth);
}

static void gen_while(Stmt *stmt) {
  // TODO: Handle break, continue

  ADD_CODE(OP_BLOCK, WT_VOID);
  ADD_CODE(OP_LOOP, WT_VOID);
  cur_depth += 2;
  gen_cond_jmp(stmt->while_.cond, false, 1);
  gen_stmt(stmt->while_.body);
  ADD_CODE(OP_BR, 0);
  ADD_CODE(OP_END);
  ADD_CODE(OP_END);
  cur_depth -= 2;
}

static void gen_do_while(Stmt *stmt) {
  // TODO: Handle break, continue

  ADD_CODE(OP_BLOCK, WT_VOID);
  ADD_CODE(OP_LOOP, WT_VOID);
  cur_depth += 2;
  gen_stmt(stmt->while_.body);
  gen_cond_jmp(stmt->while_.cond, false, 1);
  ADD_CODE(OP_BR, 0);
  ADD_CODE(OP_END);
  ADD_CODE(OP_END);
  cur_depth -= 2;
}

static void gen_for(Stmt *stmt) {
  // TODO: Handle break, continue

  if (stmt->for_.pre != NULL)
    gen_expr_stmt(stmt->for_.pre);

  ADD_CODE(OP_BLOCK, WT_VOID);
  ADD_CODE(OP_LOOP, WT_VOID);
  cur_depth += 2;
  if (stmt->for_.cond != NULL)
    gen_cond_jmp(stmt->for_.cond, false, 1);
  gen_stmt(stmt->for_.body);
  if (stmt->for_.post != NULL)
    gen_expr_stmt(stmt->for_.post);
  ADD_CODE(OP_BR, 0);
  ADD_CODE(OP_END);
  ADD_CODE(OP_END);
  cur_depth -= 2;
}

static void gen_block(Stmt *stmt) {
  gen_stmts(stmt->block.stmts);
}

static void gen_return(Stmt *stmt) {
  assert(curfunc != NULL);
  if (stmt->return_.val != NULL) {
    Expr *val = stmt->return_.val;
    gen_expr(val);

    const Name *name = alloc_name(RETVAL_NAME, NULL, false);
    VarInfo *varinfo = scope_find(curfunc->scopes->data[0], name, NULL);
    assert(varinfo != NULL);
    ADD_CODE(OP_LOCAL_SET);
    ADD_ULEB128(varinfo->local.reg->local_index);
  }
  ADD_CODE(OP_BR);
  ADD_ULEB128(cur_depth - 1);
}

static void gen_if(Stmt *stmt) {
  gen_cond(stmt->if_.cond, true);
  ADD_CODE(OP_IF, WT_VOID);
  ++cur_depth;
  gen_stmt(stmt->if_.tblock);
  if (stmt->if_.fblock != NULL) {
    ADD_CODE(OP_ELSE);
    gen_stmt(stmt->if_.fblock);
  }
  ADD_CODE(OP_END);
  --cur_depth;
}

static void gen_vardecl(Vector *decls, Vector *inits) {
  if (curfunc != NULL) {
    UNUSED(decls);
    /*for (int i = 0; i < decls->len; ++i) {
      VarDecl *decl = decls->data[i];
      if (decl->init == NULL)
        continue;
      VarInfo *varinfo = scope_find(curscope, decl->ident->ident, NULL);
      if (varinfo == NULL || (varinfo->storage & (VS_STATIC | VS_EXTERN)) ||
          !(varinfo->type->kind == TY_STRUCT ||
            varinfo->type->kind == TY_ARRAY))
        continue;
      gen_clear_local_var(varinfo);
    }*/
  }
  gen_stmts(inits);
}

static void gen_expr_stmt(Expr *expr) {
  gen_expr(expr);
  if (expr->type->kind != TY_VOID)
    ADD_CODE(OP_DROP);
}

static void gen_stmt(Stmt *stmt) {
  if (stmt == NULL)
    return;

  switch (stmt->kind) {
  case ST_EXPR:  gen_expr_stmt(stmt->expr); break;
  case ST_RETURN:  gen_return(stmt); break;
  case ST_BLOCK:  gen_block(stmt); break;
  case ST_IF:  gen_if(stmt); break;
  /*case ST_SWITCH:  gen_switch(stmt); break;
  case ST_CASE:  gen_case(stmt); break;
  case ST_DEFAULT:  gen_default(); break;*/
  case ST_WHILE:  gen_while(stmt); break;
  case ST_DO_WHILE:  gen_do_while(stmt); break;
  case ST_FOR:  gen_for(stmt); break;
  /*case ST_BREAK:  gen_break(); break;
  case ST_CONTINUE:  gen_continue(); break;
  case ST_GOTO:  gen_goto(stmt); break;
  case ST_LABEL:  gen_label(stmt); break;*/
  case ST_VARDECL:  gen_vardecl(stmt->vardecl.decls, stmt->vardecl.inits); break;
  /*case ST_ASM:  gen_asm(stmt); break;*/

  default:
    parse_error(stmt->token, "Unhandled stmt: %d", stmt->kind);
    break;
  }
}

static void gen_stmts(Vector *stmts) {
  if (stmts == NULL)
    return;

  for (int i = 0, len = stmts->len; i < len; ++i) {
    Stmt *stmt = stmts->data[i];
    if (stmt == NULL)
      continue;
    gen_stmt(stmt);
  }
}

static void gen_defun(Function *func) {
  if (func->scopes == NULL)  // Prototype definition
    return;

  const Type *functype = func->type;

  code = malloc(sizeof(*code));
  data_init(code);

  DataStorage *data = malloc(sizeof(*data));
  data_init(data);

  // Allocate local variables.
  unsigned int param_count = functype->func.params != NULL ? functype->func.params->len : 0;  // TODO: Consider stack params.
  unsigned int local_count = 0;

  for (int i = 0; i < func->scopes->len; ++i) {
    Scope *scope = func->scopes->data[i];
    if (scope->vars == NULL)
      continue;

    for (int j = 0; j < scope->vars->len; ++j) {
      VarInfo *varinfo = scope->vars->data[j];
      if (varinfo->storage & (VS_STATIC | VS_EXTERN | VS_ENUM_MEMBER)) {
        // Static entity is allocated in global, not on stack.
        // Extern doesn't have its entity.
        // Enum members are replaced to constant value.
        continue;
      }

      VReg *vreg = malloc(sizeof(*vreg));
      varinfo->local.reg = vreg;
      int param_index = -1;
      if (i == 0 && param_count > 0) {
        const Vector *params = functype->func.params;
        for (int i = 0, n = params->len; i < n; ++i) {
          const VarInfo *v = params->data[i];
          if (equal_name(v->name, varinfo->name)) {
            param_index = i;
            break;
          }
        }
      }
      if (param_index >= 0) {
        vreg->local_index = param_index;
      } else {
        vreg->local_index = local_count++ + param_count;
        // TODO: Group same type variables.
        emit_uleb128(data, data->len, 1);  // TODO: Set type bytes.
        data_push(data, to_wtype(varinfo->type));
      }
    }
  }

  // Insert local count at the top.
  emit_uleb128(data, 0, local_count);  // Put local count at the top.

  curfunc = func;

  // Statements
  ADD_CODE(OP_BLOCK, WT_VOID);
  cur_depth += 1;
  gen_stmts(func->stmts);
  ADD_CODE(OP_END);
  cur_depth -= 1;
  assert(cur_depth == 0);
  if (functype->func.ret->kind != TY_VOID) {
    const Name *name = alloc_name(RETVAL_NAME, NULL, false);
    VarInfo *varinfo = scope_find(func->scopes->data[0], name, NULL);
    assert(varinfo != NULL);
    ADD_CODE(OP_LOCAL_GET);
    ADD_ULEB128(varinfo->local.reg->local_index);
  }
  ADD_CODE(OP_END);

  emit_uleb128(data, 0, data->len + code->len);  // Insert code size at the top.

  data_concat(data, code);
  func->bbcon = (BBContainer*)data;  // Store code to `bbcon`.

  curfunc = NULL;
  free(code);
  code = NULL;
}

static void gen_decl(Declaration *decl) {
  if (decl == NULL)
    return;

  switch (decl->kind) {
  case DCL_DEFUN:
    gen_defun(decl->defun.func);
    break;
  case DCL_VARDECL:
    break;

  default:
    error("Unhandled decl: %d", decl->kind);
    break;
  }
}

void gen(Vector *decls) {
  if (decls == NULL)
    return;

  for (int i = 0, len = decls->len; i < len; ++i) {
    Declaration *decl = decls->data[i];
    if (decl == NULL)
      continue;
    gen_decl(decl);
  }
}
