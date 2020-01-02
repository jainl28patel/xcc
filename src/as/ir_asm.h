// Intermediate Representation for assembly

#pragma once

#include <stddef.h>  // size_t
#include <stdint.h>  // uintptr_t

#include "asm_x86.h"

typedef struct Name Name;
typedef struct Table Table;
typedef struct Vector Vector;

typedef struct {
  size_t len;
  unsigned char *buf;
} Data;

enum IrKind {
  IR_LABEL,
  IR_CODE,
  IR_DATA,
  IR_BSS,
  IR_ALIGN,
  IR_ABS_QUAD,
};

typedef struct {
  enum IrKind kind;
  union {
    const Name *label;
    Code code;
    Data data;
    size_t bss;
    int align;
    int section;
  };
  uintptr_t address;
} IR;

IR *new_ir_label(const Name *label);
IR *new_ir_code(const Code *code);
IR *new_ir_data(const void *data, size_t size);
IR *new_ir_bss(size_t size);
IR *new_ir_align(int align);
IR *new_ir_abs_quad(const Name *label);

void calc_label_address(uintptr_t start_address, Vector **section_irs, Table *label_table);
bool resolve_relative_address(Vector **section_irs, Table *label_table);
void emit_irs(Vector **section_irs, Table *label_table);
