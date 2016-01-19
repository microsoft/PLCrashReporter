/*
 * Copyright (c) 2013 Plausible Labs Cooperative, Inc.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include "PLCrashFrameCompactUnwind.h"
#include "PLCrashAsyncCompactUnwindEncoding.h"
#include "PLCrashFeatureConfig.h"

#include <inttypes.h>

#if PLCRASH_FEATURE_UNWIND_DWARF

/**
 * Attempt to fetch next frame using compact frame unwinding data from @a image_list.
 *
 * @param task The task containing the target frame stack.
 * @param image_list The list of images loaded in the target @a task.
 * @param current_frame The current stack frame.
 * @param previous_frame The previous stack frame, or NULL if this is the first frame.
 * @param next_frame The new frame to be initialized.
 *
 * @return Returns PLFRAME_ESUCCESS on success, PLFRAME_ENOFRAME is no additional frames are available, or a standard plframe_error_t code if an error occurs.
 */
plframe_error_t plframe_cursor_read_compact_unwind (task_t task,
                                                    plcrash_async_image_list_t *image_list,
                                                    const plframe_stackframe_t *current_frame,
                                                    const plframe_stackframe_t *previous_frame,
                                                    plframe_stackframe_t *next_frame)
{
    plframe_error_t result;
    plcrash_error_t err;

    /* Fetch the IP. It should always be available */
    if (!plcrash_async_thread_state_has_reg(&current_frame->thread_state, PLCRASH_REG_IP)) {
        PLCF_DEBUG("Frame is missing a valid IP register, skipping compact unwind encoding");
        return PLFRAME_EBADFRAME;
    }
    plcrash_greg_t pc = plcrash_async_thread_state_get_reg(&current_frame->thread_state, PLCRASH_REG_IP);
    
    /* Find the corresponding image */
    plcrash_async_macho_t *image = plcrash_async_image_containing_address(image_list, pc);
    if (image == NULL) {
        PLCF_DEBUG("Could not find a loaded image for the current frame pc: 0x%" PRIx64, (uint64_t) pc);
        return PLFRAME_ENOTSUP;
    }
    
    /* Map the unwind section */
    plcrash_async_mobject_t unwind_mobj;
    err = plcrash_async_macho_map_section(image, SEG_TEXT, "__unwind_info", &unwind_mobj);
    if (err != PLCRASH_ESUCCESS) {
        if (err != PLCRASH_ENOTFOUND)
            PLCF_DEBUG("Could not map the compact unwind info section for image %s: %d", image->name, err);
        return PLFRAME_ENOTSUP;
    }

    /* Initialize the CFE reader. */
    cpu_type_t cputype = image->byteorder->swap32(image->header.cputype);
    plcrash_async_cfe_reader_t reader;

    err = plcrash_async_cfe_reader_init(&reader, &unwind_mobj, cputype);
    if (err != PLCRASH_ESUCCESS) {
        PLCF_DEBUG("Could not parse the compact unwind info section for image '%s': %d", image->name, err);
        return PLFRAME_EINVAL;
    }

    /* Find the encoding entry (if any) and free the reader */
    pl_vm_address_t function_base;
    uint32_t encoding;
    err = plcrash_async_cfe_reader_find_pc(&reader, pc - image->header_addr, &function_base, &encoding);
    plcrash_async_cfe_reader_free(&reader);
    if (err != PLCRASH_ESUCCESS) {
        PLCF_DEBUG("Did not find CFE entry for PC 0x%" PRIx64 ": %d", (uint64_t) pc, err);
        return PLFRAME_ENOTSUP;
    }
    
    /* Decode the entry */
    plcrash_async_cfe_entry_t entry;
    err = plcrash_async_cfe_entry_init(&entry, cputype, encoding);
    if (err != PLCRASH_ESUCCESS) {
        PLCF_DEBUG("Could not decode CFE encoding 0x%" PRIx32 " for PC 0x%" PRIx64 ": %d", encoding, (uint64_t) pc, err);
        return PLFRAME_ENOTSUP;
    }

    /* Skip entries for which no unwind information is unavailable */
    if (plcrash_async_cfe_entry_type(&entry) == PLCRASH_ASYNC_CFE_ENTRY_TYPE_NONE) {

        plcrash_async_cfe_entry_free(&entry);
        return PLFRAME_ENOFRAME;
    }
    
    /* Compute the in-core function address */
    pl_vm_address_t function_address;
    if (!plcrash_async_address_apply_offset(image->header_addr, function_base, &function_address)) {
        PLCF_DEBUG("The provided function base (0x%" PRIx64 ") plus header address (0x%" PRIx64 ") will overflow pl_vm_address_t",
                   (uint64_t) function_base, (uint64_t) image->header_addr);

        plcrash_async_cfe_entry_free(&entry);
        return PLFRAME_EINVAL;
    }

    /* Apply the frame delta -- this may fail. */
    if ((err = plcrash_async_cfe_entry_apply(task, function_address, &current_frame->thread_state, &entry, &next_frame->thread_state)) == PLCRASH_ESUCCESS) {
        result = PLFRAME_ESUCCESS;
    } else {
        PLCF_DEBUG("Failed to apply CFE encoding 0x%" PRIx32 " for PC 0x%" PRIx64 ": %d", encoding, (uint64_t) pc, err);
        result = PLFRAME_ENOFRAME;
    }

    plcrash_async_cfe_entry_free(&entry);
    return result;
}

#endif /* PLCRASH_FEATURE_UNWIND_DWARF */
