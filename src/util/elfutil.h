// ELF format utility

#pragma once

#include <stdint.h>  // ssize_t
#include <stdio.h>  // FILE

#ifdef __APPLE__
#include "../../include/elf.h"
#else
#include <elf.h>
#endif

#include "table.h"

// String table for ELF.
typedef struct {
  Table offsets;
  size_t size;
} Strtab;

void strtab_init(Strtab *strtab);
size_t strtab_add(Strtab *strtab, const Name *name);
void *strtab_dump(Strtab *strtab);

//

typedef struct {
  Strtab strtab;
  Table indices;
  Elf64_Sym *buf;
  int count;
} Symtab;

void symtab_init(Symtab *symtab);
Elf64_Sym *symtab_add(Symtab *symtab, const Name *name);

//

void out_elf_header(FILE *fp, uintptr_t entry, int phnum, int shnum, int flags);
void out_program_header(FILE *fp, int sec, uintptr_t offset, uintptr_t vaddr, size_t filesz,
                        size_t memsz);
