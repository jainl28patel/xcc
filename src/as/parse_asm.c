#include "parse_asm.h"

#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>  // strtol
#include <string.h>
#include <strings.h>

#include "gen.h"
#include "ir_asm.h"
#include "table.h"
#include "util.h"

// Align with Opcode.
static const char *kOpTable[] = {
  "mov",
  "movsx",
  "movzx",
  "lea",

  "add",
  "addq",
  "sub",
  "subq",
  "mul",
  "div",
  "idiv",
  "neg",
  "not",
  "inc",
  "incl",
  "incq",
  "dec",
  "decl",
  "decq",
  "and",
  "or",
  "xor",
  "shl",
  "shr",
  "cmp",
  "test",
  "cltd",
  "cqto",

  "seto",
  "setno",
  "setb",
  "setae",
  "sete",
  "setne",
  "setbe",
  "seta",
  "sets",
  "setns",
  "setp",
  "setnp",
  "setl",
  "setge",
  "setle",
  "setg",

  "jmp",
  "jo",
  "jno",
  "jb",
  "jae",
  "je",
  "jne",
  "jbe",
  "ja",
  "js",
  "jns",
  "jp",
  "jnp",
  "jl",
  "jge",
  "jle",
  "jg",
  "call",
  "ret",
  "push",
  "pop",

  "int",
  "syscall",
};

static const struct {
  const char *name;
  enum RegType reg;
} kRegisters[] = {
  {"al", AL},
  {"cl", CL},
  {"dl", DL},
  {"bl", BL},
  {"spl", SPL},
  {"bpl", BPL},
  {"sil", SIL},
  {"dil", DIL},

  {"r8b", R8B},
  {"r9b", R9B},
  {"r10b", R10B},
  {"r11b", R11B},
  {"r12b", R12B},
  {"r13b", R13B},
  {"r14b", R14B},
  {"r15b", R15B},

  {"ax", AX},
  {"cx", CX},
  {"dx", DX},
  {"bx", BX},
  {"sp", SP},
  {"bp", BP},
  {"si", SI},
  {"di", DI},

  {"r8w", R8W},
  {"r9w", R9W},
  {"r10w", R10W},
  {"r11w", R11W},
  {"r12w", R12W},
  {"r13w", R13W},
  {"r14w", R14W},
  {"r15w", R15W},

  {"eax", EAX},
  {"ecx", ECX},
  {"edx", EDX},
  {"ebx", EBX},
  {"esp", ESP},
  {"ebp", EBP},
  {"esi", ESI},
  {"edi", EDI},

  {"r8d", R8D},
  {"r9d", R9D},
  {"r10d", R10D},
  {"r11d", R11D},
  {"r12d", R12D},
  {"r13d", R13D},
  {"r14d", R14D},
  {"r15d", R15D},

  {"rax", RAX},
  {"rcx", RCX},
  {"rdx", RDX},
  {"rbx", RBX},
  {"rsp", RSP},
  {"rbp", RBP},
  {"rsi", RSI},
  {"rdi", RDI},

  {"r8", R8},
  {"r9", R9},
  {"r10", R10},
  {"r11", R11},
  {"r12", R12},
  {"r13", R13},
  {"r14", R14},
  {"r15", R15},

  {"rip", RIP},
};

static const char *kDirectiveTable[] = {
  "ascii",
  "section",
  "text",
  "data",
  "align",
  "byte",
  "word",
  "long",
  "quad",
  "comm",
  "globl",
  "extern",
};

static bool is_reg8(enum RegType reg) {
  return reg >= AL && reg <= R15B;
}

static bool is_reg16(enum RegType reg) {
  return reg >= AX && reg <= R15W;
}

static bool is_reg32(enum RegType reg) {
  return reg >= EAX && reg <= R15D;
}

static bool is_reg64(enum RegType reg) {
  return reg >= RAX && reg <= R15;
}

const char *skip_whitespace(const char *p) {
  while (isspace(*p))
    ++p;
  return p;
}

static int find_match_index(const char **pp, const char **table, size_t count) {
  const char *p = *pp;
  const char *start = p;

  while (isalpha(*p))
    ++p;
  if (*p == '\0' || isspace(*p)) {
    size_t n = p - start;
    for (size_t i = 0; i < count; ++i) {
      const char *name = table[i];
      size_t len = strlen(name);
      if (n == len && strncasecmp(start, name, n) == 0) {
        *pp = skip_whitespace(p);
        return i;
      }
    }
  }
  return -1;
}

enum Opcode parse_opcode(const char **pp) {
  return find_match_index(pp, kOpTable, sizeof(kOpTable) / sizeof(*kOpTable)) + 1;
}

enum DirectiveType parse_directive(const char **pp) {
  return find_match_index(pp, kDirectiveTable, sizeof(kDirectiveTable) / sizeof(*kDirectiveTable)) + 1;
}

enum RegType parse_register(const char **pp) {
  const char *p = *pp;
  for (int i = 0, len = sizeof(kRegisters) / sizeof(*kRegisters); i < len; ++i) {
    const char *name = kRegisters[i].name;
    size_t n = strlen(name);
    if (strncmp(p, name, n) == 0) {
      *pp = p + n;
      return kRegisters[i].reg;
    }
  }
  return NOREG;
}

bool parse_immediate(const char **pp, long *value) {
  const char *p = *pp;
  bool negative = false;
  if (*p == '-') {
    negative = true;
    ++p;
  }
  if (!isdigit(*p))
    return false;
  long v = strtol(p, (char**)pp, 10);
  *value = negative ? -v : v;
  return true;
}

static bool is_label_first_chr(char c) {
  return isalpha(c) || c == '_' || c == '.';
}

static bool is_label_chr(char c) {
  return is_label_first_chr(c) || isdigit(c);
}

const Name *parse_label(const char **pp) {
  const char *p = *pp;
  const char *start = p;
  if (!is_label_first_chr(*p))
    return NULL;

  do {
    ++p;
  } while (is_label_chr(*p));
  *pp = p;
  return alloc_name(start, p, false);
}

bool parse_operand(const char **pp, Operand *operand) {
  const char *p = *pp;
  if (*p == '%') {
    *pp = p + 1;
    enum RegType reg = parse_register(pp);
    enum RegSize size;
    int no;
    if (is_reg8(reg)) {
      size = REG8;
      no = reg - AL;
    } else if (is_reg16(reg)) {
      size = REG16;
      no = reg - AX;
    } else if (is_reg32(reg)) {
      size = REG32;
      no = reg - EAX;
    } else if (is_reg64(reg)) {
      size = REG64;
      no = reg - RAX;
    } else {
      error("Illegal register");
      return false;
    }

    operand->type = REG;
    operand->reg.size = size;
    operand->reg.no = no & 7;
    operand->reg.x = (no & 8) >> 3;
    return true;
  }

  if (*p == '*' && p[1] == '%') {
    *pp = p + 2;
    enum RegType reg = parse_register(pp);
    if (!is_reg64(reg))
      error("Illegal register");

    char no = reg - RAX;
    operand->type = DEREF_REG;
    operand->deref_reg.size = REG64;
    operand->deref_reg.no = no & 7;
    operand->deref_reg.x = (no & 8) >> 3;
    return true;
  }

  if (*p == '$') {
    *pp = p + 1;
    if (!parse_immediate(pp, &operand->immediate))
      error("Syntax error");
    operand->type = IMMEDIATE;
    return true;
  }

  bool has_offset = false;
  long offset = 0;
  const Name *label = parse_label(pp);
  if (label == NULL) {
    bool neg = false;
    if (*p == '-') {
      neg = true;
      ++p;
    }
    if (isdigit(*p)) {
      offset = strtol(p, (char**)pp, 10);
      if (*pp > p)
        has_offset = true;
      if (neg)
        offset = -offset;
    } else if (neg) {
      error("Illegal `-'");
    }
  }
  p = skip_whitespace(*pp);
  if (*p != '(') {
    if (label != NULL) {
      operand->type = LABEL;
      operand->label = label;
      *pp = p;
      return true;
    }
    if (has_offset)
      error("direct number not implemented");
  } else {
    if (p[1] == '%') {
      *pp = p + 2;
      enum RegType reg = parse_register(pp);
      if (!(is_reg64(reg) || reg == RIP))
        error("Register expected");
      p = skip_whitespace(*pp);
      if (*p != ')')
        error("`)' expected");
      *pp = ++p;

      char no = reg - RAX;
      operand->type = INDIRECT;
      operand->indirect.reg.size = REG64;
      operand->indirect.reg.no = reg != RIP ? no & 7 : RIP;
      operand->indirect.reg.x = (no & 8) >> 3;
      operand->indirect.label = label;
      operand->indirect.offset = offset;
      return true;
    }
    error("Illegal `('");
  }

  return false;
}

void parse_inst(const char **pp, Inst *inst) {
  const char *p = *pp;
  enum Opcode op = parse_opcode(&p);
  inst->op = op;
  if (op != NOOP) {
    if (parse_operand(&p, &inst->src)) {
      p = skip_whitespace(p);
      if (*p == ',') {
        p = skip_whitespace(p + 1);
        parse_operand(&p, &inst->dst);
        p = skip_whitespace(p);
      }
    }
  }

  *pp = p;
}

int current_section = SEC_CODE;
bool err;

Line *parse_line(const char *rawline) {
  Line *line = malloc(sizeof(*line));
  line->rawline = rawline;
  line->label = NULL;
  line->inst.op = NOOP;
  line->inst.src.type = line->inst.dst.type = NOOPERAND;
  line->dir = NODIRECTIVE;

  const char *p = rawline;
  line->label = parse_label(&p);
  if (line->label != NULL) {
    if (*p != ':')
      error("`:' expected");
    ++p;
  }

  p = skip_whitespace(p);
  if (*p == '.') {
    ++p;
    enum DirectiveType dir = parse_directive(&p);
    if (dir == NODIRECTIVE)
      error("Unknown directive");
    line->dir = dir;
    line->directive_line = p;
  } else if (*p != '\0') {
    parse_inst(&p, &line->inst);
    if (*p != '\0' && !(*p == '/' && p[1] == '/')) {
      fprintf(stderr, "Syntax error: %s\n", p);
      err = true;
    }
  }
  return line;
}

static char unescape_char(char c) {
  switch (c) {
  case '0':  return '\0';
  case 'n':  return '\n';
  case 't':  return '\t';
  case 'r':  return '\r';
  case '"':  return '"';
  case '\'':  return '\'';
  default:
    return c;
  }
}

static size_t unescape_string(const char *p, char *dst) {
  size_t len = 0;
  for (; *p != '"'; ++p, ++len) {
    char c = *p;
    if (c == '\0')
      error("string not closed");
    if (c == '\\') {
      // TODO: Handle \x...
      c = unescape_char(*(++p));
    }
    if (dst != NULL)
      *dst++ = c;
  }
  return len;
}

void handle_directive(enum DirectiveType dir, const char *p, Vector **section_irs) {
  Vector *irs = section_irs[current_section];

  switch (dir) {
  case DT_ASCII:
    {
      if (*p != '"')
        error("`\"' expected");
      ++p;
      size_t len = unescape_string(p, NULL);
      char *str = malloc(len);
      unescape_string(p, str);

      vec_push(irs, new_ir_data(str, len));
    }
    break;

  case DT_COMM:
    {
      const Name *label = parse_label(&p);
      if (label == NULL)
        error(".comm: label expected");
      p = skip_whitespace(p);
      if (*p != ',')
        error(".comm: `,' expected");
      p = skip_whitespace(p + 1);
      long count;
      if (!parse_immediate(&p, &count))
        error(".comm: count expected");
      current_section = SEC_BSS;
      irs = section_irs[current_section];
      vec_push(irs, new_ir_label(label));
      vec_push(irs, new_ir_bss(count));
    }
    break;

  case DT_TEXT:
    current_section = SEC_CODE;
    break;

  case DT_DATA:
    current_section = SEC_DATA;
    break;

  case DT_ALIGN:
    {
      long align;
      if (!parse_immediate(&p, &align))
        error(".align: number expected");
      vec_push(irs, new_ir_align(align));
    }
    break;

  case DT_BYTE:
  case DT_WORD:
  case DT_LONG:
  case DT_QUAD:
    {
      long value;
      if (parse_immediate(&p, &value)) {
        // TODO: Target endian.
        int size = 1 << (dir - DT_BYTE);
        unsigned char *buf = malloc(size);
        for (int i = 0; i < size; ++i)
          buf[i] = value >> (8 * i);
        vec_push(irs, new_ir_data(buf, size));
      } else {
        const Name *label = parse_label(&p);
        if (label != NULL) {
          if (dir == DT_QUAD) {
            vec_push(irs, new_ir_abs_quad(label));
          } else {
            error("label can use only in .quad");
          }
        } else {
          error(".quad: number or label expected");
        }
      }
    }
    break;

  case DT_SECTION:
  case DT_GLOBL:
  case DT_EXTERN:
    break;

  default:
    fprintf(stderr, "Unhandled directive: %d, %s\n", dir, p);
    break;
  }
}
