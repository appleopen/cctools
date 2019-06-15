/*
 * Copyright (c) 2015 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stuff/bool.h"
#include "stuff/errors.h"
#include "stuff/breakout.h"
#include "stuff/allocate.h"
#include "stuff/rnd.h"

/* used by error routines as the name of the program */
char *progname = NULL;

static enum bool rflag = FALSE; /* remove bitcode segment */
static enum bool mflag = FALSE; /* remove bitcode but leave a marker segment */
static enum bool lflag = FALSE; /* leave only bitcode segment */

/*
 * We shortcut bitcode_strip(1) to do nothing with the -r option when there is
 * no bitcode and the input file is the same as the output file.  The this will
 * get set to TRUE by strip_bitcode_segment() if it sees any slice containing
 * bitcode.
 */
static enum bool some_slice_has_bitcode = FALSE;

static void usage(
    void);

static void process(
    struct arch *archs,
    uint32_t narchs);

static enum bool check_object(
    struct arch *arch,
    struct object *object);

static void strip_bitcode_segment(
    struct arch *arch,
    struct object *object);

static void leave_just_bitcode_segment(
    struct arch *arch,
    struct object *object);

static void strip_bitcode_from_load_commands(
    struct arch *arch,
    struct object *object);

static void leave_only_bitcode_load_commands(
    struct arch *arch,
    struct object *object);

static void reset_pointers_for_object_load_commands(
    struct arch *arch,
    struct object *object);

/*
 * The bitcode_strip(1) program takes one of two options:
 * -r remove the bitcode segment  
 * -l leave only the bitcode segment if present (or leave the file the same).
 * and operates on a single input file and writes to a specified output file
 * with the "-o output" argument.
 */
int
main(
int argc,
char **argv,
char **envp)
{
    uint32_t i;
    char *input, *output;
    struct arch *archs;
    uint32_t narchs;

	progname = argv[0];
	input = NULL;
	output = NULL;
	archs = NULL;
	narchs = 0;
	for(i = 1; i < argc; i++){
	    if(strcmp(argv[i], "-o") == 0){
		if(i + 1 == argc){
		    error("missing argument(s) to: %s option", argv[i]);
		    usage();
		}
		if(output != NULL){
		    error("more than one: %s option specified", argv[i]);
		    usage();
		}
		output = argv[i+1];
		i++;
	    }
	    else if(strcmp(argv[i], "-l") == 0){
		if(rflag == TRUE || mflag == TRUE){
		    error("only one of -r, -m or -l can be specified");
		    usage();
		}
		lflag = TRUE;
	    }
	    else if(strcmp(argv[i], "-r") == 0){
		if(lflag == TRUE || mflag == TRUE){
		    error("only one of -r, -m or -l can be specified");
		    usage();
		}
		rflag = TRUE;
	    }
	    else if(strcmp(argv[i], "-m") == 0){
		if(lflag == TRUE || rflag == TRUE){
		    error("only one of -r, -m or -l can be specified");
		    usage();
		}
		mflag = TRUE;
	    }
	    else{
		if(input != NULL){
		    error("more than one input file specified (%s and %s)",
			  argv[i], input);
		    usage();
		}
		input = argv[i];
	    }
	}
	if(rflag == FALSE && mflag == FALSE && lflag == FALSE){
	    error("one of -r, -m or -l must specified");
	    usage();
	}
	if(input == NULL || output == NULL)
	    usage();

	breakout(input, &archs, &narchs, FALSE);
	if(errors)
	    exit(EXIT_FAILURE);

	checkout(archs, narchs);

	process(archs, narchs);

	/*
	 * We shortcut bitcode_strip(1) to do nothing with the -r option when
	 * there is no bitcode and the input file is the same as the output
	 * file.
	 */
	if(rflag && some_slice_has_bitcode == FALSE &&
	   strcmp(input, output) == 0)
	    return(EXIT_SUCCESS);

	writeout(archs, narchs, output, 0777, TRUE, FALSE, FALSE, NULL);

	if(errors)
	    return(EXIT_FAILURE);
	else
	    return(EXIT_SUCCESS);
}

/*
 * usage() prints the current usage message and exits indicating failure.
 */
static
void
usage(
void)
{
	fprintf(stderr, "Usage: %s input [-r | -m | -l] -o output\n", progname);
	exit(EXIT_FAILURE);
}

/*
 * process() drives the work to strip or leave the bitcode segment from each
 * architecture slice.  The arch must be a fully linked Mach-O file and not
 * an archive.
 */
static
void
process(
struct arch *archs,
uint32_t narchs)
{
    uint32_t i;
#ifdef ALLOW_ARCHIVES
    uint32_t j, offset, size;
#endif
    struct object *object;

	for(i = 0; i < narchs; i++){
	    if(archs[i].type == OFILE_ARCHIVE){
#ifndef ALLOW_ARCHIVES
		error_arch(archs + i, NULL, "input file must be a linked "
			   "Mach-O file not an archive: ");
		return;
#else /* defined(ALLOW_ARCHIVES) */
		for(j = 0; j < archs[i].nmembers; j++){
		    if(archs[i].members[j].type == OFILE_Mach_O){
			object = archs[i].members[j].object;
			if(check_object(archs + i, object) == FALSE)
			    return;
			if(rflag || (object->seg_bitcode == NULL &&
				     object->seg_bitcode64 == NULL))
			    strip_bitcode_segment(archs + i, object);
			else
			    leave_just_bitcode_segment(archs + i, object);
		    }
		}
		/*
		 * Reset the library offsets and size.
		 */
		offset = 0;
		for(j = 0; j < archs[i].nmembers; j++){
		    archs[i].members[j].offset = offset;
		    size = 0;
		    if(archs[i].members[j].member_long_name == TRUE){
			size = rnd(archs[i].members[j].member_name_size,
				     sizeof(int32_t));
			archs[i].toc_long_name = TRUE;
		    }
		    if(archs[i].members[j].object != NULL){
			size += archs[i].members[j].object->object_size
			   - archs[i].members[j].object->input_sym_info_size
			   + archs[i].members[j].object->output_sym_info_size;
			sprintf(archs[i].members[j].ar_hdr->ar_size, "%-*ld",
			       (int)sizeof(archs[i].members[j].ar_hdr->ar_size),
			       (long)(size));
			/*
			 * This has to be done by hand because sprintf puts a
			 * null at the end of the buffer.
			 */
			memcpy(archs[i].members[j].ar_hdr->ar_fmag, ARFMAG,
			      (int)sizeof(archs[i].members[j].ar_hdr->ar_fmag));
		    }
		    else{
			size += archs[i].members[j].unknown_size;
		    }
		    offset += sizeof(struct ar_hdr) + size;
		}
		archs[i].library_size = offset;
#endif /* defined(ALLOW_ARCHIVES) */
	    }
	    else if(archs[i].type == OFILE_Mach_O){
		object = archs[i].object;
		if(check_object(archs +i, object) == FALSE)
		    return;
		if(rflag || mflag || (object->seg_bitcode == NULL &&
			              object->seg_bitcode64 == NULL))
		    strip_bitcode_segment(archs + i, object);
		else
		    leave_just_bitcode_segment(archs + i, object);
	    }
	}
}

/*
 * check_object() checks to make sure the object is one that can processed for
 * stripping the bitcode segment or just leaving only the bitcode segment.
 * This must be a fully linked Mach-O file for use with the dynamic linker.
 * And if built for the Watch OS it must have a bitcode segment.
 * And the bitcode segment must not have any relocation entries or symbols
 * defined it its sections.  And its sections must be of type S_REGULAR.
 */
static
enum bool
check_object(
struct arch *arch,
struct object *object)
{
    uint32_t i, mh_ncmds, mh_flags;
    struct load_command *lc;
    struct segment_command *sg;
    struct segment_command_64 *sg64;
    uint32_t section_ordinal, first_bitcode_section_ordinal,
	     last_bitcode_section_ordinal;
    struct nlist *symbols;
    struct nlist_64 *symbols64;
    struct section *s;
    struct section_64 *s64;

        if(arch->object->mh != NULL){
            mh_ncmds = arch->object->mh->ncmds;
	    mh_flags = object->mh->flags;
	}
	else{
            mh_ncmds = arch->object->mh64->ncmds;
	    mh_flags = object->mh64->flags;
	}

	if((mh_flags & MH_DYLDLINK) != MH_DYLDLINK)
	    fatal_arch(arch, NULL, "can't be used on a file not built for use "
		       "with the dynamic linker: ");

	/*
	 * If it has a bitcode segment it can't have any relocation entries.
	 *
	 * The SG_NORELOC segment flag should have been set for the bitcode
	 * segments created with ld(1)'s -sectcreate option but are currently
	 * not.
	 *
	 * To check this in fully linked images one would have to look in
	 * the dyld info for any binding from these sections.  Older systems
	 * had these off the LC_DYSYMTAB in nextrel and nlocrel but likely
	 * binaries from older system would not be used with this program.
	 */

	/*
	 * If it has a bitcode segment it can't have any symbols defined in
	 * the sections of that segment.
	 */
	if(object->seg_bitcode != NULL || object->seg_bitcode64 != NULL){
	    section_ordinal = 1;
	    first_bitcode_section_ordinal = 0;
	    lc = arch->object->load_commands;
	    for(i = 0; i < mh_ncmds && first_bitcode_section_ordinal == 0; i++){
		if(lc->cmd == LC_SEGMENT){
		    sg = (struct segment_command *)lc;
		    if(sg == object->seg_bitcode && sg->nsects > 0){
			first_bitcode_section_ordinal = section_ordinal;
			last_bitcode_section_ordinal = section_ordinal +
						       sg->nsects;
		    }
		    section_ordinal += sg64->nsects;
		}
		else if(lc->cmd == LC_SEGMENT_64){
		    sg64 = (struct segment_command_64 *)lc;
		    if(sg64 == object->seg_bitcode64 && sg64->nsects > 0){
			first_bitcode_section_ordinal = section_ordinal;
			last_bitcode_section_ordinal = section_ordinal +
						       sg64->nsects;
		    }
		    section_ordinal += sg64->nsects;
		}
		lc = (struct load_command *)((char *)lc + lc->cmdsize);
	    }
	    if(first_bitcode_section_ordinal != 0){
		if(object->seg_bitcode != NULL){
		    symbols = (struct nlist *)
		              (object->object_addr + object->st->symoff);
		    for(i = 0; i < object->st->nsyms; i++){
			if((symbols[i].n_type & N_TYPE) == N_SECT &&
			   symbols[i].n_sect >= first_bitcode_section_ordinal &&
			   symbols[i].n_sect < last_bitcode_section_ordinal){
	    		    fatal_arch(arch, NULL, "bitcode segment can't have "
				"symbols defined it its sections in: ");
			}
		    }
		}
		else{
		    symbols64 = (struct nlist_64 *)
		                (object->object_addr + object->st->symoff);
		    for(i = 0; i < object->st->nsyms; i++){
			if((symbols64[i].n_type & N_TYPE) == N_SECT &&
			   symbols64[i].n_sect >=
				first_bitcode_section_ordinal &&
			   symbols64[i].n_sect < last_bitcode_section_ordinal){
	    		    fatal_arch(arch, NULL, "bitcode segment can't have "
				"symbols defined it its sections in: ");
			}
		    }
		}
	    }
	}

	/*
	 * Also all the sections in the bitcode segment should be of type
	 * S_REGULAR.
	 */
	if(object->seg_bitcode != NULL){
	    s = (struct section *)((char *)object->seg_bitcode +
				   sizeof(struct segment_command));
	    for(i = 0; i < object->seg_bitcode->nsects; i++){
		if((s->flags & SECTION_TYPE) != S_REGULAR)
		    fatal_arch(arch, NULL, "bitcode segment can't have "
			"sections that are not of type S_REGULAR in: ");
		s++;
	    }
	}
	else if(object->seg_bitcode64 != NULL){
	    s64 = (struct section_64 *)((char *)object->seg_bitcode64 +
				        sizeof(struct segment_command_64));
	    for(i = 0; i < object->seg_bitcode64->nsects; i++){
		if((s64->flags & SECTION_TYPE) != S_REGULAR)
		    fatal_arch(arch, NULL, "bitcode segment can't have "
			"sections that are not of type S_REGULAR in: ");
		s64++;
	    }
	}

	/*
	 * If the lflag is used on a file without a bitcode segment then
	 * then the code in process() will call strip_bitcode_segment() and
	 * not leave_just_bitcode_segment() and strip_bitcode_segment() will
	 * just pass the object through essentially unchanged.
	 */
#if 0
	if(lflag == TRUE &&
	   object->seg_bitcode == NULL && object->seg_bitcode64 == NULL)
	    fatal_arch(arch, NULL, "-l to leave only bitcode segment can't be "
		       "used on a file without a __LLVM segment: ");
#endif

	return(TRUE);
}

/*
 * strip_bitcode_segment() is called when there is a bitcode segment or not.
 * If there is a bitcode segment it is removed and the offsets are adjusted.
 * In this case the code signature is also removed.  It has been previously
 * checked that the bitcode segment is directly before the link edit segment. 
 *
 * This is also called when we want to remove the bitcode segment and leave
 * just a marker, the -m flag.  In this case we leave an __LLVM segment with
 * the first section with just one zero byte and the __LLVM segment the size of
 * the segment rounding.
 *
 * If there is no bitcode segment the linkedit information including the
 * code signature info is set up to be written out so this arch is essentially
 * unchanged.
 */
static
void
strip_bitcode_segment(
struct arch *arch,
struct object *object)
{
    uint32_t start_offset, offset, end_of_string_table, alignment_padding;
    uint32_t dyld_info_start;
    uint32_t dyld_info_end;
    enum bool has_bitcode;

    const struct arch_flag *arch_flag;
    uint32_t i, segalign, bitcode_marker_size, sect_offset;
    struct section *s;
    struct section_64 *s64;

	/*
	 * If we are removing the bitcode segment and leaving just a marker
	 * calculate a minimum sized segment contents with all zeros which
	 * usually will be the segment alignment.
	 */
	if(mflag){
	    arch_flag = get_arch_family_from_cputype(object->mh_cputype);
	    if(arch_flag != NULL)
		segalign = get_segalign_from_flag(arch_flag);
	    else
		segalign = 0x4000; /* 16K */
	}

	/*
	 * To get the right amount of the start of the file copied out by
	 * writeout() before the symbolic information for this case when we
	 * are stripping out the bitcode segment we reduce the object size by
	 * the size of the bitcode segment which is just before the linkedit
	 * segment.  Then this size minus the size of the input symbolic
	 * information is copied out from the input file to the output file.
	 *
	 * This routine also handles objects without bitcode segments and
	 * just passes the object through essentially unchanged in this case.
	 *
	 * Also adjust the file offsets of the link edit information which is
	 * where the output symbolic information will start in the output file.
	 */
	has_bitcode = FALSE;
	if(object->mh != NULL){
	    if(object->seg_bitcode) {
		has_bitcode = TRUE;
		object->object_size -= object->seg_bitcode->filesize;
		object->seg_linkedit->fileoff -= object->seg_bitcode->filesize;
	    }
	    object->input_sym_info_size = object->seg_linkedit->filesize;
	    start_offset = object->seg_linkedit->fileoff;

	    /*
	     * If we have bitcode and are replacing it with just a marker
	     * then set up the segment and first section to point to the
	     * minimum sized zero'ed out segment.  The first section is to be 1
	     * byte in size and the rest are zero sized.
	     */
	    if(has_bitcode && mflag){
		if(object->seg_bitcode->filesize >= segalign)
		    bitcode_marker_size = segalign;
		else
		    bitcode_marker_size = object->seg_bitcode->filesize;
		object->output_new_content = allocate(bitcode_marker_size);
		memset(object->output_new_content, '\0', bitcode_marker_size);
		object->output_new_content_size = bitcode_marker_size;

		object->seg_bitcode->filesize = bitcode_marker_size;
		object->seg_bitcode->fileoff = start_offset;
		/* Note we are leaving the vmsize unchanged */

		sect_offset = object->seg_bitcode->fileoff;
		s = (struct section *)((char *)object->seg_bitcode +
				       sizeof(struct segment_command));
		if(object->seg_bitcode->nsects > 0){
		    s->offset = sect_offset;
		    s->size = bitcode_marker_size > 0 ? 1 : 0;
		    s++;
		}
		for(i = 1; i < object->seg_bitcode->nsects; i++){
		    s->offset = 0;
		    s->size = 0;
		    s++;
		}

		object->seg_linkedit->fileoff += object->seg_bitcode->filesize;
		start_offset += object->seg_bitcode->filesize;
	    }
	}
	else{
	    if(object->seg_bitcode64 != NULL){
		has_bitcode = TRUE;
		object->object_size -= object->seg_bitcode64->filesize;
		object->seg_linkedit64->fileoff -=
		    object->seg_bitcode64->filesize;
	    }
	    object->input_sym_info_size = object->seg_linkedit64->filesize;
	    start_offset = object->seg_linkedit64->fileoff;

	    /*
	     * If we have bitcode and are replacing it with just a marker
	     * then set up the segment and first section to point to the
	     * minimum sized zero'ed out segment.  The first section is to be 1
	     * byte in size and the rest are zero sized.
	     */
	    if(has_bitcode && mflag){
		if(object->seg_bitcode64->filesize >= segalign)
		    bitcode_marker_size = segalign;
		else
		    bitcode_marker_size = object->seg_bitcode64->filesize;
		object->output_new_content = allocate(bitcode_marker_size);
		memset(object->output_new_content, '\0', bitcode_marker_size);
		object->output_new_content_size = bitcode_marker_size;

		object->seg_bitcode64->filesize = bitcode_marker_size;
		object->seg_bitcode64->fileoff = start_offset;
		/* Note we are leaving the vmsize unchanged */

		sect_offset = object->seg_bitcode64->fileoff;
		s64 = (struct section_64 *)((char *)object->seg_bitcode64 +
				           sizeof(struct segment_command_64));
		if(object->seg_bitcode64->nsects > 0){
		    s64->offset = sect_offset;
		    s64->size = bitcode_marker_size > 0 ? 1 : 0;
		    s64++;
		}
		for(i = 1; i < object->seg_bitcode64->nsects; i++){
		    s64->offset = 0;
		    s64->size = 0;
		    s64++;
		}

		object->seg_linkedit64->fileoff +=
		    object->seg_bitcode64->filesize;
		start_offset += object->seg_bitcode64->filesize;
	    }
	}
	if(has_bitcode)
	    some_slice_has_bitcode = TRUE;

	/*
	 * Now set up all the input symbolic info to be the output symbolic
	 * info except any code signature data which will be removed.
	 *
	 * Assign the offsets to the symbolic data in the proper order and
	 * increment the local variable offset by the size of each part if the
	 * the symbolic data.  The output_sym_info_size is then determined at
	 * the end as the difference of the local variable offset from the 
	 * local variable start_offset.
	 */
	offset = start_offset;

	/* The dyld info is first in symbolic info in the output file. */
	if(object->dyld_info != NULL){
	    /*
	     * There are five parts to the dyld info, but it will be copied as a
	     * single block of info in the output.
	     */
	    dyld_info_start = 0;
	    if (object->dyld_info->rebase_off != 0)
		dyld_info_start = object->dyld_info->rebase_off;
	    else if (object->dyld_info->bind_off != 0)
		dyld_info_start = object->dyld_info->bind_off;
	    else if (object->dyld_info->weak_bind_off != 0)
		dyld_info_start = object->dyld_info->weak_bind_off;
	    else if (object->dyld_info->lazy_bind_off != 0)
		dyld_info_start = object->dyld_info->lazy_bind_off;
	    else if (object->dyld_info->export_off != 0)
		dyld_info_start = object->dyld_info->export_off;
	    dyld_info_end = 0;
	    if (object->dyld_info->export_size != 0)
		dyld_info_end = object->dyld_info->export_off
		    + object->dyld_info->export_size;
	    else if (object->dyld_info->lazy_bind_size != 0)
		dyld_info_end = object->dyld_info->lazy_bind_off
		    + object->dyld_info->lazy_bind_size;
	    else if (object->dyld_info->weak_bind_size != 0)
		dyld_info_end = object->dyld_info->weak_bind_off
		    + object->dyld_info->weak_bind_size;
	    else if (object->dyld_info->bind_size != 0)
		dyld_info_end = object->dyld_info->bind_off
		    + object->dyld_info->bind_size;
	    else if (object->dyld_info->rebase_size != 0)
		dyld_info_end = object->dyld_info->rebase_off
		    + object->dyld_info->rebase_size;

	    object->output_dyld_info = object->object_addr + dyld_info_start; 
	    object->output_dyld_info_size = dyld_info_end - dyld_info_start;
	    if(object->dyld_info->rebase_off != 0){
		object->dyld_info->rebase_off = offset;
		offset += object->dyld_info->rebase_size;
	    }
	    if(object->dyld_info->bind_off != 0){
		object->dyld_info->bind_off = offset;
		offset += object->dyld_info->bind_size;
	    }
	    if(object->dyld_info->weak_bind_off != 0){
		object->dyld_info->weak_bind_off = offset;
		offset += object->dyld_info->weak_bind_size;
	    }
	    if(object->dyld_info->lazy_bind_off != 0){
		object->dyld_info->lazy_bind_off = offset;
		offset += object->dyld_info->lazy_bind_size;
	    }
	    if(object->dyld_info->export_off != 0){
		object->dyld_info->export_off = offset;
		offset += object->dyld_info->export_size;
	    }
	}

	/* Local relocation entries are next in the output. */
	if(object->dyst != NULL){
	    if(object->dyst->nlocrel != 0){
		object->output_loc_relocs = (struct relocation_info *)
		    (object->object_addr + object->dyst->locreloff);
		object->dyst->locreloff = offset;
		offset +=
		    object->dyst->nlocrel * sizeof(struct relocation_info);
	    }
	    else
		object->dyst->locreloff = 0;
	}

	if(object->split_info_cmd != NULL){
	    object->output_split_info_data = object->object_addr +
		object->split_info_cmd->dataoff;
	    object->output_split_info_data_size = 
		object->split_info_cmd->datasize;
	    object->split_info_cmd->dataoff = offset;
	    offset += object->split_info_cmd->datasize;
	}

	if(object->func_starts_info_cmd != NULL){
	    object->output_func_start_info_data = object->object_addr +
		object->func_starts_info_cmd->dataoff;
	    object->output_func_start_info_data_size = 
		object->func_starts_info_cmd->datasize;
	    object->func_starts_info_cmd->dataoff = offset;
	    offset += object->func_starts_info_cmd->datasize;
	}

	if(object->data_in_code_cmd != NULL){
	    object->output_data_in_code_info_data = object->object_addr +
		object->data_in_code_cmd->dataoff;
	    object->output_data_in_code_info_data_size = 
		object->data_in_code_cmd->datasize;
	    object->data_in_code_cmd->dataoff = offset;
	    offset += object->data_in_code_cmd->datasize;
	}

	if(object->code_sign_drs_cmd != NULL){
	    if(has_bitcode){
		/*
		 * We remove the code signature on output if we are removing
		 * a bitcode segment.
		 */
		if(object->mh != NULL)
		    object->seg_linkedit->filesize -=
			object->code_sign_drs_cmd->datasize;
		else
		    object->seg_linkedit64->filesize -=
			object->code_sign_drs_cmd->datasize;
		object->output_code_sign_drs_info_data = NULL;
		object->output_code_sign_drs_info_data_size = 0;
		object->code_sign_drs_cmd->dataoff = 0;
		object->code_sign_drs_cmd->datasize = 0;
	    }
	    else{
		object->output_code_sign_drs_info_data = object->object_addr +
		    object->code_sign_drs_cmd->dataoff;
		object->output_code_sign_drs_info_data_size = 
		    object->code_sign_drs_cmd->datasize;
		object->code_sign_drs_cmd->dataoff = offset;
		offset += object->code_sign_drs_cmd->datasize;
	    }
	}

	if(object->link_opt_hint_cmd != NULL){
	    object->output_link_opt_hint_info_data = object->object_addr +
		object->link_opt_hint_cmd->dataoff;
	    object->output_link_opt_hint_info_data_size = 
		object->link_opt_hint_cmd->datasize;
	    object->link_opt_hint_cmd->dataoff = offset;
	    offset += object->link_opt_hint_cmd->datasize;
	}

	if(object->st != NULL && object->st->nsyms != 0){
	    if(object->mh != NULL){
		object->output_symbols = (struct nlist *)
		    (object->object_addr + object->st->symoff);
		if(object->object_byte_sex != get_host_byte_sex())
		    swap_nlist(object->output_symbols,
			       object->st->nsyms,
			       get_host_byte_sex());
		object->output_symbols64 = NULL;
	    }
	    else{
		object->output_symbols64 = (struct nlist_64 *)
		    (object->object_addr + object->st->symoff);
		if(object->object_byte_sex != get_host_byte_sex())
		    swap_nlist_64(object->output_symbols64,
				  object->st->nsyms,
				  get_host_byte_sex());
		object->output_symbols = NULL;
	    }
	    object->output_nsymbols = object->st->nsyms;
	    object->st->symoff = offset;
	    if(object->mh != NULL)
		offset += object->st->nsyms * sizeof(struct nlist);
	    else
		offset += object->st->nsyms * sizeof(struct nlist_64);
	}
	else if(object->st != NULL && object->st->nsyms == 0)
	    object->st->symoff = 0;

	if(object->hints_cmd != NULL){
	    if(object->hints_cmd->nhints != 0){
		object->output_hints = (struct twolevel_hint *)
		    (object->object_addr + object->hints_cmd->offset);
		object->hints_cmd->offset = offset;
		offset += object->hints_cmd->nhints *
			  sizeof(struct twolevel_hint);
	    }
	    else
		object->hints_cmd->offset = 0;
	}

	// Note that this should always be true in objects this program
	// operates on as it does not need to work on staticly linked images.
	if(object->dyst != NULL){
	    object->output_ilocalsym = object->dyst->ilocalsym;
	    object->output_nlocalsym = object->dyst->nlocalsym;
	    object->output_iextdefsym = object->dyst->iextdefsym;
	    object->output_nextdefsym = object->dyst->nextdefsym;
	    object->output_iundefsym = object->dyst->iundefsym;
	    object->output_nundefsym = object->dyst->nundefsym;

	    /* Note local relocation entries are above. */

	    if(object->dyst->nextrel != 0){
		object->output_ext_relocs = (struct relocation_info *)
		    (object->object_addr + object->dyst->extreloff);
		object->dyst->extreloff = offset;
		offset += object->dyst->nextrel *
			  sizeof(struct relocation_info);
	    }
	    else
		object->dyst->extreloff = 0;

	    if(object->dyst->nindirectsyms != 0){
		object->output_indirect_symtab = (uint32_t *)
		    (object->object_addr + object->dyst->indirectsymoff);
		object->dyst->indirectsymoff = offset;
		offset += object->dyst->nindirectsyms * sizeof(uint32_t) +
		          object->input_indirectsym_pad;
	    }
	    else
		object->dyst->indirectsymoff = 0;

	    if(object->dyst->ntoc != 0){
		object->output_tocs =
		    (struct dylib_table_of_contents *)
		    (object->object_addr + object->dyst->tocoff);
		object->output_ntoc = object->dyst->ntoc;
		object->dyst->tocoff = offset;
		offset += object->dyst->ntoc *
			  sizeof(struct dylib_table_of_contents);
	    }
	    else
		object->dyst->tocoff = 0;

	    if(object->dyst->nmodtab != 0){
		if(object->mh != NULL){
		    object->output_mods = (struct dylib_module *)
			(object->object_addr + object->dyst->modtaboff);
		    object->output_mods64 = NULL;
		}
		else{
		    object->output_mods64 = (struct dylib_module_64 *)
			(object->object_addr + object->dyst->modtaboff);
		    object->output_mods = NULL;
		}
		object->output_nmodtab = object->dyst->nmodtab;
		object->dyst->modtaboff = offset;
		if(object->mh != NULL)
		    offset += object->dyst->nmodtab *
			      sizeof(struct dylib_module);
		else
		    offset += object->dyst->nmodtab *
			      sizeof(struct dylib_module_64);
	    }
	    else
		object->dyst->modtaboff = 0;

	    if(object->dyst->nextrefsyms != 0){
		object->output_refs = (struct dylib_reference *)
		    (object->object_addr + object->dyst->extrefsymoff);
		object->output_nextrefsyms = object->dyst->nextrefsyms;
		object->dyst->extrefsymoff = offset;
		offset += object->dyst->nextrefsyms  *
			  sizeof(struct dylib_reference);
	    }
	    else
		object->dyst->extrefsymoff = 0;
	}

	if(object->st != NULL && object->st->strsize != 0){
	    end_of_string_table = object->st->stroff + object->st->strsize;
	    object->output_strings = object->object_addr + object->st->stroff;
	    object->output_strings_size = object->st->strsize;
	    object->st->stroff = offset;
	    offset += object->st->strsize;
	}
	else{
	    end_of_string_table = 0;
	    object->st->stroff = 0;
	}

	/* The code signature if any is last, after the strings. */
	if(object->code_sig_cmd != NULL){
	    if(has_bitcode){
		/*
		 * We remove the code signature on output if we are removing
		 * a bitcode segment.
		 */
		if(end_of_string_table != 0)
		    alignment_padding = object->code_sig_cmd->dataoff -
					end_of_string_table;
		else
		    alignment_padding = 0;
		if(object->mh != NULL)
		    object->seg_linkedit->filesize -=
			object->code_sig_cmd->datasize + alignment_padding;
		else
		    object->seg_linkedit64->filesize -=
			object->code_sig_cmd->datasize + alignment_padding;
		object->output_code_sig_data = NULL;
		object->output_code_sig_data_size = 0;
	    }
	    else{
		object->output_code_sig_data = object->object_addr +
		    object->code_sig_cmd->dataoff;
		object->output_code_sig_data_size = 
		    object->code_sig_cmd->datasize;
		offset = rnd(offset, 16);
		object->code_sig_cmd->dataoff = offset;
		offset += object->code_sig_cmd->datasize;
	    }
	}

	object->output_sym_info_size = offset - start_offset;

	if(has_bitcode)
	    strip_bitcode_from_load_commands(arch, object);
}

/*
 * These are the eight bytes of zeros for the fake string table to be
 * written out.
 */
static char fake_string_table[8];

/*
 * leave_just_bitcode_segment() is only called when there is a bitcode segment,
 * and removes everything but that segment contents but leaving the load
 * commands intact but zeros out the sizes and counts to make the resulting
 * Mach-O file valid.
 */
static
void
leave_just_bitcode_segment(
struct arch *arch,
struct object *object)
{
    uint32_t i, start_offset, offset, sect_offset;
    struct load_command *lc;
    struct segment_command *sg;
    struct segment_command_64 *sg64;
    struct section *s;
    struct section_64 *s64;

	/*
	 * To get the right amount of the start of the file copied out by
	 * writeout() before the symbolic information for this case when we
	 * are leaving only the bitcode segment we reduce the object size by
	 * the size of the section contents excluding the padding after the
	 * load commands.  Then this size minus the size of the input symbolic
	 * information is copied out from the input file to the output file.
	 * Which is just the size of the old load commands.
	 *
	 * To get the bit code segment into the output file we use the
	 * output_new_content and output_new_content_size.
	 *
	 * Lastly to fake up the output symbolic information will have a 8-byte
	 * string table of zeros and set the size of all the counts to zero
	 * but leave the load commands so other tools should still be happy.
	 *
	 * Also adjust the file offset of the link edit information which is
	 * where the new fake output symbolic information will start in the
	 * output file. Which will be right after the load commands.
	 */
	if(object->mh != NULL){
	    start_offset = 0;
	    lc = arch->object->load_commands;
	    for(i = 0; i < arch->object->mh->ncmds && start_offset == 0; i++){
		if(lc->cmd == LC_SEGMENT){
		    sg = (struct segment_command *)lc;
		    if(sg->filesize != 0 && sg->fileoff == 0){
			s = (struct section *)((char *)sg +
					       sizeof(struct segment_command));
			if(sg->nsects > 0)
			    start_offset = s->offset;
		    }
		}
		lc = (struct load_command *)((char *)lc + lc->cmdsize);
	    }
	    if(start_offset == 0)
		start_offset = sizeof(struct mach_header) +
			       object->mh->sizeofcmds;

	    object->object_size -= (object->seg_linkedit->fileoff -
				    start_offset);

	    object->output_new_content = object->object_addr +
					 object->seg_bitcode->fileoff;
	    object->output_new_content_size = object->seg_bitcode->filesize;

	    object->seg_bitcode->fileoff = start_offset;
	    sect_offset = object->seg_bitcode->fileoff;
	    s = (struct section *)((char *)object->seg_bitcode +
				   sizeof(struct segment_command));
	    for(i = 0; i < object->seg_bitcode->nsects; i++){
		s->offset = sect_offset;
		sect_offset += s->offset;
		s++;
	    }
	    start_offset += object->seg_bitcode->filesize;

	    object->input_sym_info_size = object->seg_linkedit->filesize;
	    object->seg_linkedit->fileoff = start_offset;
	}
	else{
	    start_offset = 0;
	    lc = arch->object->load_commands;
	    for(i = 0; i < arch->object->mh64->ncmds && start_offset == 0; i++){
		if(lc->cmd == LC_SEGMENT_64){
		    sg64 = (struct segment_command_64 *)lc;
		    if(sg64->filesize != 0 && sg64->fileoff == 0){
			s64 = (struct section_64 *)((char *)sg64 +
					    sizeof(struct segment_command_64));
			if(sg64->nsects > 0)
			    start_offset = s64->offset;
		    }
		}
		lc = (struct load_command *)((char *)lc + lc->cmdsize);
	    }
	    if(start_offset == 0)
		start_offset = sizeof(struct mach_header_64) +
			       object->mh64->sizeofcmds;

	    object->object_size -= (object->seg_linkedit64->fileoff -
				    start_offset);

	    object->output_new_content = object->object_addr +
					 object->seg_bitcode64->fileoff;
	    object->output_new_content_size = object->seg_bitcode64->filesize;

	    object->seg_bitcode64->fileoff = start_offset;
	    sect_offset = object->seg_bitcode64->fileoff;
	    s64 = (struct section_64 *)((char *)object->seg_bitcode64 +
				        sizeof(struct segment_command_64));
	    for(i = 0; i < object->seg_bitcode64->nsects; i++){
		s64->offset = sect_offset;
		sect_offset += s64->offset;
		s64++;
	    }
	    start_offset += object->seg_bitcode64->filesize;

	    object->input_sym_info_size = object->seg_linkedit64->filesize;
	    object->seg_linkedit64->fileoff = start_offset;
	}

	/*
	 * Now "remove" all the input symbolic info so it won't be the output
	 * symbolic info except for a fake 8 byte string table.
	 *
	 * Assign the offsets to the symbolic data in the proper order and
	 * increment the local variable offset by the size of each part if the
	 * the symbolic data.  The output_sym_info_size is then determined at
	 * the end as the difference of the local variable offset from the 
	 * local variable start_offset.
	 */
	offset = start_offset;

	if(object->dyld_info != NULL){
	    object->output_dyld_info = NULL;
	    object->output_dyld_info_size = 0;
	    object->dyld_info->rebase_off = 0;
	    object->dyld_info->rebase_size = 0;
	    object->dyld_info->bind_off = 0;
	    object->dyld_info->bind_size = 0;
	    object->dyld_info->weak_bind_off = 0;
	    object->dyld_info->weak_bind_size = 0;
	    object->dyld_info->lazy_bind_off = 0;
	    object->dyld_info->lazy_bind_size = 0;
	    object->dyld_info->export_off = 0;
	    object->dyld_info->export_size = 0;
	}

	/* Local relocation entries are next in the output. */
	if(object->dyst != NULL){
	    object->output_loc_relocs = NULL;
	    object->dyst->locreloff = 0;
	    object->dyst->nlocrel = 0;
	}

	if(object->split_info_cmd != NULL){
	    object->output_split_info_data = NULL;
	    object->output_split_info_data_size = 0;
	    object->split_info_cmd->dataoff = 0;
	    object->split_info_cmd->datasize = 0;
	}

	if(object->func_starts_info_cmd != NULL){
	    object->output_func_start_info_data = NULL;
	    object->output_func_start_info_data_size = 0;
	    object->func_starts_info_cmd->dataoff = 0;
	    object->func_starts_info_cmd->datasize = 0;
	}

	if(object->data_in_code_cmd != NULL){
	    object->output_data_in_code_info_data = NULL;
	    object->output_data_in_code_info_data_size = 0;
	    object->data_in_code_cmd->dataoff = 0;
	    object->data_in_code_cmd->datasize = 0;
	}

	if(object->code_sign_drs_cmd != NULL){
	    object->output_code_sign_drs_info_data = NULL;
	    object->output_code_sign_drs_info_data_size = 0;
	    object->code_sign_drs_cmd->dataoff = 0;
	    object->code_sign_drs_cmd->datasize = 0;
	}

	if(object->link_opt_hint_cmd != NULL){
	    object->output_link_opt_hint_info_data = NULL;
	    object->output_link_opt_hint_info_data_size = 0;
	    object->link_opt_hint_cmd->dataoff = 0;
	    object->link_opt_hint_cmd->datasize = 0;
	}

	if(object->st != NULL){
	    object->output_symbols = NULL;
	    object->output_symbols64 = NULL;
	    object->output_nsymbols = 0;
	    object->st->nsyms = 0;
	    object->st->symoff = 0;
	}

	if(object->hints_cmd != NULL){
	    object->output_hints = NULL;
	    object->hints_cmd->offset = 0;
	    object->hints_cmd->nhints = 0;
	}

	// Note that this should always be true in objects this program
	// operates on as it does not need to work on staticly linked images.
	if(object->dyst != NULL){
	    object->output_ilocalsym = 0;
	    object->output_nlocalsym = 0;
	    object->output_iextdefsym = 0;
	    object->output_nextdefsym = 0;
	    object->output_iundefsym = 0;
	    object->output_nundefsym = 0;
	    object->dyst->ilocalsym = 0;
	    object->dyst->nlocalsym = 0;
	    object->dyst->iextdefsym = 0;
	    object->dyst->nextdefsym = 0;
	    object->dyst->iundefsym = 0;
	    object->dyst->nundefsym = 0;

	    object->output_ext_relocs = NULL;
	    object->dyst->nextrel = 0;
	    object->dyst->extreloff = 0;

	    object->output_indirect_symtab = NULL;
	    object->dyst->nindirectsyms = 0;
	    object->dyst->indirectsymoff = 0;

	    object->output_tocs = NULL;
	    object->output_ntoc = 0;
	    object->dyst->tocoff = 0;

	    object->output_mods = NULL;
	    object->output_mods64 = NULL;
	    object->output_nmodtab = 0;
	    object->dyst->modtaboff = 0;

	    object->output_refs = NULL;
	    object->output_nextrefsyms = 0;
	    object->dyst->extrefsymoff = 0;
	}

	if(object->st != NULL){
	    object->output_strings = fake_string_table;
	    object->output_strings_size = sizeof(fake_string_table);
	    object->st->stroff = offset;
	    object->st->strsize = sizeof(fake_string_table);
	    offset += sizeof(fake_string_table);
	}

	/* The code signature if any is last, after the strings. */
	if(object->code_sig_cmd != NULL){
	    object->output_code_sig_data = NULL;
	    object->output_code_sig_data_size = 0;
	}

	object->output_sym_info_size = offset - start_offset;
	if(object->mh != NULL)
	    object->seg_linkedit->filesize = object->output_sym_info_size;
	else
	    object->seg_linkedit64->filesize = object->output_sym_info_size;

	leave_only_bitcode_load_commands(arch, object);
}

/*
 * strip_bitcode_segment_command() is called when -r is specified to remove
 * the LC_SEGMENT or LC_SEGMENT_64 load command from the object's load commands
 * for the bitcode segment and any LC_CODE_SIGNATURE and LC_DYLIB_CODE_SIGN_DRS
 * load commands.
 *
 * If we are using the -m flag to remove the bitcode and leave a marker then
 * the LC_SEGMENT or LC_SEGMENT_64 load command are not removed.
 */
static
void
strip_bitcode_from_load_commands(
struct arch *arch,
struct object *object)
{
    uint32_t i, mh_ncmds, new_ncmds, mh_sizeofcmds, new_sizeofcmds;
    struct load_command *lc1, *lc2, *new_load_commands;
    struct segment_command *sg;
    struct segment_command_64 *sg64;

	/*
	 * A check of the existance of the bitcode segment has already been
	 * done so it must be present at this point.
	 */

	/*
	 * Allocate space for the new load commands and zero it out so any holes
	 * will be zero bytes.
	 */
        if(arch->object->mh != NULL){
            mh_ncmds = arch->object->mh->ncmds;
	    mh_sizeofcmds = arch->object->mh->sizeofcmds;
	}
	else{
            mh_ncmds = arch->object->mh64->ncmds;
	    mh_sizeofcmds = arch->object->mh64->sizeofcmds;
	}
	new_load_commands = allocate(mh_sizeofcmds);
	memset(new_load_commands, '\0', mh_sizeofcmds);

	/*
	 * Copy all the load commands except the LC_SEGMENT or LC_SEGMENT_64 for
	 * bitcode segment into the allocated space for the new load commands.
	 * Unless the -m flag is specified then do copy the bitcode segment
	 * load command.
	 */
	lc1 = arch->object->load_commands;
	lc2 = new_load_commands;
	new_ncmds = 0;
	new_sizeofcmds = 0;
	for(i = 0; i < mh_ncmds; i++){
	    if(lc1->cmd == LC_SEGMENT){
		sg = (struct segment_command *)lc1;
		if(strcmp(sg->segname, "__LLVM") != 0 || mflag){
		    memcpy(lc2, lc1, lc1->cmdsize);
		    new_ncmds++;
		    new_sizeofcmds += lc2->cmdsize;
		    lc2 = (struct load_command *)((char *)lc2 + lc2->cmdsize);
		}
	    }
	    else if(lc1->cmd == LC_SEGMENT_64){
		sg64 = (struct segment_command_64 *)lc1;
		if(strcmp(sg64->segname, "__LLVM") != 0 || mflag){
		    memcpy(lc2, lc1, lc1->cmdsize);
		    new_ncmds++;
		    new_sizeofcmds += lc2->cmdsize;
		    lc2 = (struct load_command *)((char *)lc2 + lc2->cmdsize);
		}
	    }
	    else if(lc1->cmd != LC_CODE_SIGNATURE &&
		    lc1->cmd != LC_DYLIB_CODE_SIGN_DRS){
		memcpy(lc2, lc1, lc1->cmdsize);
		new_ncmds++;
		new_sizeofcmds += lc2->cmdsize;
		lc2 = (struct load_command *)((char *)lc2 + lc2->cmdsize);
	    }
	    lc1 = (struct load_command *)((char *)lc1 + lc1->cmdsize);
	}

	/*
	 * Finally copy the updated load commands over the existing load
	 * commands.
	 */
	memcpy(arch->object->load_commands, new_load_commands, new_sizeofcmds);
	if(mh_sizeofcmds > new_sizeofcmds)
		memset((char *)arch->object->load_commands + new_sizeofcmds,
		       '\0', (mh_sizeofcmds - new_sizeofcmds));
        if(arch->object->mh != NULL) {
            arch->object->mh->sizeofcmds = new_sizeofcmds;
            arch->object->mh->ncmds = new_ncmds;
        } else {
            arch->object->mh64->sizeofcmds = new_sizeofcmds;
            arch->object->mh64->ncmds = new_ncmds;
        }
	free(new_load_commands);

	/* reset the pointers into the load commands */
	reset_pointers_for_object_load_commands(arch, object);

	/*
	 * The LC_CODE_SIGNATURE and LC_DYLIB_CODE_SIGN_DRS load commands
	 * if any were removed above.
	 */
	object->code_sig_cmd = NULL;
	object->code_sign_drs_cmd = NULL;
}

/*
 * leave_only_bitcode_load_commands() is called when -l is specified to leave
 * the LC_SEGMENT or LC_SEGMENT_64 load command from the object's load commands
 * for the bitcode segment.  It only removes the LC_CODE_SIGNATURE and 
 * LC_DYLIB_CODE_SIGN_DRS load commands.  And zeros out all the fields of
 * other load commands to make them valid in the Mach-O file.
 */
static
void
leave_only_bitcode_load_commands(
struct arch *arch,
struct object *object)
{
    uint32_t i, j, mh_ncmds, new_ncmds, mh_sizeofcmds, new_sizeofcmds;
    struct load_command *lc1, *lc2, *new_load_commands;
    struct segment_command *sg;
    struct segment_command_64 *sg64;
    struct section *s;
    struct section_64 *s64;
    struct entry_point_command *ep;

	/*
	 * Allocate space for the new load commands and zero it out so any holes
	 * will be zero bytes.
	 */
        if(arch->object->mh != NULL){
            mh_ncmds = arch->object->mh->ncmds;
	    mh_sizeofcmds = arch->object->mh->sizeofcmds;
	}
	else{
            mh_ncmds = arch->object->mh64->ncmds;
	    mh_sizeofcmds = arch->object->mh64->sizeofcmds;
	}
	new_load_commands = allocate(mh_sizeofcmds);
	memset(new_load_commands, '\0', mh_sizeofcmds);

	/*
	 * Copy all the load commands except the LC_CODE_SIGNATURE and the
	 * LC_DYLIB_CODE_SIGN_DRS.  For the segment commands other than the
	 * bitcode segment and linkedit segment zero out the fields.
	 */
	lc1 = arch->object->load_commands;
	lc2 = new_load_commands;
	new_ncmds = 0;
	new_sizeofcmds = 0;
	for(i = 0; i < mh_ncmds; i++){
	    if(lc1->cmd == LC_SEGMENT){
		sg = (struct segment_command *)lc1;
		if(strcmp(sg->segname, "__LLVM") != 0 &&
		   strcmp(sg->segname, SEG_LINKEDIT) != 0){
		    sg->vmaddr = 0;
		    sg->vmsize = 0;
		    sg->fileoff = 0;
		    sg->filesize = 0;
		    s = (struct section *)((char *)sg +
					   sizeof(struct segment_command));
		    for(j = 0; j < sg->nsects; j++){
			s->addr = 0;
			s->size = 0;
			s->offset = 0;
			s->reloff = 0;
			s->nreloc = 0;
			s->reserved1 = 0;
			s++;
		    }
		}
		memcpy(lc2, lc1, lc1->cmdsize);
		new_ncmds++;
		new_sizeofcmds += lc2->cmdsize;
		lc2 = (struct load_command *)((char *)lc2 + lc2->cmdsize);
	    }
	    else if(lc1->cmd == LC_SEGMENT_64){
		sg64 = (struct segment_command_64 *)lc1;
		if(strcmp(sg64->segname, "__LLVM") != 0 &&
		   strcmp(sg64->segname, SEG_LINKEDIT) != 0){
		    sg64->vmaddr = 0;
		    sg64->vmsize = 0;
		    sg64->fileoff = 0;
		    sg64->filesize = 0;
		    s64 = (struct section_64 *)((char *)sg64 +
					    sizeof(struct segment_command_64));
		    for(j = 0; j < sg64->nsects; j++){
			s64->addr = 0;
			s64->size = 0;
			s64->offset = 0;
			s64->reloff = 0;
			s64->nreloc = 0;
			s64->reserved1 = 0;
			s64++;
		    }
		}
		memcpy(lc2, lc1, lc1->cmdsize);
		new_ncmds++;
		new_sizeofcmds += lc2->cmdsize;
		lc2 = (struct load_command *)((char *)lc2 + lc2->cmdsize);
	    }
	    else if(lc1->cmd != LC_CODE_SIGNATURE &&
		    lc1->cmd != LC_DYLIB_CODE_SIGN_DRS){
		if(lc1->cmd == LC_MAIN){
		    ep = (struct entry_point_command *)lc1;
		    ep->entryoff = 0;
		}
		memcpy(lc2, lc1, lc1->cmdsize);
		new_ncmds++;
		new_sizeofcmds += lc2->cmdsize;
		lc2 = (struct load_command *)((char *)lc2 + lc2->cmdsize);
	    }
	    lc1 = (struct load_command *)((char *)lc1 + lc1->cmdsize);
	}

	/*
	 * Finally copy the updated load commands over the existing load
	 * commands.
	 */
	memcpy(arch->object->load_commands, new_load_commands, new_sizeofcmds);
	if(mh_sizeofcmds > new_sizeofcmds)
		memset((char *)arch->object->load_commands + new_sizeofcmds,
		       '\0', (mh_sizeofcmds - new_sizeofcmds));
        if(arch->object->mh != NULL) {
            arch->object->mh->sizeofcmds = new_sizeofcmds;
            arch->object->mh->ncmds = new_ncmds;
        } else {
            arch->object->mh64->sizeofcmds = new_sizeofcmds;
            arch->object->mh64->ncmds = new_ncmds;
        }
	free(new_load_commands);

	/* reset the pointers into the load commands */
	reset_pointers_for_object_load_commands(arch, object);

	/*
	 * The LC_CODE_SIGNATURE and LC_DYLIB_CODE_SIGN_DRS load commands
	 * if any were removed above.
	 */
	object->code_sig_cmd = NULL;
	object->code_sign_drs_cmd = NULL;
}

/*
 * reset_pointers_for_object_load_commands() sets the fields in the object
 * struct that are pointers to the load commands.  This is needed when we
 * rewrite the load commands making those fields that have pointers invalid.
 */
static
void
reset_pointers_for_object_load_commands(
struct arch *arch,
struct object *object)
{
    uint32_t i, mh_ncmds;
    struct load_command *lc;
    struct segment_command *sg;
    struct segment_command_64 *sg64;

        if(arch->object->mh != NULL)
            mh_ncmds = arch->object->mh->ncmds;
        else
            mh_ncmds = arch->object->mh64->ncmds;

	/* reset the pointers into the load commands */
	lc = arch->object->load_commands;
	for(i = 0; i < mh_ncmds; i++){
	    switch(lc->cmd){
	    case LC_SYMTAB:
		arch->object->st = (struct symtab_command *)lc;
	        break;
	    case LC_DYSYMTAB:
		arch->object->dyst = (struct dysymtab_command *)lc;
		break;
	    case LC_TWOLEVEL_HINTS:
		arch->object->hints_cmd = (struct twolevel_hints_command *)lc;
		break;
	    case LC_PREBIND_CKSUM:
		arch->object->cs = (struct prebind_cksum_command *)lc;
		break;
	    case LC_SEGMENT:
		sg = (struct segment_command *)lc;
		if(strcmp(sg->segname, SEG_LINKEDIT) == 0)
		    arch->object->seg_linkedit = sg;
		else if(strcmp(sg->segname, "__LLVM") == 0)
		    arch->object->seg_bitcode = sg;
		break;
	    case LC_SEGMENT_64:
		sg64 = (struct segment_command_64 *)lc;
		if(strcmp(sg64->segname, SEG_LINKEDIT) == 0)
		    arch->object->seg_linkedit64 = sg64;
		else if(strcmp(sg64->segname, "__LLVM") == 0)
		    arch->object->seg_bitcode64 = sg64;
		break;
	    case LC_SEGMENT_SPLIT_INFO:
		object->split_info_cmd = (struct linkedit_data_command *)lc;
		break;
	    case LC_FUNCTION_STARTS:
		object->func_starts_info_cmd =
					 (struct linkedit_data_command *)lc;
		break;
	    case LC_DATA_IN_CODE:
		object->data_in_code_cmd =
				         (struct linkedit_data_command *)lc;
		break;
	    case LC_LINKER_OPTIMIZATION_HINT:
		object->link_opt_hint_cmd =
				         (struct linkedit_data_command *)lc;
		break;
	    case LC_DYLD_INFO_ONLY:
	    case LC_DYLD_INFO:
		object->dyld_info = (struct dyld_info_command *)lc;
	    case LC_DYLIB_CODE_SIGN_DRS:
		object->code_sign_drs_cmd = (struct linkedit_data_command *)lc;
		break;
	    case LC_CODE_SIGNATURE:
		object->code_sig_cmd = (struct linkedit_data_command *)lc;
		break;
	    }
	    lc = (struct load_command *)((char *)lc + lc->cmdsize);
	}
}
