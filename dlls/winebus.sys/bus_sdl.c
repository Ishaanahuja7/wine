/*
 * Plug and Play support for hid devices found through SDL2
 *
 * Copyright 2017 CodeWeavers, Aric Stewart
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

#include "config.h"
#include "wine/port.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_SDL_H
# include <SDL.h>
#endif

#include <pthread.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winreg.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "ddk/hidtypes.h"
#include "wine/debug.h"
#include "wine/unicode.h"
#include "hidusage.h"

#ifdef WORDS_BIGENDIAN
# define LE_DWORD(x) RtlUlongByteSwap(x)
#else
# define LE_DWORD(x) (x)
#endif

#include "unix_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(plugplay);

#ifdef SONAME_LIBSDL2

WINE_DECLARE_DEBUG_CHANNEL(hid_report);

static pthread_mutex_t sdl_cs = PTHREAD_MUTEX_INITIALIZER;
static struct sdl_bus_options options;

static void *sdl_handle = NULL;
static UINT quit_event = -1;
static struct list event_queue = LIST_INIT(event_queue);
static struct list device_list = LIST_INIT(device_list);

#define MAKE_FUNCPTR(f) static typeof(f) * p##f = NULL
MAKE_FUNCPTR(SDL_GetError);
MAKE_FUNCPTR(SDL_Init);
MAKE_FUNCPTR(SDL_JoystickClose);
MAKE_FUNCPTR(SDL_JoystickEventState);
MAKE_FUNCPTR(SDL_JoystickGetGUID);
MAKE_FUNCPTR(SDL_JoystickGetGUIDString);
MAKE_FUNCPTR(SDL_JoystickInstanceID);
MAKE_FUNCPTR(SDL_JoystickName);
MAKE_FUNCPTR(SDL_JoystickNumAxes);
MAKE_FUNCPTR(SDL_JoystickOpen);
MAKE_FUNCPTR(SDL_WaitEvent);
MAKE_FUNCPTR(SDL_JoystickNumButtons);
MAKE_FUNCPTR(SDL_JoystickNumBalls);
MAKE_FUNCPTR(SDL_JoystickNumHats);
MAKE_FUNCPTR(SDL_JoystickGetAxis);
MAKE_FUNCPTR(SDL_JoystickGetHat);
MAKE_FUNCPTR(SDL_IsGameController);
MAKE_FUNCPTR(SDL_GameControllerClose);
MAKE_FUNCPTR(SDL_GameControllerGetAxis);
MAKE_FUNCPTR(SDL_GameControllerGetButton);
MAKE_FUNCPTR(SDL_GameControllerName);
MAKE_FUNCPTR(SDL_GameControllerOpen);
MAKE_FUNCPTR(SDL_GameControllerEventState);
MAKE_FUNCPTR(SDL_HapticClose);
MAKE_FUNCPTR(SDL_HapticDestroyEffect);
MAKE_FUNCPTR(SDL_HapticNewEffect);
MAKE_FUNCPTR(SDL_HapticOpenFromJoystick);
MAKE_FUNCPTR(SDL_HapticQuery);
MAKE_FUNCPTR(SDL_HapticRumbleInit);
MAKE_FUNCPTR(SDL_HapticRumblePlay);
MAKE_FUNCPTR(SDL_HapticRumbleSupported);
MAKE_FUNCPTR(SDL_HapticRunEffect);
MAKE_FUNCPTR(SDL_HapticStopAll);
MAKE_FUNCPTR(SDL_JoystickIsHaptic);
MAKE_FUNCPTR(SDL_memset);
MAKE_FUNCPTR(SDL_GameControllerAddMapping);
MAKE_FUNCPTR(SDL_RegisterEvents);
MAKE_FUNCPTR(SDL_PushEvent);
static Uint16 (*pSDL_JoystickGetProduct)(SDL_Joystick * joystick);
static Uint16 (*pSDL_JoystickGetProductVersion)(SDL_Joystick * joystick);
static Uint16 (*pSDL_JoystickGetVendor)(SDL_Joystick * joystick);

struct platform_private
{
    struct unix_device unix_device;

    SDL_Joystick *sdl_joystick;
    SDL_GameController *sdl_controller;
    SDL_JoystickID id;

    int button_start;
    int axis_start;
    int ball_start;
    int hat_start;

    struct hid_descriptor desc;

    int buffer_length;
    BYTE *report_buffer;

    SDL_Haptic *sdl_haptic;
    int haptic_effect_id;
};

static inline struct platform_private *impl_from_unix_device(struct unix_device *iface)
{
    return CONTAINING_RECORD(iface, struct platform_private, unix_device);
}

static struct platform_private *find_device_from_id(SDL_JoystickID id)
{
    struct platform_private *device;

    LIST_FOR_EACH_ENTRY(device, &device_list, struct platform_private, unix_device.entry)
        if (device->id == id) return device;

    return NULL;
}

static void set_button_value(struct platform_private *ext, int index, int value)
{
    int byte_index = ext->button_start + index / 8;
    int bit_index = index % 8;
    BYTE mask = 1 << bit_index;

    if (value)
    {
        ext->report_buffer[byte_index] = ext->report_buffer[byte_index] | mask;
    }
    else
    {
        mask = ~mask;
        ext->report_buffer[byte_index] = ext->report_buffer[byte_index] & mask;
    }
}

static void set_axis_value(struct platform_private *ext, int index, short value)
{
    DWORD *report = (DWORD *)(ext->report_buffer + ext->axis_start);
    report[index] = LE_DWORD(value);
}

static void set_ball_value(struct platform_private *ext, int index, int value1, int value2)
{
    int offset;
    offset = ext->ball_start + (index * sizeof(DWORD));
    *(DWORD *)&ext->report_buffer[offset] = LE_DWORD(value1);
    *(DWORD *)&ext->report_buffer[offset + sizeof(DWORD)] = LE_DWORD(value2);
}

static void set_hat_value(struct platform_private *ext, int index, int value)
{
    int byte = ext->hat_start + index;
    unsigned char val;

    switch (value)
    {
        /* 8 1 2
         * 7 0 3
         * 6 5 4 */
        case SDL_HAT_CENTERED: val = 0; break;
        case SDL_HAT_UP: val = 1; break;
        case SDL_HAT_RIGHTUP: val = 2; break;
        case SDL_HAT_RIGHT: val = 3; break;
        case SDL_HAT_RIGHTDOWN: val = 4; break;
        case SDL_HAT_DOWN: val = 5; break;
        case SDL_HAT_LEFTDOWN: val = 6; break;
        case SDL_HAT_LEFT: val = 7; break;
        case SDL_HAT_LEFTUP: val = 8; break;
        default: return;
    }

    ext->report_buffer[byte] = val;
}

static BOOL descriptor_add_haptic(struct platform_private *ext)
{
    if (pSDL_JoystickIsHaptic(ext->sdl_joystick))
    {
        ext->sdl_haptic = pSDL_HapticOpenFromJoystick(ext->sdl_joystick);
        if (ext->sdl_haptic &&
            ((pSDL_HapticQuery(ext->sdl_haptic) & SDL_HAPTIC_LEFTRIGHT) != 0 ||
             pSDL_HapticRumbleSupported(ext->sdl_haptic)))
        {
            pSDL_HapticStopAll(ext->sdl_haptic);
            pSDL_HapticRumbleInit(ext->sdl_haptic);
            if (!hid_descriptor_add_haptics(&ext->desc))
                return FALSE;
            ext->haptic_effect_id = -1;
        }
        else
        {
            pSDL_HapticClose(ext->sdl_haptic);
            ext->sdl_haptic = NULL;
        }
    }

    return TRUE;
}

static NTSTATUS build_joystick_report_descriptor(struct platform_private *ext)
{
    static const USAGE joystick_usages[] =
    {
        HID_USAGE_GENERIC_X,
        HID_USAGE_GENERIC_Y,
        HID_USAGE_GENERIC_Z,
        HID_USAGE_GENERIC_RX,
        HID_USAGE_GENERIC_RY,
        HID_USAGE_GENERIC_RZ,
        HID_USAGE_GENERIC_SLIDER,
        HID_USAGE_GENERIC_DIAL,
        HID_USAGE_GENERIC_WHEEL
    };

    int i, report_size = 1;
    int button_count, axis_count, ball_count, hat_count;

    axis_count = pSDL_JoystickNumAxes(ext->sdl_joystick);
    if (axis_count > 6)
    {
        FIXME("Clamping joystick to 6 axis\n");
        axis_count = 6;
    }
    ext->axis_start = report_size;
    report_size += (sizeof(DWORD) * axis_count);

    ball_count = pSDL_JoystickNumBalls(ext->sdl_joystick);
    if (axis_count + ball_count * 2 > ARRAY_SIZE(joystick_usages))
    {
        FIXME("Capping ball + axis at 9\n");
        ball_count = (ARRAY_SIZE(joystick_usages) - axis_count) / 2;
    }
    ext->ball_start = report_size;
    report_size += (sizeof(DWORD) * 2 * ball_count);

    hat_count = pSDL_JoystickNumHats(ext->sdl_joystick);
    ext->hat_start = report_size;
    report_size += hat_count;

    /* For now lump all buttons just into incremental usages, Ignore Keys */
    button_count = pSDL_JoystickNumButtons(ext->sdl_joystick);
    ext->button_start = report_size;
    report_size += (button_count + 7) / 8;

    TRACE("Report will be %i bytes\n", report_size);

    if (!hid_descriptor_begin(&ext->desc, HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_JOYSTICK))
        return STATUS_NO_MEMORY;

    if (axis_count && !hid_descriptor_add_axes(&ext->desc, axis_count, HID_USAGE_PAGE_GENERIC,
                                               joystick_usages, FALSE, -32768, 32767))
        return STATUS_NO_MEMORY;

    if (ball_count && !hid_descriptor_add_axes(&ext->desc, ball_count * 2, HID_USAGE_PAGE_GENERIC,
                                               &joystick_usages[axis_count], TRUE, INT32_MIN, INT32_MAX))
        return STATUS_NO_MEMORY;

    if (hat_count && !hid_descriptor_add_hatswitch(&ext->desc, hat_count))
        return STATUS_NO_MEMORY;

    if (button_count && !hid_descriptor_add_buttons(&ext->desc, HID_USAGE_PAGE_BUTTON, 1, button_count))
        return STATUS_NO_MEMORY;

    if (!descriptor_add_haptic(ext))
        return STATUS_NO_MEMORY;

    if (!hid_descriptor_end(&ext->desc))
        return STATUS_NO_MEMORY;

    ext->buffer_length = report_size;
    if (!(ext->report_buffer = calloc(1, report_size))) goto failed;

    /* Initialize axis in the report */
    for (i = 0; i < axis_count; i++)
        set_axis_value(ext, i, pSDL_JoystickGetAxis(ext->sdl_joystick, i));
    for (i = 0; i < hat_count; i++)
        set_hat_value(ext, i, pSDL_JoystickGetHat(ext->sdl_joystick, i));

    return STATUS_SUCCESS;

failed:
    free(ext->report_buffer);
    hid_descriptor_free(&ext->desc);
    return STATUS_NO_MEMORY;
}

static SHORT compose_dpad_value(SDL_GameController *joystick)
{
    if (pSDL_GameControllerGetButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_UP))
    {
        if (pSDL_GameControllerGetButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
            return SDL_HAT_RIGHTUP;
        if (pSDL_GameControllerGetButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
            return SDL_HAT_LEFTUP;
        return SDL_HAT_UP;
    }

    if (pSDL_GameControllerGetButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_DOWN))
    {
        if (pSDL_GameControllerGetButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
            return SDL_HAT_RIGHTDOWN;
        if (pSDL_GameControllerGetButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
            return SDL_HAT_LEFTDOWN;
        return SDL_HAT_DOWN;
    }

    if (pSDL_GameControllerGetButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
        return SDL_HAT_RIGHT;
    if (pSDL_GameControllerGetButton(joystick, SDL_CONTROLLER_BUTTON_DPAD_LEFT))
        return SDL_HAT_LEFT;
    return SDL_HAT_CENTERED;
}

static NTSTATUS build_controller_report_descriptor(struct platform_private *ext)
{
    static const USAGE left_axis_usages[] = {HID_USAGE_GENERIC_X, HID_USAGE_GENERIC_Y};
    static const USAGE right_axis_usages[] = {HID_USAGE_GENERIC_RX, HID_USAGE_GENERIC_RY};
    static const USAGE trigger_axis_usages[] = {HID_USAGE_GENERIC_Z, HID_USAGE_GENERIC_RZ};
    ULONG i, button_count = SDL_CONTROLLER_BUTTON_MAX - 1;
    C_ASSERT(SDL_CONTROLLER_AXIS_MAX == 6);

    ext->axis_start = 0;
    ext->hat_start = SDL_CONTROLLER_AXIS_MAX * sizeof(DWORD);
    ext->button_start = ext->hat_start + 1;
    ext->buffer_length = ext->button_start + (button_count + 7) / 8;

    TRACE("Report will be %i bytes\n", ext->buffer_length);

    if (!hid_descriptor_begin(&ext->desc, HID_USAGE_PAGE_GENERIC, HID_USAGE_GENERIC_GAMEPAD))
        return STATUS_NO_MEMORY;

    if (!hid_descriptor_add_axes(&ext->desc, 2, HID_USAGE_PAGE_GENERIC, left_axis_usages,
                                 FALSE, -32768, 32767))
        return STATUS_NO_MEMORY;

    if (!hid_descriptor_add_axes(&ext->desc, 2, HID_USAGE_PAGE_GENERIC, right_axis_usages,
                                 FALSE, -32768, 32767))
        return STATUS_NO_MEMORY;

    if (!hid_descriptor_add_axes(&ext->desc, 2, HID_USAGE_PAGE_GENERIC, trigger_axis_usages,
                                 FALSE, 0, 32767))
        return STATUS_NO_MEMORY;

    if (!hid_descriptor_add_hatswitch(&ext->desc, 1))
        return STATUS_NO_MEMORY;

    if (!hid_descriptor_add_buttons(&ext->desc, HID_USAGE_PAGE_BUTTON, 1, button_count))
        return STATUS_NO_MEMORY;

    if (!descriptor_add_haptic(ext))
        return STATUS_NO_MEMORY;

    if (!hid_descriptor_end(&ext->desc))
        return STATUS_NO_MEMORY;

    if (!(ext->report_buffer = calloc(1, ext->buffer_length))) goto failed;

    /* Initialize axis in the report */
    for (i = SDL_CONTROLLER_AXIS_LEFTX; i < SDL_CONTROLLER_AXIS_MAX; i++)
        set_axis_value(ext, i, pSDL_GameControllerGetAxis(ext->sdl_controller, i));
    set_hat_value(ext, 0, compose_dpad_value(ext->sdl_controller));

    return STATUS_SUCCESS;

failed:
    free(ext->report_buffer);
    hid_descriptor_free(&ext->desc);
    return STATUS_NO_MEMORY;
}

static void sdl_device_destroy(struct unix_device *iface)
{
}

static NTSTATUS sdl_device_start(struct unix_device *iface)
{
    struct platform_private *ext = impl_from_unix_device(iface);
    if (ext->sdl_controller) return build_controller_report_descriptor(ext);
    return build_joystick_report_descriptor(ext);
}

static void sdl_device_stop(struct unix_device *iface)
{
    struct platform_private *private = impl_from_unix_device(iface);

    pSDL_JoystickClose(private->sdl_joystick);
    if (private->sdl_controller) pSDL_GameControllerClose(private->sdl_controller);
    if (private->sdl_haptic) pSDL_HapticClose(private->sdl_haptic);

    pthread_mutex_lock(&sdl_cs);
    list_remove(&private->unix_device.entry);
    pthread_mutex_unlock(&sdl_cs);
}

static NTSTATUS sdl_device_get_reportdescriptor(struct unix_device *iface, BYTE *buffer,
                                                DWORD length, DWORD *out_length)
{
    struct platform_private *ext = impl_from_unix_device(iface);

    *out_length = ext->desc.size;
    if (length < ext->desc.size) return STATUS_BUFFER_TOO_SMALL;

    memcpy(buffer, ext->desc.data, ext->desc.size);
    return STATUS_SUCCESS;
}

static void sdl_device_set_output_report(struct unix_device *iface, HID_XFER_PACKET *packet, IO_STATUS_BLOCK *io)
{
    struct platform_private *ext = impl_from_unix_device(iface);

    if (ext->sdl_haptic && packet->reportId == 0)
    {
        WORD left = packet->reportBuffer[2] * 128;
        WORD right = packet->reportBuffer[3] * 128;

        if (ext->haptic_effect_id >= 0)
        {
            pSDL_HapticDestroyEffect(ext->sdl_haptic, ext->haptic_effect_id);
            ext->haptic_effect_id = -1;
        }
        pSDL_HapticStopAll(ext->sdl_haptic);
        if (left != 0 || right != 0)
        {
            SDL_HapticEffect effect;

            pSDL_memset( &effect, 0, sizeof(SDL_HapticEffect) );
            effect.type = SDL_HAPTIC_LEFTRIGHT;
            effect.leftright.length = -1;
            effect.leftright.large_magnitude = left;
            effect.leftright.small_magnitude = right;

            ext->haptic_effect_id = pSDL_HapticNewEffect(ext->sdl_haptic, &effect);
            if (ext->haptic_effect_id >= 0)
            {
                pSDL_HapticRunEffect(ext->sdl_haptic, ext->haptic_effect_id, 1);
            }
            else
            {
                float i = (float)((left + right)/2.0) / 32767.0;
                pSDL_HapticRumblePlay(ext->sdl_haptic, i, -1);
            }
        }

        io->Information = packet->reportBufferLen;
        io->Status = STATUS_SUCCESS;
    }
    else
    {
        io->Information = 0;
        io->Status = STATUS_NOT_IMPLEMENTED;
    }
}

static void sdl_device_get_feature_report(struct unix_device *iface, HID_XFER_PACKET *packet, IO_STATUS_BLOCK *io)
{
    io->Information = 0;
    io->Status = STATUS_NOT_IMPLEMENTED;
}

static void sdl_device_set_feature_report(struct unix_device *iface, HID_XFER_PACKET *packet, IO_STATUS_BLOCK *io)
{
    io->Information = 0;
    io->Status = STATUS_NOT_IMPLEMENTED;
}

static const struct unix_device_vtbl sdl_device_vtbl =
{
    sdl_device_destroy,
    sdl_device_start,
    sdl_device_stop,
    sdl_device_get_reportdescriptor,
    sdl_device_set_output_report,
    sdl_device_get_feature_report,
    sdl_device_set_feature_report,
};

static BOOL set_report_from_joystick_event(struct platform_private *device, SDL_Event *event)
{
    struct unix_device *iface = &device->unix_device;

    if (device->sdl_controller) return TRUE; /* use controller events instead */

    switch(event->type)
    {
        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP:
        {
            SDL_JoyButtonEvent *ie = &event->jbutton;

            set_button_value(device, ie->button, ie->state);

            bus_event_queue_input_report(&event_queue, iface, device->report_buffer, device->buffer_length);
            break;
        }
        case SDL_JOYAXISMOTION:
        {
            SDL_JoyAxisEvent *ie = &event->jaxis;

            if (ie->axis < 6)
            {
                set_axis_value(device, ie->axis, ie->value);
                bus_event_queue_input_report(&event_queue, iface, device->report_buffer, device->buffer_length);
            }
            break;
        }
        case SDL_JOYBALLMOTION:
        {
            SDL_JoyBallEvent *ie = &event->jball;

            set_ball_value(device, ie->ball, ie->xrel, ie->yrel);
            bus_event_queue_input_report(&event_queue, iface, device->report_buffer, device->buffer_length);
            break;
        }
        case SDL_JOYHATMOTION:
        {
            SDL_JoyHatEvent *ie = &event->jhat;

            set_hat_value(device, ie->hat, ie->value);
            bus_event_queue_input_report(&event_queue, iface, device->report_buffer, device->buffer_length);
            break;
        }
        default:
            ERR("TODO: Process Report (0x%x)\n",event->type);
    }
    return FALSE;
}

static BOOL set_report_from_controller_event(struct platform_private *device, SDL_Event *event)
{
    struct unix_device *iface = &device->unix_device;

    switch(event->type)
    {
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        {
            SDL_ControllerButtonEvent *ie = &event->cbutton;
            int button;

            switch ((button = ie->button))
            {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                set_hat_value(device, 0, compose_dpad_value(device->sdl_controller));
                break;
            case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: button = 4; break;
            case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: button = 5; break;
            case SDL_CONTROLLER_BUTTON_BACK: button = 6; break;
            case SDL_CONTROLLER_BUTTON_START: button = 7; break;
            case SDL_CONTROLLER_BUTTON_LEFTSTICK: button = 8; break;
            case SDL_CONTROLLER_BUTTON_RIGHTSTICK: button = 9; break;
            case SDL_CONTROLLER_BUTTON_GUIDE: button = 10; break;
            }

            set_button_value(device, button, ie->state);
            bus_event_queue_input_report(&event_queue, iface, device->report_buffer, device->buffer_length);
            break;
        }
        case SDL_CONTROLLERAXISMOTION:
        {
            SDL_ControllerAxisEvent *ie = &event->caxis;

            set_axis_value(device, ie->axis, ie->value);
            bus_event_queue_input_report(&event_queue, iface, device->report_buffer, device->buffer_length);
            break;
        }
        default:
            ERR("TODO: Process Report (%x)\n",event->type);
    }
    return FALSE;
}

static void sdl_add_device(unsigned int index)
{
    struct device_desc desc =
    {
        .input = -1,
        .manufacturer = {"SDL"},
        .serialnumber = {"0000"},
    };
    struct platform_private *private;

    SDL_Joystick* joystick;
    SDL_JoystickID id;
    SDL_JoystickGUID guid;
    SDL_GameController *controller = NULL;

    if ((joystick = pSDL_JoystickOpen(index)) == NULL)
    {
        WARN("Unable to open sdl device %i: %s\n", index, pSDL_GetError());
        return;
    }

    if (options.map_controllers && pSDL_IsGameController(index))
        controller = pSDL_GameControllerOpen(index);

    if (controller) lstrcpynA(desc.product, pSDL_GameControllerName(controller), sizeof(desc.product));
    else lstrcpynA(desc.product, pSDL_JoystickName(joystick), sizeof(desc.product));

    id = pSDL_JoystickInstanceID(joystick);

    if (pSDL_JoystickGetProductVersion != NULL) {
        desc.vid = pSDL_JoystickGetVendor(joystick);
        desc.pid = pSDL_JoystickGetProduct(joystick);
        desc.version = pSDL_JoystickGetProductVersion(joystick);
    }
    else
    {
        desc.vid = 0x01;
        desc.pid = pSDL_JoystickInstanceID(joystick) + 1;
        desc.version = 0;
    }

    guid = pSDL_JoystickGetGUID(joystick);
    pSDL_JoystickGetGUIDString(guid, desc.serialnumber, sizeof(desc.serialnumber));

    if (controller) desc.is_gamepad = TRUE;
    else
    {
        int button_count, axis_count;

        axis_count = pSDL_JoystickNumAxes(joystick);
        button_count = pSDL_JoystickNumButtons(joystick);
        desc.is_gamepad = (axis_count == 6  && button_count >= 14);
    }

    TRACE("%s id %d, desc %s.\n", controller ? "controller" : "joystick", id, debugstr_device_desc(&desc));

    if (!(private = unix_device_create(&sdl_device_vtbl, sizeof(struct platform_private)))) return;
    list_add_tail(&device_list, &private->unix_device.entry);
    private->sdl_joystick = joystick;
    private->sdl_controller = controller;
    private->id = id;

    bus_event_queue_device_created(&event_queue, &private->unix_device, &desc);
}

static void process_device_event(SDL_Event *event)
{
    struct platform_private *device;
    SDL_JoystickID id;

    TRACE_(hid_report)("Received action %x\n", event->type);

    pthread_mutex_lock(&sdl_cs);

    if (event->type == SDL_JOYDEVICEADDED)
        sdl_add_device(((SDL_JoyDeviceEvent *)event)->which);
    else if (event->type == SDL_JOYDEVICEREMOVED)
    {
        id = ((SDL_JoyDeviceEvent *)event)->which;
        device = find_device_from_id(id);
        if (device) bus_event_queue_device_removed(&event_queue, &device->unix_device);
        else WARN("failed to find device with id %d\n", id);
    }
    else if (event->type >= SDL_JOYAXISMOTION && event->type <= SDL_JOYBUTTONUP)
    {
        id = ((SDL_JoyButtonEvent *)event)->which;
        device = find_device_from_id(id);
        if (device) set_report_from_joystick_event(device, event);
        else WARN("failed to find device with id %d\n", id);
    }
    else if (event->type >= SDL_CONTROLLERAXISMOTION && event->type <= SDL_CONTROLLERBUTTONUP)
    {
        id = ((SDL_ControllerButtonEvent *)event)->which;
        device = find_device_from_id(id);
        if (device) set_report_from_controller_event(device, event);
        else WARN("failed to find device with id %d\n", id);
    }

    pthread_mutex_unlock(&sdl_cs);
}

NTSTATUS sdl_bus_init(void *args)
{
    const char *mapping;
    int i;

    TRACE("args %p\n", args);

    options = *(struct sdl_bus_options *)args;

    if (!(sdl_handle = dlopen(SONAME_LIBSDL2, RTLD_NOW)))
    {
        WARN("could not load %s\n", SONAME_LIBSDL2);
        return STATUS_UNSUCCESSFUL;
    }
#define LOAD_FUNCPTR(f)                          \
    if ((p##f = dlsym(sdl_handle, #f)) == NULL)  \
    {                                            \
        WARN("could not find symbol %s\n", #f);  \
        goto failed;                             \
    }
    LOAD_FUNCPTR(SDL_GetError);
    LOAD_FUNCPTR(SDL_Init);
    LOAD_FUNCPTR(SDL_JoystickClose);
    LOAD_FUNCPTR(SDL_JoystickEventState);
    LOAD_FUNCPTR(SDL_JoystickGetGUID);
    LOAD_FUNCPTR(SDL_JoystickGetGUIDString);
    LOAD_FUNCPTR(SDL_JoystickInstanceID);
    LOAD_FUNCPTR(SDL_JoystickName);
    LOAD_FUNCPTR(SDL_JoystickNumAxes);
    LOAD_FUNCPTR(SDL_JoystickOpen);
    LOAD_FUNCPTR(SDL_WaitEvent);
    LOAD_FUNCPTR(SDL_JoystickNumButtons);
    LOAD_FUNCPTR(SDL_JoystickNumBalls);
    LOAD_FUNCPTR(SDL_JoystickNumHats);
    LOAD_FUNCPTR(SDL_JoystickGetAxis);
    LOAD_FUNCPTR(SDL_JoystickGetHat);
    LOAD_FUNCPTR(SDL_IsGameController);
    LOAD_FUNCPTR(SDL_GameControllerClose);
    LOAD_FUNCPTR(SDL_GameControllerGetAxis);
    LOAD_FUNCPTR(SDL_GameControllerGetButton);
    LOAD_FUNCPTR(SDL_GameControllerName);
    LOAD_FUNCPTR(SDL_GameControllerOpen);
    LOAD_FUNCPTR(SDL_GameControllerEventState);
    LOAD_FUNCPTR(SDL_HapticClose);
    LOAD_FUNCPTR(SDL_HapticDestroyEffect);
    LOAD_FUNCPTR(SDL_HapticNewEffect);
    LOAD_FUNCPTR(SDL_HapticOpenFromJoystick);
    LOAD_FUNCPTR(SDL_HapticQuery);
    LOAD_FUNCPTR(SDL_HapticRumbleInit);
    LOAD_FUNCPTR(SDL_HapticRumblePlay);
    LOAD_FUNCPTR(SDL_HapticRumbleSupported);
    LOAD_FUNCPTR(SDL_HapticRunEffect);
    LOAD_FUNCPTR(SDL_HapticStopAll);
    LOAD_FUNCPTR(SDL_JoystickIsHaptic);
    LOAD_FUNCPTR(SDL_memset);
    LOAD_FUNCPTR(SDL_GameControllerAddMapping);
    LOAD_FUNCPTR(SDL_RegisterEvents);
    LOAD_FUNCPTR(SDL_PushEvent);
#undef LOAD_FUNCPTR
    pSDL_JoystickGetProduct = dlsym(sdl_handle, "SDL_JoystickGetProduct");
    pSDL_JoystickGetProductVersion = dlsym(sdl_handle, "SDL_JoystickGetProductVersion");
    pSDL_JoystickGetVendor = dlsym(sdl_handle, "SDL_JoystickGetVendor");

    if (pSDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_HAPTIC) < 0)
    {
        ERR("could not init SDL: %s\n", pSDL_GetError());
        goto failed;
    }

    if ((quit_event = pSDL_RegisterEvents(1)) == -1)
    {
        ERR("error registering quit event\n");
        goto failed;
    }

    pSDL_JoystickEventState(SDL_ENABLE);
    pSDL_GameControllerEventState(SDL_ENABLE);

    /* Process mappings */
    if (pSDL_GameControllerAddMapping)
    {
        if ((mapping = getenv("SDL_GAMECONTROLLERCONFIG")))
        {
            TRACE("Setting environment mapping %s\n", debugstr_a(mapping));
            if (pSDL_GameControllerAddMapping(mapping) < 0)
                WARN("Failed to add environment mapping %s\n", pSDL_GetError());
        }
        else for (i = 0; i < options.mappings_count; ++i)
        {
            TRACE("Setting registry mapping %s\n", debugstr_a(options.mappings[i]));
            if (pSDL_GameControllerAddMapping(options.mappings[i]) < 0)
                WARN("Failed to add registry mapping %s\n", pSDL_GetError());
        }
    }

    return STATUS_SUCCESS;

failed:
    dlclose(sdl_handle);
    sdl_handle = NULL;
    return STATUS_UNSUCCESSFUL;
}

NTSTATUS sdl_bus_wait(void *args)
{
    struct bus_event *result = args;
    SDL_Event event;

    /* cleanup previously returned event */
    bus_event_cleanup(result);

    do
    {
        if (bus_event_queue_pop(&event_queue, result)) return STATUS_PENDING;
        if (pSDL_WaitEvent(&event) != 0) process_device_event(&event);
        else WARN("SDL_WaitEvent failed: %s\n", pSDL_GetError());
    } while (event.type != quit_event);

    TRACE("SDL main loop exiting\n");
    bus_event_queue_destroy(&event_queue);
    dlclose(sdl_handle);
    sdl_handle = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS sdl_bus_stop(void *args)
{
    SDL_Event event;

    if (!sdl_handle) return STATUS_SUCCESS;

    event.type = quit_event;
    if (pSDL_PushEvent(&event) != 1)
    {
        ERR("error pushing quit event\n");
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}

#else

NTSTATUS sdl_bus_init(void *args)
{
    WARN("SDL support not compiled in!\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS sdl_bus_wait(void *args)
{
    WARN("SDL support not compiled in!\n");
    return STATUS_NOT_IMPLEMENTED;
}

NTSTATUS sdl_bus_stop(void *args)
{
    WARN("SDL support not compiled in!\n");
    return STATUS_NOT_IMPLEMENTED;
}

#endif /* SONAME_LIBSDL2 */
