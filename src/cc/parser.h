// Parser for statement

#pragma once

#include <stdbool.h>
#include <stdint.h>  // intptr_t

typedef struct Function Function;
typedef struct Expr Expr;
typedef struct Map Map;
typedef struct Scope Scope;
typedef struct Token Token;
typedef struct Type Type;
typedef struct Vector Vector;

// Defun

typedef struct Defun {
  Function *func;

  Vector *stmts;  // NULL => Prototype definition.

  Map *label_map;  // <const char*, BB*>
  Vector *gotos;

  int flag;
} Defun;

// Initializer

typedef struct Initializer {
  enum { vSingle, vMulti, vDot, vArr } kind;  // vSingle: 123, vMulti: {...}, vDot: .x=123, vArr: [n]=123
  const Token *token;
  union {
    Expr *single;
    Vector *multi;  // <Initializer*>
    struct {
      const char *name;
      struct Initializer *value;
    } dot;
    struct {
      Expr *index;
      struct Initializer *value;
    } arr;
  };
} Initializer;

// Statement

enum StmtKind {
  ST_EXPR,
  ST_DEFUN,
  ST_BLOCK,
  ST_IF,
  ST_SWITCH,
  ST_WHILE,
  ST_DO_WHILE,
  ST_FOR,
  ST_BREAK,
  ST_CONTINUE,
  ST_RETURN,
  ST_CASE,
  ST_DEFAULT,
  ST_GOTO,
  ST_LABEL,
  ST_VARDECL,
  ST_ASM,
  ST_TOPLEVEL,
};

typedef struct VarDecl {
  const Type *type;
  const Token *ident;
  Initializer *init;
  int flag;
} VarDecl;

typedef struct Stmt {
  enum StmtKind kind;
  const Token *token;
  union {
    Expr *expr;
    Defun *defun;
    struct {
      Scope *scope;
      Vector *stmts;
    } block;
    struct {
      Expr *cond;
      struct Stmt *tblock;
      struct Stmt *fblock;
    } if_;
    struct {
      Expr *value;
      struct Stmt *body;
      Vector *case_values;  // <intptr_t>
      bool has_default;
    } switch_;
    struct {
      Expr *value;
    } case_;
    struct {
      Expr *cond;
      struct Stmt *body;
    } while_;
    struct {
      Expr *pre;
      Expr *cond;
      Expr *post;
      struct Stmt *body;
    } for_;
    struct {
      const Token *label;
    } goto_;
    struct {
      // const Token *label;
      struct Stmt *stmt;
    } label;
    struct {
      Expr *val;
    } return_;
    struct {
      Vector *decls;  // <VarDecl*>
      Vector *inits;  // <Stmt*>
    } vardecl;
    struct {
      Expr *str;
    } asm_;
    struct {
      Vector *stmts;
    } toplevel;
  };
} Stmt;

Stmt *new_stmt_expr(Expr *e);
Stmt *new_top_stmt(Vector *stmts);

Vector *parse_program(Vector *stmts);
