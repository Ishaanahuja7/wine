/*
 * Common HID report descriptor helpers
 *
 * Copyright 2021 Rémi Bernon for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "winternl.h"
#include "winioctl.h"
#include "hidusage.h"
#include "ddk/wdm.h"

#include "unix_private.h"

static BOOL hid_descriptor_append(struct hid_descriptor *desc, const BYTE *buffer, SIZE_T size)
{
    BYTE *tmp = desc->data;

    if (desc->size + size > desc->max_size)
    {
        desc->max_size = max(desc->max_size * 3 / 2, desc->size + size);
        desc->data = realloc(tmp, desc->max_size);
    }

    if (!desc->data)
    {
        free(tmp);
        return FALSE;
    }

    memcpy(desc->data + desc->size, buffer, size);
    desc->size += size;
    return TRUE;
}

#include "psh_hid_macros.h"

static BOOL hid_descriptor_append_usage(struct hid_descriptor *desc, USAGE usage)
{
    const BYTE template[] =
    {
        USAGE(2, usage),
    };

    return hid_descriptor_append(desc, template, sizeof(template));
}

BOOL hid_descriptor_begin(struct hid_descriptor *desc, USAGE usage_page, USAGE usage)
{
    const BYTE template[] =
    {
        USAGE_PAGE(2, usage_page),
        USAGE(2, usage),
        COLLECTION(1, Application),
            USAGE(1, 0),
    };

    memset(desc, 0, sizeof(*desc));
    return hid_descriptor_append(desc, template, sizeof(template));
}

BOOL hid_descriptor_end(struct hid_descriptor *desc)
{
    static const BYTE template[] =
    {
        END_COLLECTION,
    };

    return hid_descriptor_append(desc, template, sizeof(template));
}

void hid_descriptor_free(struct hid_descriptor *desc)
{
    free(desc->data);
}

BOOL hid_descriptor_add_buttons(struct hid_descriptor *desc, USAGE usage_page,
                                USAGE usage_min, USAGE usage_max)
{
    const USHORT count = usage_max - usage_min + 1;
    const BYTE template[] =
    {
        USAGE_PAGE(2, usage_page),
        USAGE_MINIMUM(2, usage_min),
        USAGE_MAXIMUM(2, usage_max),
        LOGICAL_MINIMUM(1, 0),
        LOGICAL_MAXIMUM(1, 1),
        PHYSICAL_MINIMUM(1, 0),
        PHYSICAL_MAXIMUM(1, 1),
        REPORT_COUNT(2, count),
        REPORT_SIZE(1, 1),
        INPUT(1, Data|Var|Abs),
    };
    const BYTE template_pad[] =
    {
        REPORT_COUNT(1, 8 - (count % 8)),
        REPORT_SIZE(1, 1),
        INPUT(1, Cnst|Var|Abs),
    };

    if (!hid_descriptor_append(desc, template, sizeof(template)))
        return FALSE;

    if ((count % 8) && !hid_descriptor_append(desc, template_pad, sizeof(template_pad)))
        return FALSE;

    return TRUE;
}

BOOL hid_descriptor_add_hatswitch(struct hid_descriptor *desc, INT count)
{
    const BYTE template[] =
    {
        USAGE_PAGE(1, HID_USAGE_PAGE_GENERIC),
        USAGE(1, HID_USAGE_GENERIC_HATSWITCH),
        LOGICAL_MINIMUM(1, 1),
        LOGICAL_MAXIMUM(1, 8),
        PHYSICAL_MINIMUM(1, 0),
        PHYSICAL_MAXIMUM(2, 8),
        REPORT_SIZE(1, 8),
        REPORT_COUNT(4, count),
        UNIT(1, 0x0e /* none */),
        INPUT(1, Data|Var|Abs|Null),
    };

    return hid_descriptor_append(desc, template, sizeof(template));
}

BOOL hid_descriptor_add_axes(struct hid_descriptor *desc, BYTE count, USAGE usage_page,
                             const USAGE *usages, BOOL rel, LONG min, LONG max)
{
    const BYTE template_begin[] =
    {
        USAGE_PAGE(1, usage_page),
        COLLECTION(1, Physical),
    };
    const BYTE template_end[] =
    {
        END_COLLECTION,
    };
    const BYTE template[] =
    {
        LOGICAL_MINIMUM(4, min),
        LOGICAL_MAXIMUM(4, max),
        PHYSICAL_MINIMUM(4, min),
        PHYSICAL_MAXIMUM(4, max),
        REPORT_SIZE(1, 32),
        REPORT_COUNT(1, count),
        INPUT(1, Data|Var|(rel ? Rel : Abs)),
    };
    int i;

    if (!hid_descriptor_append(desc, template_begin, sizeof(template_begin)))
        return FALSE;

    for (i = 0; i < count; i++)
    {
        if (!hid_descriptor_append_usage(desc, usages[i]))
            return FALSE;
    }

    if (!hid_descriptor_append(desc, template, sizeof(template)))
        return FALSE;

    if (!hid_descriptor_append(desc, template_end, sizeof(template_end)))
        return FALSE;

    return TRUE;
}

BOOL hid_descriptor_add_haptics(struct hid_descriptor *desc)
{
    static const BYTE template[] =
    {
        USAGE_PAGE(2, HID_USAGE_PAGE_VENDOR_DEFINED_BEGIN),
        USAGE(1, 0x01),
        /* padding */
        REPORT_COUNT(1, 0x02),
        REPORT_SIZE(1, 0x08),
        OUTPUT(1, Data|Var|Abs),
        /* actuators */
        LOGICAL_MINIMUM(1, 0x00),
        LOGICAL_MAXIMUM(1, 0xff),
        PHYSICAL_MINIMUM(1, 0x00),
        PHYSICAL_MAXIMUM(1, 0xff),
        REPORT_SIZE(1, 0x08),
        REPORT_COUNT(1, 0x02),
        OUTPUT(1, Data|Var|Abs),
        /* padding */
        REPORT_COUNT(1, 0x02),
        REPORT_SIZE(1, 0x08),
        OUTPUT(1, Data|Var|Abs),
    };

    return hid_descriptor_append(desc, template, sizeof(template));
}

#include "pop_hid_macros.h"
