/*
 * Copyright (c) 2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
#include <mach-o/nlist.h>
#include <stuff/bool.h>

/*
 * Data structures to perform selective stripping of symbol table entries.
 */
struct symbol_list {
    char *name;		/* name of the global symbol */
    struct nlist *sym;	/* pointer to the nlist structure for this symbol */
    enum bool seen;	/* set if the symbol is seen in the input file */
};

__private_extern__ void setup_symbol_list(
    char *file,
    struct symbol_list **list,
    unsigned long *size);

__private_extern__ int symbol_list_bsearch(
    const char *name,
    const struct symbol_list *sym);
