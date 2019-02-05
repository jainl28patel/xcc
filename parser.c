#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>  // malloc
#include <string.h>

#include "xcc.h"

char *strndup_(const char *str, size_t size) {
  char *dup = malloc(size + 1);
  strncpy(dup, str, size);
  dup[size] = '\0';
  return dup;
}

const static Type tyVoid = {TY_VOID, NULL};
const static Type tyInt = {TY_INT, NULL};

Vector *token_vector;

Token *alloc_token(enum TokenType type, const char *input) {
  Token *token = malloc(sizeof(*token));
  token->type = type;
  token->input = input;
  vec_push(token_vector, token);
  return token;
}

Token *get_token(int pos) {
  return (Token *)token_vector->data[pos];
}

enum TokenType reserved_word(const char *word) {
  struct {
    const char *str;
    enum TokenType type;
  } table[] = {
    { "if", TK_IF },
    { "else", TK_ELSE },
    { "while", TK_WHILE },
    { "int", TK_INT },
  };
  for (int i = 0; i < sizeof(table) / sizeof(*table); ++i) {
    if (strcmp(table[i].str, word) == 0)
      return table[i].type;
  }
  return -1;
}

void tokenize(const char *p) {
  int i = 0;
  while (*p != '\0') {
    if (isspace(*p)) {
      ++p;
      continue;
    }

    if (*p == '=') {
      switch (p[1]) {
      case '=':
        alloc_token(TK_EQ, p);
        ++i;
        p += 2;
        continue;
      default:
        break;
      }
    }

    if (*p == '!' && p[1] == '=') {
      alloc_token(TK_NE, p);
      ++i;
      p += 2;
      continue;
    }

    if (strchr("+-*/&(){}}=;,", *p) != NULL) {
      alloc_token((enum TokenType)*p, p);
      ++i;
      ++p;
      continue;
    }

    if (isdigit(*p)) {
      long val = strtol(p, (char**)&p, 10);
      Token *token = alloc_token(TK_NUM, p);
      token->val = val;
      ++i;
      continue;
    }

    if (isalpha(*p) || *p == '_') {
      const char *q;
      for (q = p + 1; ; ++q) {
        if (!(isalnum(*q) || *q == '_'))
          break;
      }

      char *dup = strndup_(p, q - p);
      enum TokenType word = reserved_word(dup);
      if (word != -1) {
        alloc_token(word, p);
      } else {
        Token *token = alloc_token(TK_IDENT, p);
        token->ident = dup;
      }
      ++i;
      p = q;
      continue;
    }

    error("Unexpected character: %c\n", *p);
  }

  alloc_token(TK_EOF, p);
}

//

int var_find(Vector *lvars, const char *name) {
  int len = lvars->len;
  for (int i = 0; i < len; ++i) {
    VarInfo *info = (VarInfo*)lvars->data[i];
    if (strcmp(info->name, name) == 0)
      return i;
  }
  return -1;
}

int var_add(Vector *lvars, const char *name) {
  int idx = var_find(lvars, name);
  if (idx < 0) {
    idx = lvars->len;
    vec_push(lvars, name);
  }
  return idx;
}

//

static int pos;
static Node *curfunc;

Type* ptrof(const Type *type) {
  Type *ptr = malloc(sizeof(*ptr));
  ptr->type = TY_PTR;
  ptr->ptrof = type;
  return ptr;
}

void decl_var(Vector *lvars, const char *name, Type *type) {
  VarInfo *info = malloc(sizeof(*info));
  info->name = name;
  info->type = type;
  vec_push(lvars, info);
}

Node *new_node_bop(enum NodeType type, Node *lhs, Node *rhs) {
  Node *node = malloc(sizeof(Node));
  node->type = type;
  node->bop.lhs = lhs;
  node->bop.rhs = rhs;
  switch (type) {
  case ND_ASSIGN:
    // Check lhs and rhs types are equal.
    node->expType = rhs->expType;
    break;
  case ND_ADD:
    if (lhs->expType->type == TY_PTR) {
      if (rhs->expType->type == TY_PTR)
        error("Cannot add pointers");
      node->expType = lhs->expType;
    } else {
      node->expType = rhs->expType;  // Pointer or int.
    }
    break;
  case ND_SUB:
    if (lhs->expType->type == TY_PTR) {
      if (rhs->expType->type == TY_PTR)
        error("Cannot sub pointers");
      node->expType = lhs->expType;
    } else {
      if (lhs->expType->type == TY_PTR)
        error("Cannot sub pointer");
      node->expType = rhs->expType;  // int.
    }
    break;
  case ND_MUL:
  case ND_DIV:
    if (lhs->expType->type == TY_PTR || rhs->expType->type == TY_PTR) {
      error("Cannot sub pointers");
    } else {
      node->expType = lhs->expType;  // int.
    }
    break;
  default:
    //assert(FALSE);
    node->expType = lhs->expType;
    break;
  }
  return node;
}

Node *new_node_unary(enum NodeType type, Node *sub) {
  Node *node = malloc(sizeof(Node));
  node->type = type;
  switch (type) {
  case ND_REF:
    node->expType = ptrof(sub->expType);
    break;
  case ND_DEREF:
    if (sub->expType->type != TY_PTR)
      error("Cannot dereference raw type");
    node->expType = sub->expType->ptrof;
    break;
  default:
    node->expType = sub->expType;
    break;
  }
  node->unary.sub = sub;
  return node;
}

Node *new_node_num(int val) {
  Node *node = malloc(sizeof(Node));
  node->type = ND_NUM;
  node->expType = &tyInt;
  node->val = val;
  return node;
}

Node *new_node_ident(const char *name, const Type *type) {
  Node *node = malloc(sizeof(Node));
  node->type = ND_IDENT;
  node->expType = type;
  node->ident = name;
  return node;
}

Node *new_node_defun(const char *name, Vector *params) {
  Node *node = malloc(sizeof(Node));
  node->type = ND_DEFUN;
  node->expType = &tyVoid;
  node->defun.name = name;
  node->defun.lvars = params;
  node->defun.param_count = params->len;
  node->defun.stmts = NULL;
  return node;
}

Node *new_node_funcall(const char *name, Vector *args) {
  Node *node = malloc(sizeof(Node));
  node->type = ND_FUNCALL;
  node->expType = &tyInt;  // TODO:
  node->funcall.name = name;
  node->funcall.args = args;
  return node;
}

Node *new_node_block(Vector *nodes) {
  Node *node = malloc(sizeof(Node));
  node->type = ND_BLOCK;
  node->expType = &tyVoid;
  node->block.nodes = nodes;
  return node;
}

Node *new_node_if(Node *cond, Node *tblock, Node *fblock) {
  Node *node = malloc(sizeof(Node));
  node->type = ND_IF;
  node->expType = &tyVoid;
  node->if_.cond = cond;
  node->if_.tblock = tblock;
  node->if_.fblock = fblock;
  return node;
}

Node *new_node_while(Node *cond, Node *body) {
  Node *node = malloc(sizeof(Node));
  node->type = ND_WHILE;
  node->expType = &tyVoid;
  node->while_.cond = cond;
  node->while_.body = body;
  return node;
}

int consume(enum TokenType type) {
  if (get_token(pos)->type != type)
    return FALSE;
  ++pos;
  return TRUE;
}

Node *assign();

Node *funcall(const char *name) {
  Vector *args = NULL;
  if (!consume(TK_RPAR)) {
    args = new_vector();
    for (;;) {
      vec_push(args, assign());
      if (consume(TK_RPAR))
        break;
      if (consume(TK_COMMA))
        continue;
      error("Comma or `)` expected, but %s", get_token(pos)->input);
    }
  }
  return new_node_funcall(name, args);
}

Node *term() {
  if (consume(TK_LPAR)) {
    Node *node = assign();
    if (!consume(TK_RPAR))
      error("No close paren: %s", get_token(pos)->input);
    return node;
  }

  if (consume(TK_AMP)) {
    Node *node = term();
    return new_node_unary(ND_REF, node);
  }

  if (consume(TK_MUL)) {
    Node *node = term();
    return new_node_unary(ND_DEREF, node);
  }

  Token *token = get_token(pos);
  switch (token->type) {
  case TK_NUM:
    ++pos;
    return new_node_num(token->val);
  case TK_IDENT:
    ++pos;
    if (consume(TK_LPAR)) {
      return funcall(token->ident);
    } else {
      if (curfunc == NULL)
        error("Cannot use variable outside of function: `%s'", token->ident);
      int idx = var_find(curfunc->defun.lvars, token->ident);
      if (idx < 0)
        error("Undefined `%s'", token->ident);
      VarInfo *info = (VarInfo*)curfunc->defun.lvars->data[idx];
      return new_node_ident(token->ident, info->type);
    }
  default:
    error("Number or Ident or open paren expected: %s", token->input);
    return NULL;
  }
}

Node *mul() {
  Node *node = term();

  for (;;) {
    if (consume(TK_MUL))
      node = new_node_bop(ND_MUL, node, term());
    else if (consume(TK_DIV))
      node = new_node_bop(ND_DIV, node, term());
    else
      return node;
  }
}

Node *add() {
  Node *node = mul();

  for (;;) {
    if (consume(TK_ADD))
      node = new_node_bop(ND_ADD, node, mul());
    else if (consume(TK_SUB))
      node = new_node_bop(ND_SUB, node, mul());
    else
      return node;
  }
}

Node *eq() {
  Node *node = add();

  for (;;) {
    if (consume(TK_EQ))
      node = new_node_bop(ND_EQ, node, add());
    else if (consume(TK_NE))
      node = new_node_bop(ND_NE, node, add());
    else
      return node;
  }
}

Node *assign() {
  Node *node = eq();

  if (consume(TK_ASSIGN))
    return new_node_bop(ND_ASSIGN, node, assign());
  else
    return node;
}

Node *stmt();

Node *block() {
  Vector *nodes = new_vector();
  for (;;) {
    if (consume(TK_RBRACE))
      return new_node_block(nodes);
    vec_push(nodes, stmt());
  }
}

Node *stmt_if() {
  if (consume(TK_LPAR)) {
    Node *cond = assign();
    if (consume(TK_RPAR)) {
      Node *tblock = stmt();
      Node *fblock = NULL;
      if (consume(TK_ELSE)) {
        fblock = stmt();
      }
      return new_node_if(cond, tblock, fblock);
    }
  }
  error("Parse `if' failed: %s", get_token(pos)->input);
  return NULL;
}

Node *stmt_while() {
  if (consume(TK_LPAR)) {
    Node *cond = assign();
    if (consume(TK_RPAR)) {
      Node *body = stmt();
      return new_node_while(cond, body);
    }
  }
  error("Parse `while' failed: %s", get_token(pos)->input);
  return NULL;
}

Type *parse_type() {
  Type *type = malloc(sizeof(*type));
  type->type = TY_INT;
  type->ptrof = NULL;

  while (consume(TK_MUL))
    type = ptrof(type);

  return type;
}

void vardecl() {
  Type *type = parse_type();

  if (!consume(TK_IDENT))
    error("Ident expected, but %s", get_token(pos)->input);
  const char *name = get_token(pos - 1)->ident;
  if (!consume(TK_SEMICOL))
    error("Semicolon expected, but %s", get_token(pos)->input);
  assert(curfunc != NULL);
  decl_var(curfunc->defun.lvars, name, type);
}

Node *stmt() {
  while (consume(TK_INT)) {
    vardecl();
  }

  if (consume(TK_LBRACE))
    return block();

  if (consume(TK_IF))
    return stmt_if();

  if (consume(TK_WHILE))
    return stmt_while();

  // expression statement.
  Node *node = assign();
  if (!consume(TK_SEMICOL))
    error("Semicolon required: %s", get_token(pos)->input);
  return node;
}

Vector *funparams() {
  Vector *params = new_vector();
  if (!consume(TK_RPAR)) {
    for (;;) {
      if (!consume(TK_INT))
        error("`int' expected, but %s", get_token(pos)->input);
      Type *type = parse_type();
      if (!consume(TK_IDENT))
        error("Ident expected, but %s", get_token(pos)->input);
      decl_var(params, get_token(pos - 1)->ident, type);
      if (consume(TK_RPAR))
        break;
      if (consume(TK_COMMA))
        continue;
      error("Comma or `}' expected, but %s", get_token(pos)->input);
    }
  }
  return params;
}

Node *toplevel() {
  if (consume(TK_INT)) {
    if (consume(TK_IDENT)) {
      Token *funcname = get_token(pos - 1);
      if (consume(TK_LPAR)) {
        Vector *params = funparams();
        if (consume(TK_LBRACE)) {
          Node *node = new_node_defun(funcname->ident, params);
          curfunc = node;

          Vector *stmts = new_vector();
          while (!consume(TK_RBRACE)) {
            Node *st = stmt();
            vec_push(stmts, st);
          }
          node->defun.stmts = stmts;
          return node;
        }
      }
    }
    error("Defun failed: %s", get_token(pos)->input);
    return NULL;
  }
  error("Toplevel, %s", get_token(pos)->input);
  return NULL;
}

Vector *node_vector;

void program() {
  while (get_token(pos)->type != TK_EOF)
    vec_push(node_vector, toplevel());
}