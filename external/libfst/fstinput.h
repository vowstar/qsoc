
/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com> */
#ifndef QSOC_FST_INPUT_H
#define QSOC_FST_INPUT_H
#include "fstapi.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { INPUT_OK, INPUT_EOF, INPUT_CANCELLED, INPUT_IO_ERROR, INPUT_RANGE,
       INPUT_MALFORMED, INPUT_UNSUPPORTED, INPUT_MISSING_HIERARCHY };
typedef struct {
    uint32_t abi_version, struct_size;
    void *user;
    int (*read_at)(void *, uint64_t, void *, uint32_t, uint32_t *);
    int (*size)(void *, uint64_t *);
    int (*cancelled)(void *);
    void (*progress)(void *, uint64_t);
    FILE *(*temporary_file)(void *, uint64_t);
    uint64_t hierarchy_limit;
} QSocFstInput;
fstReaderContext *fstReaderOpenWithInput(const QSocFstInput *, int *);
fstReaderContext *fstReaderOpenWithInputAndHierarchy(const QSocFstInput *, const QSocFstInput *, int *);
int fstReaderInputStatus(fstReaderContext *);
uint64_t fstReaderInputSectionOffset(fstReaderContext *, uint64_t);
int fstReaderInputSignalInfo(fstReaderContext *, fstHandle, uint32_t *, int *);
#ifdef __cplusplus
}
#endif
#endif
