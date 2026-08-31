/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_NATIVE_DYNAMIC_PROOF_H
#define SAPOTE_NATIVE_DYNAMIC_PROOF_H

#include <stddef.h>

void proof_write(const char *text, size_t length);
int dynamic_add(int value);
int dynamic_tls_value(void);

#endif
