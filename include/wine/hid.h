/*
 * Wine internal HID structures
 *
 * Copyright 2015 Aric Stewart
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

#ifndef __WINE_PARSE_H
#define __WINE_PARSE_H

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "hidusage.h"
#include "ddk/hidpi.h"

#define HID_MAGIC 0x8491759

struct hid_value_caps
{
    USAGE   usage_page;
    USAGE   usage_min;
    USAGE   usage_max;
    USHORT  data_index_min;
    USHORT  data_index_max;
    USHORT  string_min;
    USHORT  string_max;
    USHORT  designator_min;
    USHORT  designator_max;
    ULONG   flags;
    UCHAR   report_id;
    USHORT  link_collection;
    USAGE   link_usage_page;
    USAGE   link_usage;
    USHORT  bit_field;
    USHORT  bit_size;
    USHORT  report_count;
    UCHAR   start_bit;
    USHORT  start_byte;
    LONG    logical_min;
    LONG    logical_max;
    LONG    physical_min;
    LONG    physical_max;
    ULONG   units;
    ULONG   units_exp;
};

/* named array continues on next caps */
#define HID_VALUE_CAPS_ARRAY_HAS_MORE       0x01
#define HID_VALUE_CAPS_IS_CONSTANT          0x02
#define HID_VALUE_CAPS_IS_BUTTON            0x04
#define HID_VALUE_CAPS_IS_ABSOLUTE          0x08
#define HID_VALUE_CAPS_IS_RANGE             0x10
#define HID_VALUE_CAPS_IS_STRING_RANGE      0x40
#define HID_VALUE_CAPS_IS_DESIGNATOR_RANGE  0x80

#define HID_VALUE_CAPS_HAS_NULL(x) (((x)->bit_field & 0x40) != 0)
#define HID_VALUE_CAPS_IS_ARRAY(c) (((c)->bit_field & 2) == 0)

struct hid_preparsed_data
{
    DWORD magic;
    DWORD size;
    HIDP_CAPS caps;
    USHORT value_caps_count[3];
    struct hid_value_caps value_caps[1];
};

#define HID_INPUT_VALUE_CAPS(d) ((d)->value_caps)
#define HID_OUTPUT_VALUE_CAPS(d) (HID_INPUT_VALUE_CAPS(d) + (d)->value_caps_count[0])
#define HID_FEATURE_VALUE_CAPS(d) (HID_OUTPUT_VALUE_CAPS(d) + (d)->value_caps_count[1])
#define HID_COLLECTION_VALUE_CAPS(d) (HID_FEATURE_VALUE_CAPS(d) + (d)->value_caps_count[2])

#endif /* __WINE_PARSE_H */
