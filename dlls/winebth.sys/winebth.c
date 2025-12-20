/*
 * Bluetooth bus driver
 *
 * Copyright 2024-2025 Vibhav Pant
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

#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <windef.h>
#include <winbase.h>
#include <winternl.h>
#include <winnls.h>
#include <wctype.h>
#include <wtypes.h>
#include <initguid.h>
#include <devpkey.h>
#include <propkey.h>
#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include <bthdef.h>
#include <bthledef.h>
#include <winioctl.h>
#include <bthioctl.h>
#include <ddk/wdm.h>
#include <ddk/bthguid.h>

#include <wine/winebth.h>
#include <wine/debug.h>
#include <wine/list.h>

#include "winebth_priv.h"

WINE_DEFAULT_DEBUG_CHANNEL( winebth );

static DRIVER_OBJECT *driver_obj;

static DEVICE_OBJECT *bus_fdo, *bus_pdo, *device_auth;

#define DECLARE_CRITICAL_SECTION( cs )                                                             \
    static CRITICAL_SECTION cs;                                                                    \
    static CRITICAL_SECTION_DEBUG cs##_debug = {                                                   \
        0,                                                                                         \
        0,                                                                                         \
        &( cs ),                                                                                   \
        { &cs##_debug.ProcessLocksList, &cs##_debug.ProcessLocksList },                            \
        0,                                                                                         \
        0,                                                                                         \
        { (DWORD_PTR)( __FILE__ ": " #cs ) } };                                                    \
    static CRITICAL_SECTION cs = { &cs##_debug, -1, 0, 0, 0, 0 };

DECLARE_CRITICAL_SECTION( device_list_cs );

static struct list device_list = LIST_INIT( device_list );

/* Object lifecycle state machine:
 *   INITIALIZING -> ACTIVE -> REMOVING
 *
 * State transitions:
 *   - INITIALIZING: Object being created, not yet in list
 *   - ACTIVE: Object in list, fully operational
 *   - REMOVING: Marked for removal, draining references
 *
 * State writes must be done while holding device_list_cs.
 * State reads can use InterlockedCompareExchange for atomic reads. */
enum bluetooth_object_state
{
    BLUETOOTH_STATE_INITIALIZING,
    BLUETOOTH_STATE_ACTIVE,
    BLUETOOTH_STATE_REMOVING,
};

struct bluetooth_radio
{
    struct list entry;
    LONG refcount;                              /* Atomic reference count */
    enum bluetooth_object_state state;          /* Object lifecycle state */

    DEVICE_OBJECT *device_obj;
    winebluetooth_radio_props_mask_t props_mask; /* Guarded by device_list_cs */
    struct winebluetooth_radio_properties props; /* Guarded by device_list_cs */
    winebluetooth_radio_t radio;
    WCHAR *hw_name;
    WCHAR *instance_prefix;
    unsigned int index;
    UNICODE_STRING bthport_symlink_name;
    UNICODE_STRING bthradio_symlink_name;
    unsigned int next_device_index;

    struct list remote_devices;                 /* Guarded by device_list_cs */

    LIST_ENTRY irp_list;                        /* Guarded by device_list_cs */
};

struct bluetooth_remote_device
{
    struct list entry;
    LONG refcount;                              /* Atomic reference count */
    enum bluetooth_object_state state;          /* Object lifecycle state */

    DEVICE_OBJECT *device_obj;
    struct bluetooth_radio *radio;              /* The radio associated with this remote device */
    winebluetooth_device_t device;
    CRITICAL_SECTION props_cs;                  /* Protects props, props_mask, le, bthle_symlink_name */
    winebluetooth_device_props_mask_t props_mask; /* Guarded by props_cs */
    struct winebluetooth_device_properties props; /* Guarded by props_cs */

    BOOL le;                                    /* Guarded by props_cs */
    UNICODE_STRING bthle_symlink_name;          /* Guarded by props_cs */
    struct list gatt_services;                  /* Guarded by device_list_cs (list ops) */
};

struct bluetooth_gatt_service
{
    struct list entry;
    LONG refcount;                              /* Atomic reference count */

    winebluetooth_gatt_service_t service;
    GUID uuid;
    unsigned int primary : 1;
    UINT16 handle;

    struct list characteristics;                /* Immutable after service added to device */
};

struct bluetooth_gatt_characteristic
{
    struct list entry;

    winebluetooth_gatt_characteristic_t characteristic;
    BTH_LE_GATT_CHARACTERISTIC props;
};

enum bluetooth_pdo_ext_type
{
    BLUETOOTH_PDO_EXT_RADIO,
    BLUETOOTH_PDO_EXT_REMOTE_DEVICE,
};

struct bluetooth_pdo_ext
{
    enum bluetooth_pdo_ext_type type;
    union {
        struct bluetooth_radio radio;
        struct bluetooth_remote_device remote_device;
    };
};

static NTSTATUS WINAPI dispatch_auth( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS status = irp->IoStatus.Status;

    TRACE( "device %p irp %p code %#lx\n", device, irp, code );

    switch (code)
    {
    case IOCTL_WINEBTH_AUTH_REGISTER:
        status = winebluetooth_auth_agent_enable_incoming();
        break;
    default:
        break;
    }

    irp->IoStatus.Status = status;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return status;
}

static void le_to_uuid( const BTH_LE_UUID *le_uuid, GUID *uuid )
{
    if (le_uuid->IsShortUuid)
    {
        *uuid = BTH_LE_ATT_BLUETOOTH_BASE_GUID;
        uuid->Data1 = le_uuid->Value.ShortUuid;
    }
    else
        *uuid = le_uuid->Value.LongUuid;
}

/* Compare two GUIDs for sorting - returns negative, zero, or positive */
static int compare_guids( const GUID *a, const GUID *b )
{
    if (a->Data1 != b->Data1) return (a->Data1 < b->Data1) ? -1 : 1;
    if (a->Data2 != b->Data2) return (a->Data2 < b->Data2) ? -1 : 1;
    if (a->Data3 != b->Data3) return (a->Data3 < b->Data3) ? -1 : 1;
    return memcmp( a->Data4, b->Data4, sizeof(a->Data4) );
}

/* Compare characteristics by UUID for qsort */
static int compare_characteristics_by_uuid( const void *a, const void *b )
{
    const BTH_LE_GATT_CHARACTERISTIC *ca = a;
    const BTH_LE_GATT_CHARACTERISTIC *cb = b;
    return compare_guids( &ca->CharacteristicUuid.Value.LongUuid, &cb->CharacteristicUuid.Value.LongUuid );
}

/* Caller should hold props_cs */
static struct bluetooth_gatt_service *find_gatt_service( struct list *services, const GUID *uuid, UINT16 handle )
{
    struct bluetooth_gatt_service *service;
    LIST_FOR_EACH_ENTRY( service, services, struct bluetooth_gatt_service, entry )
    {
        if (IsEqualGUID( &service->uuid, uuid ) && service->handle == handle)
            return service;
    }
    return NULL;
}

/* Caller should hold props_cs */
static struct bluetooth_gatt_service *find_gatt_service_by_handle( struct list *services, UINT16 handle )
{
    struct bluetooth_gatt_service *service;
    LIST_FOR_EACH_ENTRY( service, services, struct bluetooth_gatt_service, entry )
    {
        if (service->handle == handle)
            return service;
    }
    return NULL;
}

/* Caller should hold props_cs. Characteristics are immutable after discovery. */
static struct bluetooth_gatt_characteristic *find_gatt_characteristic(
    struct bluetooth_gatt_service *service, const BTH_LE_GATT_CHARACTERISTIC *char_props )
{
    struct bluetooth_gatt_characteristic *chrc;
    GUID uuid;

    le_to_uuid( &char_props->CharacteristicUuid, &uuid );
    ERR( "find_gatt_characteristic: looking for UUID=%s handle=%u svc_handle=%u\n",
         debugstr_guid( &uuid ), char_props->AttributeHandle, char_props->ServiceHandle );
    LIST_FOR_EACH_ENTRY( chrc, &service->characteristics, struct bluetooth_gatt_characteristic, entry )
    {
        GUID chrc_uuid;
        le_to_uuid( &chrc->props.CharacteristicUuid, &chrc_uuid );
        ERR( "  checking: UUID=%s handle=%u\n", debugstr_guid( &chrc_uuid ), chrc->props.AttributeHandle );
        if (IsEqualGUID( &chrc_uuid, &uuid ) &&
            chrc->props.AttributeHandle == char_props->AttributeHandle)
            return chrc;
    }
    ERR( "find_gatt_characteristic: NOT FOUND!\n" );
    return NULL;
}

/* ============================================================================
 * Reference Counting Helpers
 *
 * These helpers implement atomic reference counting for safe object access
 * after releasing device_list_cs. The pattern is:
 *   1. Acquire device_list_cs
 *   2. Find object and incref
 *   3. Release device_list_cs
 *   4. Use object (may make external calls)
 *   5. decref when done
 * ============================================================================ */

/* Radio reference counting */
static inline void bluetooth_radio_incref( struct bluetooth_radio *radio )
{
    InterlockedIncrement( &radio->refcount );
}

static inline LONG bluetooth_radio_decref( struct bluetooth_radio *radio )
{
    return InterlockedDecrement( &radio->refcount );
}

/* Find radio by handle and increment refcount. Caller must hold device_list_cs. */
static struct bluetooth_radio *bluetooth_radio_find_and_incref( winebluetooth_radio_t handle )
{
    struct bluetooth_radio *radio;

    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( handle, radio->radio ) &&
            radio->state == BLUETOOTH_STATE_ACTIVE)
        {
            bluetooth_radio_incref( radio );
            return radio;
        }
    }
    return NULL;
}

/* Remote device reference counting */
static inline void bluetooth_device_incref( struct bluetooth_remote_device *device )
{
    InterlockedIncrement( &device->refcount );
}

static inline LONG bluetooth_device_decref( struct bluetooth_remote_device *device )
{
    return InterlockedDecrement( &device->refcount );
}

/* Find device by handle and increment refcount. Caller must hold device_list_cs. */
static struct bluetooth_remote_device *bluetooth_device_find_and_incref(
    struct bluetooth_radio *radio, winebluetooth_device_t handle )
{
    struct bluetooth_remote_device *device;

    LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
    {
        if (winebluetooth_device_equal( handle, device->device ) &&
            device->state == BLUETOOTH_STATE_ACTIVE)
        {
            bluetooth_device_incref( device );
            return device;
        }
    }
    return NULL;
}

/* Find device by Bluetooth address and increment refcount. Caller must hold device_list_cs.
 *
 * IMPORTANT: We check state AND incref while still holding device_list_cs to avoid
 * a TOCTOU race where the device could transition to REMOVING between the check
 * and the incref. The incref must happen before releasing the lock. */
static struct bluetooth_remote_device *bluetooth_device_find_by_addr_and_incref(
    struct bluetooth_radio *radio, BTH_ADDR addr )
{
    struct bluetooth_remote_device *device;

    LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
    {
        BOOL match;

        /* Check state first - if not ACTIVE, skip entirely */
        if (device->state != BLUETOOTH_STATE_ACTIVE)
            continue;

        EnterCriticalSection( &device->props_cs );
        match = (device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS) &&
                (device->props.address.ullLong == addr);
        LeaveCriticalSection( &device->props_cs );

        if (match)
        {
            /* Take reference while still holding device_list_cs.
             * This ensures the object can't be freed before we get our ref. */
            bluetooth_device_incref( device );
            return device;
        }
    }
    return NULL;
}

/* GATT service reference counting */
static inline void bluetooth_gatt_service_incref( struct bluetooth_gatt_service *service )
{
    InterlockedIncrement( &service->refcount );
}

static inline LONG bluetooth_gatt_service_decref( struct bluetooth_gatt_service *service )
{
    return InterlockedDecrement( &service->refcount );
}

/* State validation helpers */
static inline BOOL bluetooth_radio_is_active( struct bluetooth_radio *radio )
{
    return radio->state == BLUETOOTH_STATE_ACTIVE;
}

static inline BOOL bluetooth_device_is_active( struct bluetooth_remote_device *device )
{
    return device->state == BLUETOOTH_STATE_ACTIVE;
}


static NTSTATUS bluetooth_remote_device_dispatch( DEVICE_OBJECT *device, struct bluetooth_remote_device *ext, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    ULONG outsize = stack->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS status = irp->IoStatus.Status;

    TRACE( "device=%p, ext=%p, irp=%p, code=%#lx\n", device, ext, irp, code );

    switch (code)
    {
    case IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES:
    {
        const SIZE_T min_size = offsetof( struct winebth_le_device_get_gatt_services_params, services[0] );
        struct winebth_le_device_get_gatt_services_params *services = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_gatt_service *svc;
        SIZE_T rem;
        BOOL need_connect = FALSE;

        if (!services || outsize < min_size)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        rem = (outsize - min_size)/sizeof( *services->services );
        status = STATUS_SUCCESS;
        services->count = 0;

        EnterCriticalSection( &ext->props_cs );
        TRACE( "GET_GATT_SERVICES: device=%p device.handle=%p name='%s' gatt_empty=%d connected=%d\n",
               ext, (void*)ext->device.handle, ext->props.name,
               list_empty( &ext->gatt_services ), ext->props.connected );
        if (list_empty( &ext->gatt_services ) && !ext->props.connected)
        {
            need_connect = TRUE;
            TRACE( "GATT services requested but device not connected, triggering connect\n" );
        }
        LIST_FOR_EACH_ENTRY( svc, &ext->gatt_services, struct bluetooth_gatt_service, entry )
        {
            if (!svc->primary)
                continue;
            services->count++;
            if (rem)
            {
                BTH_LE_GATT_SERVICE *info;

                info = &services->services[services->count - 1];
                memset( info, 0, sizeof( *info ) );
                uuid_to_le( &svc->uuid, &info->ServiceUuid );
                info->AttributeHandle = svc->handle;
                rem--;
            }
        }
        LeaveCriticalSection( &ext->props_cs );

        if (need_connect)
        {
            NTSTATUS pair_status = winebluetooth_device_start_pairing( ext->device, NULL );
            (void)pair_status;
        }

        irp->IoStatus.Information = offsetof( struct winebth_le_device_get_gatt_services_params, services[services->count] );
        if (services->count > rem)
            status = STATUS_MORE_ENTRIES;
        break;
    }
    case IOCTL_WINEBTH_LE_DEVICE_GET_GATT_CHARACTERISTICS:
    {
        const SIZE_T min_size = offsetof( struct winebth_le_device_get_gatt_characteristics_params, characteristics[0] );
        struct winebth_le_device_get_gatt_characteristics_params *chars = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_gatt_characteristic *chrc;
        struct bluetooth_gatt_service *service;
        SIZE_T rem;
        GUID uuid;

        if (!chars || outsize < min_size)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        rem = (outsize - min_size)/sizeof( *chars->characteristics );
        status = STATUS_SUCCESS;
        chars->count = 0;
        le_to_uuid( &chars->service.ServiceUuid, &uuid );

        EnterCriticalSection( &ext->props_cs );
        service = find_gatt_service( &ext->gatt_services, &uuid, chars->service.AttributeHandle );
        if (!service)
        {
            status = STATUS_INVALID_PARAMETER;
            LeaveCriticalSection( &ext->props_cs );
            break;
        }

        /* Characteristics are immutable after discovery - no lock needed */
        LIST_FOR_EACH_ENTRY( chrc, &service->characteristics, struct bluetooth_gatt_characteristic, entry )
        {
            chars->count++;
            if (rem)
            {
                chars->characteristics[chars->count - 1] = chrc->props;
                rem--;
            }
        }
        LeaveCriticalSection( &ext->props_cs );

        /* Sort characteristics by UUID - some apps (e.g. Square Golf) stop iterating early
         * and expect specific characteristics to appear before others. Windows appears to
         * return characteristics sorted by UUID. */
        if (chars->count > 1)
            qsort( chars->characteristics, chars->count, sizeof(chars->characteristics[0]),
                   compare_characteristics_by_uuid );

        irp->IoStatus.Information = offsetof( struct winebth_le_device_get_gatt_characteristics_params, characteristics[chars->count] );
        if (chars->count > rem)
            status = STATUS_MORE_ENTRIES;
        break;
    }
    case IOCTL_WINEBTH_LE_DEVICE_READ_CHARACTERISTIC:
    {
        struct winebth_le_device_read_characteristic_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_gatt_service *service;
        struct bluetooth_gatt_characteristic *chrc;
        GUID svc_uuid;
        unsigned int data_len = 0;

        if (!params || outsize < sizeof(struct winebth_le_device_read_characteristic_params))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        EnterCriticalSection( &ext->props_cs );
        le_to_uuid( &params->service.ServiceUuid, &svc_uuid );
        if (IsEqualGUID( &svc_uuid, &GUID_NULL ))
            service = find_gatt_service_by_handle( &ext->gatt_services, params->characteristic.ServiceHandle );
        else
            service = find_gatt_service( &ext->gatt_services, &svc_uuid, params->service.AttributeHandle );
        if (!service)
        {
            LeaveCriticalSection( &ext->props_cs );
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        chrc = find_gatt_characteristic( service, &params->characteristic );
        if (!chrc)
        {
            LeaveCriticalSection( &ext->props_cs );
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        status = winebluetooth_gatt_characteristic_read(
            chrc->characteristic,
            params->data,
            outsize - offsetof(struct winebth_le_device_read_characteristic_params, data),
            &data_len );
        params->data_size = data_len;
        irp->IoStatus.Information = offsetof(struct winebth_le_device_read_characteristic_params, data) + data_len;

        LeaveCriticalSection( &ext->props_cs );
        break;
    }
    case IOCTL_WINEBTH_LE_DEVICE_WRITE_CHARACTERISTIC:
    {
        struct winebth_le_device_write_characteristic_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_gatt_service *service;
        struct bluetooth_gatt_characteristic *chrc;
        GUID svc_uuid;
        ULONG insize = stack->Parameters.DeviceIoControl.InputBufferLength;

        if (!params || insize < sizeof(struct winebth_le_device_write_characteristic_params))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        if (insize < offsetof( struct winebth_le_device_write_characteristic_params, data ) + params->data_size)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        EnterCriticalSection( &ext->props_cs );
        le_to_uuid( &params->service.ServiceUuid, &svc_uuid );
        if (IsEqualGUID( &svc_uuid, &GUID_NULL ))
            service = find_gatt_service_by_handle( &ext->gatt_services, params->characteristic.ServiceHandle );
        else
            service = find_gatt_service( &ext->gatt_services, &svc_uuid, params->service.AttributeHandle );
        if (!service)
        {
            LeaveCriticalSection( &ext->props_cs );
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        chrc = find_gatt_characteristic( service, &params->characteristic );
        if (!chrc)
        {
            LeaveCriticalSection( &ext->props_cs );
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        status = winebluetooth_gatt_characteristic_write(
            chrc->characteristic,
            params->data,
            params->data_size,
            params->write_type );

        LeaveCriticalSection( &ext->props_cs );
        break;
    }
    case IOCTL_WINEBTH_LE_DEVICE_SET_NOTIFY:
    {
        struct winebth_le_device_set_notify_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_gatt_service *service;
        struct bluetooth_gatt_characteristic *chrc;
        GUID svc_uuid;
        ULONG insize = stack->Parameters.DeviceIoControl.InputBufferLength;

        if (!params || insize < sizeof(struct winebth_le_device_set_notify_params))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        EnterCriticalSection( &ext->props_cs );
        le_to_uuid( &params->service.ServiceUuid, &svc_uuid );
        if (IsEqualGUID( &svc_uuid, &GUID_NULL ))
            service = find_gatt_service_by_handle( &ext->gatt_services, params->characteristic.ServiceHandle );
        else
            service = find_gatt_service( &ext->gatt_services, &svc_uuid, params->service.AttributeHandle );
        if (!service)
        {
            LeaveCriticalSection( &ext->props_cs );
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        chrc = find_gatt_characteristic( service, &params->characteristic );
        if (!chrc)
        {
            LeaveCriticalSection( &ext->props_cs );
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        status = winebluetooth_gatt_characteristic_set_notify(
            chrc->characteristic,
            params->enable );

        LeaveCriticalSection( &ext->props_cs );
        break;
    }
    case IOCTL_WINEBTH_LE_DEVICE_READ_NOTIFICATION:
    {
        struct winebth_le_device_read_notification_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_gatt_service *service;
        struct bluetooth_gatt_characteristic *chrc;
        GUID svc_uuid;
        ULONG insize = stack->Parameters.DeviceIoControl.InputBufferLength;
        ULONG outsize = stack->Parameters.DeviceIoControl.OutputBufferLength;

        if (!params || insize < sizeof(struct winebth_le_device_read_notification_params))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        EnterCriticalSection( &ext->props_cs );
        le_to_uuid( &params->service.ServiceUuid, &svc_uuid );
        if (IsEqualGUID( &svc_uuid, &GUID_NULL ))
            service = find_gatt_service_by_handle( &ext->gatt_services, params->characteristic.ServiceHandle );
        else
            service = find_gatt_service( &ext->gatt_services, &svc_uuid, params->service.AttributeHandle );
        if (!service)
        {
            LeaveCriticalSection( &ext->props_cs );
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        chrc = find_gatt_characteristic( service, &params->characteristic );
        if (!chrc)
        {
            LeaveCriticalSection( &ext->props_cs );
            status = STATUS_INVALID_PARAMETER;
            break;
        }

        {
            unsigned int data_size = 0;
            unsigned int buffer_size = outsize >= sizeof(struct winebth_le_device_read_notification_params) ?
                (unsigned int)(outsize - sizeof(struct winebth_le_device_read_notification_params)) : 0;
            winebluetooth_gatt_characteristic_t characteristic = chrc->characteristic;

            winebluetooth_gatt_characteristic_dup( characteristic );
            LeaveCriticalSection( &ext->props_cs );

            status = winebluetooth_gatt_characteristic_read_notification(
                characteristic,
                params->data,
                buffer_size,
                &data_size );

            winebluetooth_gatt_characteristic_free( characteristic );

            if (status == STATUS_SUCCESS) {
                irp->IoStatus.Information = sizeof(struct winebth_le_device_read_notification_params) + data_size;
            } else {
                irp->IoStatus.Information = 0;
            }
        }
        break;
    }
    case IOCTL_WINEBTH_LE_DEVICE_GET_CONNECTION_STATUS:
    {
        BOOL *connected = irp->AssociatedIrp.SystemBuffer;

        if (!connected || outsize < sizeof(BOOL))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        EnterCriticalSection( &ext->props_cs );
        *connected = ext->props.connected;
        TRACE( "GET_CONNECTION_STATUS: returning connected=%d for device=%p\n", *connected, ext );
        LeaveCriticalSection( &ext->props_cs );

        irp->IoStatus.Information = sizeof(BOOL);
        status = STATUS_SUCCESS;
        break;
    }
    default:
        FIXME( "Unimplemented IOCTL code: %#lx\n", code );
    }

    irp->IoStatus.Status = status;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return status;
}

static NTSTATUS bluetooth_radio_dispatch( DEVICE_OBJECT *device, struct bluetooth_radio *ext, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG insize = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outsize = stack->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS status = irp->IoStatus.Status;

    TRACE( "device=%p, ext=%p, irp=%p code=%#lx\n", device, ext, irp, code );
    ERR( "bluetooth_radio_dispatch: IOCTL code received = 0x%lx\n", code );
    ERR( "bluetooth_radio_dispatch: IOCTL_WINEBTH_RADIO_GET_LE_DEVICE_GATT_SERVICES = 0x%lx\n",
         (unsigned long)IOCTL_WINEBTH_RADIO_GET_LE_DEVICE_GATT_SERVICES );

    switch (code)
    {
    case IOCTL_BTH_GET_LOCAL_INFO:
    {
        BTH_LOCAL_RADIO_INFO *info = (BTH_LOCAL_RADIO_INFO *)irp->AssociatedIrp.SystemBuffer;

        if (!info || outsize < sizeof(*info))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        memset( info, 0, sizeof( *info ) );

        EnterCriticalSection( &device_list_cs );
        if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_ADDRESS)
        {
            info->localInfo.flags |= BDIF_ADDRESS;
            info->localInfo.address = RtlUlonglongByteSwap( ext->props.address.ullLong ) >> 16;
        }
        if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_NAME)
        {
            info->localInfo.flags |= BDIF_NAME;
            strcpy( info->localInfo.name, ext->props.name );
        }
        if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_CLASS)
        {
            info->localInfo.flags |= BDIF_COD;
            info->localInfo.classOfDevice = ext->props.class;
        }
        if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_VERSION)
            info->hciVersion = info->radioInfo.lmpVersion = ext->props.version;
        if (ext->props.connectable)
            info->flags |= LOCAL_RADIO_CONNECTABLE;
        if (ext->props.discoverable)
            info->flags |= LOCAL_RADIO_DISCOVERABLE;
        if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_MANUFACTURER)
            info->radioInfo.mfg = ext->props.manufacturer;
        LeaveCriticalSection( &device_list_cs );

        irp->IoStatus.Information = sizeof( *info );
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_BTH_GET_DEVICE_INFO:
    {
        BTH_DEVICE_INFO_LIST *list = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        SIZE_T rem_devices;

        if (!list)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (outsize < sizeof( *list ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        rem_devices = (outsize - sizeof( *list ))/sizeof(BTH_DEVICE_INFO) + 1;
        status = STATUS_SUCCESS;
        irp->IoStatus.Information = 0;
        list->numOfDevices = 0;

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            list->numOfDevices++;
            if (rem_devices > 0)
            {
                BTH_DEVICE_INFO *info;

                info = &list->deviceList[list->numOfDevices - 1];
                memset( info, 0, sizeof( *info ) );

                EnterCriticalSection( &device->props_cs );
                winebluetooth_device_properties_to_info( device->props_mask, &device->props, info );
                LeaveCriticalSection( &device->props_cs );

                irp->IoStatus.Information += sizeof( *info );
                rem_devices--;
            }
        }
        LeaveCriticalSection( &device_list_cs );

        irp->IoStatus.Information += sizeof( *list );
        if (list->numOfDevices)
            irp->IoStatus.Information -= sizeof( BTH_DEVICE_INFO );

        /* The output buffer needs to be exactly sized. */
        if (rem_devices)
            status = STATUS_INVALID_BUFFER_SIZE;
        break;
    }
    case IOCTL_BTH_DISCONNECT_DEVICE:
    {
        const BTH_ADDR *param = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        winebluetooth_device_t device_handle;
        BTH_ADDR device_addr;
        BOOL found = FALSE;

        if (!param || insize < sizeof( *param ))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        device_addr = RtlUlonglongByteSwap( *param ) >> 16;
        status = STATUS_DEVICE_NOT_CONNECTED;

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            EnterCriticalSection( &device->props_cs );
            found = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                    device_addr == device->props.address.ullLong;
            LeaveCriticalSection( &device->props_cs );
            if (found)
            {
                winebluetooth_device_dup(( device_handle = device->device ));
                break;
            }
        }
        LeaveCriticalSection( &device_list_cs );
        if (found)
        {
            status = winebluetooth_device_disconnect( device_handle );
            winebluetooth_device_free( device_handle );
        }
        break;
    }
    case IOCTL_WINEBTH_RADIO_SET_FLAG:
    {
        const struct winebth_radio_set_flag_params *params = irp->AssociatedIrp.SystemBuffer;
        union winebluetooth_property prop_value = {0};

        if (!params)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (insize < sizeof( *params ))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        prop_value.boolean = !!params->enable;
        status = winebluetooth_radio_set_property( ext->radio, params->flag, &prop_value );
        break;
    }
    case IOCTL_WINEBTH_RADIO_START_DISCOVERY:
        status = winebluetooth_radio_start_discovery( ext->radio );
        break;
    case IOCTL_WINEBTH_RADIO_STOP_DISCOVERY:
        status = winebluetooth_radio_stop_discovery( ext->radio );
        break;
    case IOCTL_WINEBTH_RADIO_SEND_AUTH_RESPONSE:
    {
        struct winebth_radio_send_auth_response_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;

        if (!params)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (insize < sizeof( *params ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        if (outsize < sizeof( *params ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        status = STATUS_DEVICE_NOT_CONNECTED;
        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                      device->props.address.ullLong == params->address;
            LeaveCriticalSection( &device->props_cs );
            if (matches)
            {
                BOOL authenticated = FALSE;
                status = winebluetooth_auth_send_response( device->device, params->method,
                                                           params->numeric_value_or_passkey, params->negative,
                                                           &authenticated );
                params->authenticated = !!authenticated;
                break;
            }
        }
        if (!status)
            irp->IoStatus.Information = sizeof( *params );
        LeaveCriticalSection( &device_list_cs );
        break;
    }
    case IOCTL_WINEBTH_RADIO_START_AUTH:
    {
        const struct winebth_radio_start_auth_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;

        if (!params)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (insize < sizeof( *params ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        status = STATUS_DEVICE_DOES_NOT_EXIST;
        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                device->props.address.ullLong == params->address;
            LeaveCriticalSection( &device->props_cs );
            if (matches)
            {
                status = winebluetooth_device_start_pairing( device->device, irp );
                if (status == STATUS_PENDING)
                {
                    IoMarkIrpPending( irp );
                    InsertTailList( &ext->irp_list, &irp->Tail.Overlay.ListEntry );
                }
                break;
            }
        }
        LeaveCriticalSection( &device_list_cs );
        break;
    }
    case IOCTL_WINEBTH_RADIO_REMOVE_DEVICE:
    {
        const BTH_ADDR *param = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        winebluetooth_device_t device_handle;
        winebluetooth_radio_t radio_handle;
        BOOL found = FALSE;

        if (!param)
        {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (insize < sizeof( *param ))
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }

        status = STATUS_NOT_FOUND;
        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            EnterCriticalSection( &device->props_cs );
            found = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                    device->props.address.ullLong == *param && device->props.paired;
            LeaveCriticalSection( &device->props_cs );

            if (found)
            {
                winebluetooth_device_dup(( device_handle = device->device ));
                winebluetooth_radio_dup(( radio_handle = ext->radio ));
                break;
            }
        }
        LeaveCriticalSection( &device_list_cs );
        if (found)
        {
            status = winebluetooth_radio_remove_device( radio_handle, device_handle );
            winebluetooth_device_free( device_handle );
            winebluetooth_radio_free( radio_handle );
        }
        break;
    }
    case IOCTL_WINEBTH_RADIO_GET_LE_DEVICE_GATT_SERVICES:
    {
        const SIZE_T min_size = offsetof( struct winebth_radio_get_le_device_gatt_services_params, services[0] );
        struct winebth_radio_get_le_device_gatt_services_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        BTH_ADDR device_addr;
        SIZE_T rem;
        BOOL found = FALSE;
        winebluetooth_device_t device_handle = {0};
        BOOL need_connect = FALSE;

        ERR( "IOCTL_WINEBTH_RADIO_GET_LE_DEVICE_GATT_SERVICES handler called!\n" );

        if (!params || outsize < min_size || insize < sizeof( BTH_ADDR ))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        device_addr = params->address;
        rem = (outsize - min_size) / sizeof( *params->services );
        status = STATUS_DEVICE_NOT_CONNECTED;
        params->count = 0;

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                      device->props.address.ullLong == device_addr;
            if (matches)
            {
                struct bluetooth_gatt_service *svc;

                TRACE( "RADIO_GET_LE_DEVICE_GATT_SERVICES: found device '%s' with %s gatt_services connected=%d\n",
                       device->props.name, list_empty( &device->gatt_services ) ? "empty" : "populated",
                       device->props.connected );

                if (list_empty( &device->gatt_services ) && !device->props.connected)
                {
                    need_connect = TRUE;
                    winebluetooth_device_dup(( device_handle = device->device ));
                }

                found = TRUE;
                status = STATUS_SUCCESS;
                LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
                {
                    if (!svc->primary)
                        continue;
                    params->count++;
                    if (rem)
                    {
                        BTH_LE_GATT_SERVICE *info = &params->services[params->count - 1];
                        memset( info, 0, sizeof( *info ) );
                        uuid_to_le( &svc->uuid, &info->ServiceUuid );
                        info->AttributeHandle = svc->handle;
                        rem--;
                    }
                }
            }
            LeaveCriticalSection( &device->props_cs );
            if (found)
                break;
        }
        LeaveCriticalSection( &device_list_cs );

        if (need_connect && device_handle.handle)
        {
            ERR( "RADIO_GET_LE_DEVICE_GATT_SERVICES: triggering connection and waiting for services...\n" );
            winebluetooth_device_start_pairing( device_handle, NULL );
            winebluetooth_device_free( device_handle );

            /* Give event loop time to process GATT_SERVICE_ADDED events */
            Sleep( 500 );

            /* Re-query services after pairing */
            params->count = 0;
            EnterCriticalSection( &device_list_cs );
            LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
            {
                BOOL matches;
                EnterCriticalSection( &device->props_cs );
                matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                          device->props.address.ullLong == device_addr;
                if (matches)
                {
                    struct bluetooth_gatt_service *svc;
                    ERR( "RADIO_GET_LE_DEVICE_GATT_SERVICES: re-query after pairing, checking device '%s' addr=%I64x\n",
                           device->props.name, device->props.address.ullLong );
                    LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
                    {
                        if (!svc->primary)
                            continue;
                        params->count++;
                        if (rem > params->count - 1)
                        {
                            BTH_LE_GATT_SERVICE *info = &params->services[params->count - 1];
                            memset( info, 0, sizeof( *info ) );
                            uuid_to_le( &svc->uuid, &info->ServiceUuid );
                            info->AttributeHandle = svc->handle;
                        }
                    }
                    ERR( "RADIO_GET_LE_DEVICE_GATT_SERVICES: re-query found %lu services\n", params->count );
                }
                LeaveCriticalSection( &device->props_cs );
                if (matches)
                    break;
            }
            LeaveCriticalSection( &device_list_cs );
        }

        if (status == STATUS_SUCCESS)
            irp->IoStatus.Information = min_size + params->count * sizeof( *params->services );
        break;
    }
    case IOCTL_WINEBTH_RADIO_GET_DEVICE_CONNECTION_STATUS:
    {
        struct winebth_radio_get_device_connection_status_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        BTH_ADDR device_addr;
        BOOL found = FALSE;

        if (!params || insize < sizeof( *params ) || outsize < sizeof( *params ))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        device_addr = params->address;
        status = STATUS_NOT_FOUND;
        params->connected = FALSE;

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                      device->props.address.ullLong == device_addr;
            if (matches)
            {
                params->connected = device->props.connected;
                found = TRUE;
                status = STATUS_SUCCESS;
                TRACE( "GET_DEVICE_CONNECTION_STATUS: device %s connected=%d\n",
                       device->props.name, device->props.connected );
            }
            LeaveCriticalSection( &device->props_cs );
            if (found)
                break;
        }
        LeaveCriticalSection( &device_list_cs );

        if (status == STATUS_SUCCESS)
            irp->IoStatus.Information = sizeof( *params );
        break;
    }
    case IOCTL_WINEBTH_RADIO_GET_LE_DEVICE_GATT_CHARACTERISTICS:
    {
        const SIZE_T min_size = offsetof( struct winebth_radio_get_le_device_gatt_characteristics_params, characteristics[0] );
        struct winebth_radio_get_le_device_gatt_characteristics_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        BTH_ADDR device_addr;
        SIZE_T rem;
        BOOL found = FALSE;
        GUID svc_uuid;
        winebluetooth_device_t device_handle = {0};
        BOOL need_wait = FALSE;

        ERR( "IOCTL_WINEBTH_RADIO_GET_LE_DEVICE_GATT_CHARACTERISTICS handler called!\n" );

        if (!params || outsize < min_size || insize < min_size)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        device_addr = params->address;
        le_to_uuid( &params->service.ServiceUuid, &svc_uuid );
        rem = (outsize - min_size) / sizeof( *params->characteristics );
        status = STATUS_NOT_FOUND;
        params->count = 0;

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                      device->props.address.ullLong == device_addr;
            if (matches)
            {
                struct bluetooth_gatt_service *svc;

                TRACE( "RADIO_GET_LE_DEVICE_GATT_CHARACTERISTICS: found device '%s', looking for service\n",
                       device->props.name );

                found = TRUE;
                LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
                {
                    if (IsEqualGUID( &svc->uuid, &svc_uuid ) && svc->handle == params->service.AttributeHandle)
                    {
                        struct bluetooth_gatt_characteristic *chrc;

                        TRACE( "RADIO_GET_LE_DEVICE_GATT_CHARACTERISTICS: found service, enumerating characteristics\n" );
                        status = STATUS_SUCCESS;
                        if (list_empty( &svc->characteristics ))
                        {
                            need_wait = TRUE;
                            winebluetooth_device_dup(( device_handle = device->device ));
                            ERR( "RADIO_GET_LE_DEVICE_GATT_CHARACTERISTICS: characteristics empty, will wait for discovery\n" );
                        }
                        else
                        {
                            LIST_FOR_EACH_ENTRY( chrc, &svc->characteristics, struct bluetooth_gatt_characteristic, entry )
                            {
                                params->count++;
                                if (rem)
                                {
                                    params->characteristics[params->count - 1] = chrc->props;
                                    rem--;
                                }
                            }
                        }
                        break;
                    }
                }
            }
            LeaveCriticalSection( &device->props_cs );
            if (found)
                break;
        }
        LeaveCriticalSection( &device_list_cs );

        if (need_wait && device_handle.handle)
        {
            ERR( "RADIO_GET_LE_DEVICE_GATT_CHARACTERISTICS: waiting for characteristic discovery...\n" );
            winebluetooth_device_start_pairing( device_handle, NULL );
            winebluetooth_device_free( device_handle );

            rem = (outsize - min_size) / sizeof( *params->characteristics );
            params->count = 0;
            EnterCriticalSection( &device_list_cs );
            LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
            {
                BOOL matches;
                EnterCriticalSection( &device->props_cs );
                matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                          device->props.address.ullLong == device_addr;
                if (matches)
                {
                    struct bluetooth_gatt_service *svc;
                    LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
                    {
                        if (IsEqualGUID( &svc->uuid, &svc_uuid ) && svc->handle == params->service.AttributeHandle)
                        {
                            struct bluetooth_gatt_characteristic *chrc;
                            LIST_FOR_EACH_ENTRY( chrc, &svc->characteristics, struct bluetooth_gatt_characteristic, entry )
                            {
                                params->count++;
                                if (rem)
                                {
                                    params->characteristics[params->count - 1] = chrc->props;
                                    rem--;
                                }
                            }
                            ERR( "RADIO_GET_LE_DEVICE_GATT_CHARACTERISTICS: after wait, found %lu characteristics\n", params->count );
                            break;
                        }
                    }
                }
                LeaveCriticalSection( &device->props_cs );
                if (matches)
                    break;
            }
            LeaveCriticalSection( &device_list_cs );
        }

        /* Sort characteristics by UUID - some apps (e.g. Square Golf) stop iterating early
         * and expect specific characteristics to appear before others. Windows appears to
         * return characteristics sorted by UUID. */
        if (params->count > 1)
            qsort( params->characteristics, params->count, sizeof(params->characteristics[0]),
                   compare_characteristics_by_uuid );

        if (status == STATUS_SUCCESS)
            irp->IoStatus.Information = min_size + params->count * sizeof( *params->characteristics );
        break;
    }
    case IOCTL_WINEBTH_RADIO_READ_NOTIFICATION:
    {
        const SIZE_T min_size = offsetof( struct winebth_radio_read_notification_params, data[0] );
        struct winebth_radio_read_notification_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        BTH_ADDR device_addr;
        BOOL found = FALSE;
        GUID svc_uuid;

        ERR( "IOCTL_WINEBTH_RADIO_READ_NOTIFICATION handler called!\n" );

        if (!params || outsize < min_size || insize < min_size)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        device_addr = params->address;
        le_to_uuid( &params->service.ServiceUuid, &svc_uuid );
        status = STATUS_NOT_FOUND;

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                      device->props.address.ullLong == device_addr;
            if (matches)
            {
                struct bluetooth_gatt_service *svc;
                found = TRUE;
                LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
                {
                    if ((IsEqualGUID( &svc->uuid, &svc_uuid ) && svc->handle == params->service.AttributeHandle) ||
                        (IsEqualGUID( &svc_uuid, &GUID_NULL ) && svc->handle == params->characteristic.ServiceHandle))
                    {
                        struct bluetooth_gatt_characteristic *chrc;
                        chrc = find_gatt_characteristic( svc, &params->characteristic );
                        if (chrc)
                        {
                            winebluetooth_gatt_characteristic_t characteristic = chrc->characteristic;
                            unsigned int data_size = 0;
                            unsigned int buffer_size = outsize >= min_size ? (unsigned int)(outsize - min_size) : 0;

                            if (!characteristic.handle)
                            {
                                ERR( "GATT characteristic handle is NULL!\n" );
                                status = STATUS_DEVICE_NOT_CONNECTED;
                                break;
                            }

                            LeaveCriticalSection( &device->props_cs );
                            LeaveCriticalSection( &device_list_cs );

                            status = winebluetooth_gatt_characteristic_read_notification(
                                characteristic,
                                params->data,
                                buffer_size,
                                &data_size );

                            winebluetooth_gatt_characteristic_free( characteristic );

                            if (status == STATUS_SUCCESS)
                                irp->IoStatus.Information = min_size + data_size;
                            else
                                irp->IoStatus.Information = 0;

                            irp->IoStatus.Status = status;
                            IoCompleteRequest( irp, IO_NO_INCREMENT );
                            return status;
                        }
                        else
                            status = STATUS_INVALID_PARAMETER;
                        break;
                    }
                }
            }
            LeaveCriticalSection( &device->props_cs );
            if (found)
                break;
        }
        LeaveCriticalSection( &device_list_cs );
        break;
    }
    case IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC:
    {
        const SIZE_T min_size = offsetof( struct winebth_radio_read_characteristic_params, data[0] );
        struct winebth_radio_read_characteristic_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        BTH_ADDR device_addr;
        BOOL found = FALSE;
        GUID svc_uuid;
        winebluetooth_device_t device_handle = {0};
        BOOL need_connect = FALSE;

        ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: addr=%I64x insize=%u outsize=%u min=%Iu\n",
             params ? params->address : 0, (unsigned int)insize, (unsigned int)outsize, min_size );

        if (!params || outsize < min_size || insize < min_size)
        {
            ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: invalid buffer\n" );
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        device_addr = params->address;
        le_to_uuid( &params->service.ServiceUuid, &svc_uuid );

    retry:
        status = STATUS_NOT_FOUND;

        ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: looking for device addr=%I64x svc=%s char_handle=%u\n",
             device_addr, debugstr_guid( &svc_uuid ), params->characteristic.AttributeHandle );

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                      device->props.address.ullLong == device_addr;
            if (matches)
            {
                struct bluetooth_gatt_service *svc;

                if (!device->props.connected && !need_connect)
                {
                    ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: device not connected, triggering connect\n" );
                    need_connect = TRUE;
                    winebluetooth_device_dup(( device_handle = device->device ));
                    LeaveCriticalSection( &device->props_cs );
                    break;
                }

                found = TRUE;
                ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: found device, scanning %u services\n",
                     list_count( &device->gatt_services ) );
                LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
                {
                    ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: checking svc=%s handle=%u\n",
                         debugstr_guid( &svc->uuid ), svc->handle );
                    if ((IsEqualGUID( &svc->uuid, &svc_uuid ) && svc->handle == params->service.AttributeHandle) ||
                        (IsEqualGUID( &svc_uuid, &GUID_NULL ) && svc->handle == params->characteristic.ServiceHandle))
                    {
                        struct bluetooth_gatt_characteristic *chrc;
                        ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: service match! looking for char\n" );
                        chrc = find_gatt_characteristic( svc, &params->characteristic );
                        if (chrc)
                        {
                            winebluetooth_gatt_characteristic_t characteristic = chrc->characteristic;
                            unsigned int data_size = 0;
                            unsigned int buffer_size = outsize >= min_size ? (unsigned int)(outsize - min_size) : 0;

                            LeaveCriticalSection( &device->props_cs );
                            LeaveCriticalSection( &device_list_cs );

                            ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: calling read (blocking) buf_size=%u\n", buffer_size );
                            status = winebluetooth_gatt_characteristic_read(
                                characteristic,
                                params->data,
                                buffer_size,
                                &data_size );
                            ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: read returned status=0x%x data_size=%u\n",
                                 (unsigned int)status, data_size );

                            winebluetooth_gatt_characteristic_free( characteristic );

                            if (status == STATUS_SUCCESS)
                            {
                                params->data_size = data_size;
                                irp->IoStatus.Information = min_size + data_size;
                            }
                            else
                                irp->IoStatus.Information = 0;

                            irp->IoStatus.Status = status;
                            IoCompleteRequest( irp, IO_NO_INCREMENT );
                            return status;
                        }
                        else
                        {
                            ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: char NOT found\n" );
                            status = STATUS_INVALID_PARAMETER;
                        }
                        break;
                    }
                }
            }
            LeaveCriticalSection( &device->props_cs );
            if (found)
                break;
        }
        LeaveCriticalSection( &device_list_cs );
        break;

        if (need_connect && device_handle.handle)
        {
            ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: connecting...\n" );
            winebluetooth_device_start_pairing( device_handle, NULL );
            winebluetooth_device_free( device_handle );
            device_handle.handle = 0;
            Sleep( 500 );
            goto retry;
        }

        if (!found)
            ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: device NOT found\n" );
        ERR( "IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC: final status=0x%x\n", (unsigned int)status );
        break;
    }
    case IOCTL_WINEBTH_RADIO_WRITE_CHARACTERISTIC:
    {
        const SIZE_T min_size = offsetof( struct winebth_radio_write_characteristic_params, data[0] );
        struct winebth_radio_write_characteristic_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        BTH_ADDR device_addr;
        BOOL found = FALSE;
        GUID svc_uuid;

        ERR( "IOCTL_WINEBTH_RADIO_WRITE_CHARACTERISTIC handler called!\n" );

        if (!params || insize < min_size)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        if (insize < min_size + params->data_size)
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        device_addr = params->address;
        le_to_uuid( &params->service.ServiceUuid, &svc_uuid );
        status = STATUS_NOT_FOUND;

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                      device->props.address.ullLong == device_addr;
            if (matches)
            {
                struct bluetooth_gatt_service *svc;
                found = TRUE;
                LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
                {
                    if ((IsEqualGUID( &svc->uuid, &svc_uuid ) && svc->handle == params->service.AttributeHandle) ||
                        (IsEqualGUID( &svc_uuid, &GUID_NULL ) && svc->handle == params->characteristic.ServiceHandle))
                    {
                        struct bluetooth_gatt_characteristic *chrc;
                        chrc = find_gatt_characteristic( svc, &params->characteristic );
                        if (chrc)
                        {
                            winebluetooth_gatt_characteristic_t characteristic = chrc->characteristic;

                            LeaveCriticalSection( &device->props_cs );
                            LeaveCriticalSection( &device_list_cs );

                            status = winebluetooth_gatt_characteristic_write(
                                characteristic,
                                params->data,
                                params->data_size,
                                params->write_type );

                            winebluetooth_gatt_characteristic_free( characteristic );

                            if (status == STATUS_SUCCESS)
                                irp->IoStatus.Information = min_size + params->data_size;
                            else
                                irp->IoStatus.Information = 0;

                            irp->IoStatus.Status = status;
                            IoCompleteRequest( irp, IO_NO_INCREMENT );
                            return status;
                        }
                        else
                            status = STATUS_INVALID_PARAMETER;
                        break;
                    }
                }
            }
            LeaveCriticalSection( &device->props_cs );
            if (found)
                break;
        }
        LeaveCriticalSection( &device_list_cs );
        break;
    }
    case IOCTL_WINEBTH_RADIO_SET_NOTIFY:
    {
        struct winebth_radio_set_notify_params *params = irp->AssociatedIrp.SystemBuffer;
        struct bluetooth_remote_device *device;
        BTH_ADDR device_addr;
        BOOL found = FALSE;
        GUID svc_uuid;

        ERR( "IOCTL_WINEBTH_RADIO_SET_NOTIFY handler called!\n" );

        if (!params || insize < sizeof(*params))
        {
            status = STATUS_INVALID_USER_BUFFER;
            break;
        }

        device_addr = params->address;
        le_to_uuid( &params->service.ServiceUuid, &svc_uuid );
        status = STATUS_NOT_FOUND;

        EnterCriticalSection( &device_list_cs );
        LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
        {
            BOOL matches;
            EnterCriticalSection( &device->props_cs );
            matches = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                      device->props.address.ullLong == device_addr;
            if (matches)
            {
                struct bluetooth_gatt_service *svc;
                found = TRUE;
                LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
                {
                    if ((IsEqualGUID( &svc->uuid, &svc_uuid ) && svc->handle == params->service.AttributeHandle) ||
                        (IsEqualGUID( &svc_uuid, &GUID_NULL ) && svc->handle == params->characteristic.ServiceHandle))
                    {
                        struct bluetooth_gatt_characteristic *chrc;
                        chrc = find_gatt_characteristic( svc, &params->characteristic );
                        if (chrc)
                        {
                            status = winebluetooth_gatt_characteristic_set_notify(
                                chrc->characteristic,
                                params->enable );
                            if (status == STATUS_SUCCESS)
                                irp->IoStatus.Information = sizeof(*params);
                            else
                                irp->IoStatus.Information = 0;
                        }
                        else
                            status = STATUS_INVALID_PARAMETER;
                        break;
                    }
                }
            }
            LeaveCriticalSection( &device->props_cs );
            if (found)
                break;
        }
        LeaveCriticalSection( &device_list_cs );
        break;
    }
    default:
        FIXME( "Unimplemented IOCTL code: %#lx\n", code );
        break;
    }

    if (status != STATUS_PENDING)
    {
        irp->IoStatus.Status = status;
        IoCompleteRequest( irp, IO_NO_INCREMENT );
    }
    return status;
}

static NTSTATUS WINAPI dispatch_bluetooth( DEVICE_OBJECT *device, IRP *irp )
{
    struct bluetooth_pdo_ext *ext = device->DeviceExtension;
    TRACE( "(%p, %p)\n", device, irp );

    if (device == device_auth)
        return dispatch_auth( device, irp );

    switch (ext->type)
    {
    case BLUETOOTH_PDO_EXT_RADIO:
        return bluetooth_radio_dispatch( device, &ext->radio, irp );
    case BLUETOOTH_PDO_EXT_REMOTE_DEVICE:
        return bluetooth_remote_device_dispatch( device, &ext->remote_device, irp );
    DEFAULT_UNREACHABLE;
    }
}

void WINAPIV append_id( struct string_buffer *buffer, const WCHAR *format, ... )
{
    va_list args;
    WCHAR *string;
    int len;

    va_start( args, format );

    len = _vsnwprintf( NULL, 0, format, args ) + 1;
    if (!(string = ExAllocatePool( PagedPool, (buffer->len + len) * sizeof( WCHAR ) )))
    {
        if (buffer->string)
            ExFreePool( buffer->string );
        buffer->string = NULL;
        return;
    }
    if (buffer->string)
    {
        memcpy( string, buffer->string, buffer->len * sizeof( WCHAR ) );
        ExFreePool( buffer->string );
    }
    _vsnwprintf( string + buffer->len, len, format, args );
    buffer->string = string;
    buffer->len += len;

    va_end( args );
}


static HANDLE event_loop_thread;
static NTSTATUS radio_get_hw_name_w( winebluetooth_radio_t radio, WCHAR **name )
{
    char *name_a;
    SIZE_T size = sizeof( char ) *  256;
    NTSTATUS status;

    name_a = malloc( size );
    if (!name_a)
        return STATUS_NO_MEMORY;

    status = winebluetooth_radio_get_unique_name( radio, name_a, &size );
    if (status == STATUS_BUFFER_TOO_SMALL)
    {
        void *ptr = realloc( name_a, size );
        if (!ptr)
        {
            free( name_a );
            return STATUS_NO_MEMORY;
        }
        name_a = ptr;
        status = winebluetooth_radio_get_unique_name( radio, name_a, &size );
    }
    if (status != STATUS_SUCCESS)
    {
        free( name_a );
        return status;
    }

    *name = malloc( (mbstowcs( NULL, name_a, 0 ) + 1) * sizeof( WCHAR ));
    if (!*name)
    {
        free( name_a );
        return status;
    }

    mbstowcs( *name, name_a, strlen( name_a ) + 1 );
    free( name_a );
    return STATUS_SUCCESS;
}

static WCHAR *sanitize_hw_name( const WCHAR *input, unsigned int fallback_idx )
{
    WCHAR sanitized[256];
    unsigned int out_len = 0;
    const WCHAR *p;

    sanitized[0] = 0;
    if (input)
    {
        for (p = input; *p && out_len < ARRAY_SIZE( sanitized ) - 1; p++)
        {
            WCHAR c = *p;
            if (c >= L'a' && c <= L'z') c -= 32; /* uppercase */
            if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9'))
                sanitized[out_len++] = c;
        }
        sanitized[out_len] = 0;
    }

    if (!out_len)
        swprintf( sanitized, ARRAY_SIZE( sanitized ), L"RADIO%u", fallback_idx );

    return wcsdup( sanitized );
}
static void add_bluetooth_radio( struct winebluetooth_watcher_event_radio_added event )
{
    struct bluetooth_pdo_ext *ext;
    DEVICE_OBJECT *device_obj;
    UNICODE_STRING string;
    NTSTATUS status;
    WCHAR name[256];
    WCHAR *hw_name;
    WCHAR *sanitized_hw_name;
    static unsigned int radio_index;

    swprintf( name, ARRAY_SIZE( name ), L"\\Device\\WINEBTH-RADIO-%d", radio_index++ );
    TRACE( "Adding new bluetooth radio %p: %s\n", (void *)event.radio.handle, debugstr_w( name ) );

    status = radio_get_hw_name_w( event.radio, &hw_name );
    if (status)
    {
        ERR( "Failed to get hardware name for radio %p, status %#lx\n", (void *)event.radio.handle, status );
        return;
    }

    sanitized_hw_name = sanitize_hw_name( hw_name, radio_index );
    free( hw_name );
    if (!sanitized_hw_name)
    {
        ERR( "Failed to sanitize hardware name for radio %p\n", (void *)event.radio.handle );
        return;
    }

    RtlInitUnicodeString( &string, name );
    status = IoCreateDevice( driver_obj, sizeof( *ext ), &string, FILE_DEVICE_BLUETOOTH, 0,
                             FALSE, &device_obj );
    if (status)
    {
        ERR( "Failed to create device, status %#lx\n", status );
        return;
    }

    ext = device_obj->DeviceExtension;
    ext->type = BLUETOOTH_PDO_EXT_RADIO;
    ext->radio.device_obj = device_obj;
    ext->radio.radio = event.radio;
    ext->radio.refcount = 1;
    ext->radio.state = BLUETOOTH_STATE_INITIALIZING;
    ext->radio.hw_name = sanitized_hw_name;
    ext->radio.props = event.props;
    ext->radio.props_mask = event.props_mask;
    ext->radio.next_device_index = 0;
    ext->radio.index = radio_index - 1;
    /* instance_prefix like RADIO0 */
    {
        WCHAR tmp[16];
        swprintf( tmp, ARRAY_SIZE( tmp ), L"RADIO%u", ext->radio.index );
        ext->radio.instance_prefix = wcsdup( tmp );
    }
    list_init( &ext->radio.remote_devices );

    InitializeListHead( &ext->radio.irp_list );

    EnterCriticalSection( &device_list_cs );
    list_add_tail( &device_list, &ext->radio.entry );
    LeaveCriticalSection( &device_list_cs );

    IoInvalidateDeviceRelations( bus_pdo, BusRelations );
}

static void remove_bluetooth_radio( winebluetooth_radio_t radio )
{
    struct bluetooth_radio *device;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( device, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( radio, device->radio ) && device->state != BLUETOOTH_STATE_REMOVING)
        {
            TRACE( "Removing bluetooth radio %p\n", (void *)radio.handle );
            device->state = BLUETOOTH_STATE_REMOVING;
            list_remove( &device->entry );
            IoInvalidateDeviceRelations( device->device_obj, BusRelations );
            break;
        }
    }
    LeaveCriticalSection( &device_list_cs );

    IoInvalidateDeviceRelations( bus_pdo, BusRelations );
    winebluetooth_radio_free( radio );
}

static void bluetooth_radio_set_properties( DEVICE_OBJECT *obj,
                                            winebluetooth_radio_props_mask_t mask,
                                            struct winebluetooth_radio_properties *props );

static void update_bluetooth_radio_properties( struct winebluetooth_watcher_event_radio_props_changed event )
{
    struct bluetooth_radio *device;
    winebluetooth_radio_t radio = event.radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( device, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( radio, device->radio ) && device->state != BLUETOOTH_STATE_REMOVING)
        {
            device->props_mask |= event.changed_props_mask;
            device->props_mask &= ~event.invalid_props_mask;

            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_NAME)
                memcpy( device->props.name, event.props.name, sizeof( event.props.name ));
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_ADDRESS)
                device->props.address.ullLong = event.props.address.ullLong;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERABLE)
                device->props.discoverable = event.props.discoverable;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_CONNECTABLE)
                device->props.connectable = event.props.connectable;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_CLASS)
                device->props.class = event.props.class;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_MANUFACTURER)
                device->props.manufacturer = event.props.manufacturer;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_VERSION)
                device->props.version = event.props.version;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERING)
                device->props.discovering = event.props.discovering;
            if (event.changed_props_mask & WINEBLUETOOTH_RADIO_PROPERTY_PAIRABLE)
                device->props.pairable = event.props.pairable;
            if (device->state == BLUETOOTH_STATE_ACTIVE)
                bluetooth_radio_set_properties( device->device_obj, device->props_mask,
                                                &device->props );
            break;
        }
    }
    LeaveCriticalSection( &device_list_cs );
    winebluetooth_radio_free( radio );
}

/* IoReportTargetDeviceChange is not available in Wine Crossover - skip PnP notifications.
 * The WinRT layer uses IOCTL polling to discover devices so this is not critical. */
static void bluetooth_radio_report_radio_in_range_event( DEVICE_OBJECT *radio_obj, ULONG remote_device_old_flags,
                                                         const BTH_DEVICE_INFO *new_device_info )
{
    /* PnP notifications disabled for Wine Crossover compatibility */
}

static NTSTATUS bluetooth_device_set_properties( struct bluetooth_remote_device *device,
                                                 const BYTE *adapter_addr,
                                                 const struct winebluetooth_device_properties *props,
                                                 winebluetooth_device_props_mask_t mask,
                                                 BOOL is_le );

static void bluetooth_radio_add_remote_device( struct winebluetooth_watcher_event_device_added event )
{
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( event.radio, radio->radio ))
        {
            struct bluetooth_remote_device *existing;
            struct bluetooth_pdo_ext *ext;
            DEVICE_OBJECT *device_obj;
            NTSTATUS status;
            UNICODE_STRING dev_name;
            WCHAR name_buf[128];

            LIST_FOR_EACH_ENTRY( existing, &radio->remote_devices, struct bluetooth_remote_device, entry )
            {
                if (existing->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
                    existing->props.address.ullLong == event.props.address.ullLong &&
                    existing->state != BLUETOOTH_STATE_REMOVING)
                {
                    TRACE( "Device with address %I64x already exists, updating properties\n", event.props.address.ullLong );
                    EnterCriticalSection( &existing->props_cs );
                    existing->props_mask = event.known_props_mask;
                    existing->props = event.props;
                    LeaveCriticalSection( &existing->props_cs );
                    LeaveCriticalSection( &device_list_cs );
                    winebluetooth_device_free( event.device );
                    return;
                }
            }

            /* Create a named PDO similar to Linux's path style for stable instance IDs */
            swprintf( name_buf, ARRAY_SIZE( name_buf ), L"\\Device\\WINEBTH-DEV-%u", radio->next_device_index++ );
            RtlInitUnicodeString( &dev_name, name_buf );

            TRACE( "Creating remote device with name %s\n", debugstr_w( dev_name.Buffer ) );

            status = IoCreateDevice( driver_obj, sizeof( *ext ), &dev_name, FILE_DEVICE_BLUETOOTH,
                                     0, FALSE, &device_obj );
            if (status)
            {
                ERR( "Failed to create remote device, status %#lx\n", status );
                winebluetooth_device_free( event.device );
                break;
            }

            ext = device_obj->DeviceExtension;
            ext->type = BLUETOOTH_PDO_EXT_REMOTE_DEVICE;
            ext->remote_device.radio = radio;

            ext->remote_device.device_obj = device_obj;
            InitializeCriticalSectionEx( &ext->remote_device.props_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
            ext->remote_device.props_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": bluetooth_pdo_ext.props_cs");
            winebluetooth_device_dup( event.device );
            ext->remote_device.device = event.device;
            TRACE( "Stored device.handle=%p for device '%s' device_obj=%p ext=%p\n",
                   (void*)event.device.handle, event.props.name, device_obj, ext );
            ext->remote_device.props_mask = event.known_props_mask;
            ext->remote_device.props = event.props;
            TRACE( "Stored device '%s' address 0x%llx\n", event.props.name, (unsigned long long)event.props.address.ullLong );
            ext->remote_device.refcount = 1;
            ext->remote_device.state = BLUETOOTH_STATE_INITIALIZING;

            ext->remote_device.le = TRUE;
            list_init( &ext->remote_device.gatt_services );
            ext->remote_device.bthle_symlink_name.Buffer = NULL;

            if (!event.init_entry)
            {
                BTH_DEVICE_INFO device_info = {0};
                winebluetooth_device_properties_to_info( ext->remote_device.props_mask, &ext->remote_device.props, &device_info );
                bluetooth_radio_report_radio_in_range_event( radio->device_obj, 0, &device_info );
            }

            /* Prioritize devices with names by adding them at the head of the list.
             * This ensures named devices (like SquareGolf) get PnP enumerated quickly
             * instead of waiting behind hundreds of unnamed devices. */
            if (event.props.name[0])
                list_add_head( &radio->remote_devices, &ext->remote_device.entry );
            else
                list_add_tail( &radio->remote_devices, &ext->remote_device.entry );

            IoInvalidateDeviceRelations( radio->device_obj, BusRelations );
            break;
        }
    }
    LeaveCriticalSection( &device_list_cs );

    /* Device interface will be registered in IRP_MN_START_DEVICE or bluetooth_device_enable_le_iface.
     * Attempting to register here causes deadlocks because IoRegisterDeviceInterface triggers
     * PnP operations that callback into our driver while we're still processing. */
    winebluetooth_radio_free( event.radio );
    winebluetooth_device_free( event.device );
}

static void bluetooth_radio_remove_remote_device( struct winebluetooth_watcher_event_device_removed event )
{
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device, *next;

        LIST_FOR_EACH_ENTRY_SAFE( device, next, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (winebluetooth_device_equal( event.device, device->device ))
            {
                BOOL has_addr;

                TRACE( "Removing bluetooth remote device %p\n", (void *)device->device.handle );

                device->state = BLUETOOTH_STATE_REMOVING;
                list_remove( &device->entry );

                EnterCriticalSection( &device->props_cs );
                has_addr = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS;
                LeaveCriticalSection( &device->props_cs );

                /* PnP out-of-range notification skipped for Wine Crossover compatibility */
                (void)has_addr;
                LeaveCriticalSection( &device_list_cs );
                IoInvalidateDeviceRelations( radio->device_obj, BusRelations );
                winebluetooth_device_free( event.device );
                return;
            }
        }
    }
    LeaveCriticalSection( &device_list_cs );
    winebluetooth_device_free( event.device );
}

/* Uses only device->device_obj and passed parameters; no lock required. */
static NTSTATUS bluetooth_device_set_properties( struct bluetooth_remote_device *device,
                                                 const BYTE *adapter_addr,
                                                 const struct winebluetooth_device_properties *props,
                                                 winebluetooth_device_props_mask_t mask,
                                                 BOOL is_le )
{
    NTSTATUS status = STATUS_SUCCESS, tmp;

    if (mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS)
    {
        WCHAR addr_str[64], aep_id[256];
        const WCHAR *connection;
        const BYTE *device_addr = props->address.rgBytes;

        connection = is_le ? L"BluetoothLE#BluetoothLE" : L"Bluetooth#Bluetooth";
        swprintf( aep_id, ARRAY_SIZE( aep_id ), L"%s#%s%02x:%02x:%02x:%02x:%02x:%02x-%02x:%02x:%02x:%02x:%02x:%02x",
                  connection, connection, adapter_addr[0], adapter_addr[1], adapter_addr[2], adapter_addr[3],
                  adapter_addr[4], adapter_addr[5], device_addr[0], device_addr[1], device_addr[2], device_addr[3],
                  device_addr[4], device_addr[5] );
        tmp = IoSetDevicePropertyData( device->device_obj, (DEVPROPKEY *)&PKEY_Devices_Aep_AepId, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_STRING, (lstrlenW( aep_id ) + 1) * sizeof(WCHAR), aep_id );
        if (tmp) { ERR("IoSetDevicePropertyData(AepId) failed: %#lx\n", tmp ); status = tmp; }

        swprintf( addr_str, ARRAY_SIZE( addr_str ), L"%02x%02x%02x%02x%02x%02x", device_addr[0], device_addr[1],
                  device_addr[2], device_addr[3], device_addr[4], device_addr[5] );
        tmp = IoSetDevicePropertyData( device->device_obj, &DEVPKEY_Bluetooth_DeviceAddress, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_STRING, (lstrlenW( addr_str ) + 1) * sizeof(WCHAR), addr_str );
        if (tmp) { ERR("IoSetDevicePropertyData(DeviceAddress) failed: %#lx\n", tmp ); status = tmp; }
        /* IoSetDeviceInterfacePropertyData skipped for Wine Crossover compatibility */

        swprintf( addr_str, ARRAY_SIZE( addr_str ), L"%02x:%02x:%02x:%02x:%02x:%02x", device_addr[0], device_addr[1],
                  device_addr[2], device_addr[3], device_addr[4], device_addr[5] );
        tmp = IoSetDevicePropertyData( device->device_obj, (DEVPROPKEY *)&PKEY_Devices_Aep_DeviceAddress, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_STRING, (lstrlenW( addr_str ) + 1) * sizeof(WCHAR), addr_str );
        if (tmp) { ERR("IoSetDevicePropertyData(Aep_DeviceAddress) failed: %#lx\n", tmp ); status = tmp; }
    }
    if (mask & WINEBLUETOOTH_DEVICE_PROPERTY_CLASS)
    {
         tmp = IoSetDevicePropertyData( device->device_obj, &DEVPKEY_Bluetooth_ClassOfDevice, LOCALE_NEUTRAL, 0,
                                  DEVPROP_TYPE_UINT32, sizeof( props->class ), (void *)&props->class );
         if (tmp) { ERR("IoSetDevicePropertyData(ClassOfDevice) failed: %#lx\n", tmp ); status = tmp; }
    }
    if (mask & WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED && props->connected)
    {
        FILETIME time = {0};

        GetSystemTimeAsFileTime( &time );
        tmp = IoSetDevicePropertyData( device->device_obj, &DEVPKEY_Bluetooth_LastConnectedTime, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_FILETIME, sizeof( time ), (void *)&time );
        if (tmp) { ERR("IoSetDevicePropertyData(LastConnectedTime) failed: %#lx\n", tmp ); status = tmp; }
        /* IoSetDeviceInterfacePropertyData(LastConnectedTime) skipped for Wine Crossover compatibility */
    }
    if (mask & WINEBLUETOOTH_DEVICE_PROPERTY_NAME && props->name[0])
    {
        WCHAR name_w[BLUETOOTH_MAX_NAME_SIZE];
        DEVPROPKEY friendly = DEVPKEY_Device_FriendlyName;
        DEVPROPKEY devdesc = DEVPKEY_Device_DeviceDesc;
        NTSTATUS name_status;

        MultiByteToWideChar( CP_UTF8, 0, props->name, -1, name_w, ARRAY_SIZE( name_w ) );
        TRACE("Setting device properties for '%s'\n", props->name);

        name_status = IoSetDevicePropertyData( device->device_obj, &friendly, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_STRING, (wcslen( name_w ) + 1) * sizeof( WCHAR ), name_w );
        if (name_status) { ERR("IoSetDevicePropertyData(FriendlyName) failed: %#lx\n", name_status); status = name_status; }
        name_status = IoSetDevicePropertyData( device->device_obj, &devdesc, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_STRING, (wcslen( name_w ) + 1) * sizeof( WCHAR ), name_w );
        name_status = IoSetDevicePropertyData( device->device_obj, &DEVPKEY_NAME, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_STRING, (wcslen( name_w ) + 1) * sizeof( WCHAR ), name_w );
        /* IoSetDeviceInterfacePropertyData(FriendlyName) skipped for Wine Crossover compatibility */
    }
    /* Paired key not available in our headers; skip */
    /* Trusted/Legacy keys not defined in our headers; skip */

    /* IoSetDeviceInterfacePropertyData(Enabled) skipped for Wine Crossover compatibility */

    return status;
}

static void bluetooth_radio_update_device_props( struct winebluetooth_watcher_event_device_props_changed event )
{
    BTH_DEVICE_INFO device_new_info = {0};
    DEVICE_OBJECT *radio_obj = NULL; /* The radio PDO the remote device exists on. */
    struct bluetooth_radio *radio;
    ULONG device_old_flags = 0;
    int device_count = 0;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            BTH_DEVICE_INFO old_info = {0};
            BLUETOOTH_ADDRESS adapter_addr;

            device_count++;
            if (winebluetooth_device_equal( event.device, device->device ))
            {
                adapter_addr = radio->props.address;
                EnterCriticalSection( &device->props_cs );

                device->props_mask |= event.changed_props_mask;
                device->props_mask &= ~event.invalid_props_mask;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_NAME)
                    memcpy( device->props.name, event.props.name, sizeof( event.props.name ));
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS)
                    device->props.address = event.props.address;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED)
                {
                    ERR("=== bluetooth_radio_update_device_props: UPDATING device connected: %d -> %d ===\n",
                        device->props.connected, event.props.connected);
                    device->props.connected = event.props.connected;
                }
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_PAIRED)
                    device->props.paired = event.props.paired;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_LEGACY_PAIRING)
                    device->props.legacy_pairing = event.props.legacy_pairing;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_TRUSTED)
                    device->props.trusted = event.props.trusted;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_CLASS)
                    device->props.class = event.props.class;
                winebluetooth_device_properties_to_info( device->props_mask, &device->props, &device_new_info );
                bluetooth_device_set_properties( device, adapter_addr.rgBytes, &device->props, device->props_mask, device->le );
                LeaveCriticalSection( &device->props_cs );

                device_old_flags = old_info.flags;
                goto done;
            }
        }
    }
done:
    winebluetooth_device_free( event.device );

    if (radio_obj)
        bluetooth_radio_report_radio_in_range_event( radio_obj, device_old_flags, &device_new_info );

    LeaveCriticalSection( &device_list_cs );
}

static void bluetooth_radio_report_auth_event( struct winebluetooth_auth_event event )
{
    TARGET_DEVICE_CUSTOM_NOTIFICATION *notification;
    struct winebth_authentication_request *request;
    struct bluetooth_radio *radio;
    SIZE_T notif_size;

    notif_size = offsetof( TARGET_DEVICE_CUSTOM_NOTIFICATION, CustomDataBuffer[sizeof( *request )] );
    notification = ExAllocatePool( PagedPool, notif_size );
    if (!notification)
        return;

    notification->Version = 1;
    notification->Size = notif_size;
    notification->Event = GUID_WINEBTH_AUTHENTICATION_REQUEST;
    notification->FileObject = NULL;
    notification->NameBufferOffset = -1;
    request = (struct winebth_authentication_request *)notification->CustomDataBuffer;
    memset( request, 0, sizeof( *request ) );
    request->auth_method = event.method;
    request->numeric_value_or_passkey = event.numeric_value_or_passkey;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (winebluetooth_device_equal( event.device, device->device ))
            {
                NTSTATUS ret;

                EnterCriticalSection( &device->props_cs );
                winebluetooth_device_properties_to_info( device->props_mask, &device->props, &request->device_info );
                LeaveCriticalSection( &device->props_cs );
                LeaveCriticalSection( &device_list_cs );

                /* PnP auth notification skipped for Wine Crossover compatibility */
                (void)ret;

                ExFreePool( notification );
                return;
            }
        }
    }
    LeaveCriticalSection( &device_list_cs );

    ExFreePool( notification );
}

static void complete_irp( IRP *irp, NTSTATUS result )
{
    EnterCriticalSection( &device_list_cs );
    RemoveEntryList( &irp->Tail.Overlay.ListEntry );
    LeaveCriticalSection( &device_list_cs );

    irp->IoStatus.Status = result;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
}

static void bluetooth_device_enable_le_iface( struct bluetooth_remote_device *device )
{
    EnterCriticalSection( &device->props_cs );
    if (device->le)
    {
        LeaveCriticalSection( &device->props_cs );
        return;
    }
    device->le = TRUE;
    /* Register the LE interface immediately. We can't wait for IRP_MN_START_DEVICE because
     * Wine's PnP enumeration may never send it for dynamically discovered BLE devices.
     * This is a workaround - on real Windows, the interface would be registered in START_DEVICE.
     * Use address as reference string for consistent symlink format. */
    if (!device->bthle_symlink_name.Buffer)
    {
        /* Device interface registration moved to IRP_MN_START_DEVICE to avoid PnP enumeration timing issues.
         * Early registration here would fail because Wine's PnP manager hasn't processed the device yet. */
        TRACE( "LE interface will be registered during IRP_MN_START_DEVICE for device %p\n", device->device_obj );
    }
    LeaveCriticalSection( &device->props_cs );
}

static void bluetooth_device_add_gatt_service( struct winebluetooth_watcher_event_gatt_service_added event )
{
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (winebluetooth_device_equal( event.device, device->device ) && device->state != BLUETOOTH_STATE_REMOVING)
            {
                struct bluetooth_gatt_service *service;

                EnterCriticalSection( &device->props_cs );
                LIST_FOR_EACH_ENTRY( service, &device->gatt_services, struct bluetooth_gatt_service, entry )
                {
                    if (service->handle == event.attr_handle && IsEqualGUID( &service->uuid, &event.uuid ))
                    {
                        TRACE( "=== GATT service %s already exists for device %p ===\n", debugstr_guid( &event.uuid ), (void *)event.device.handle );
                        LeaveCriticalSection( &device->props_cs );
                        LeaveCriticalSection( &device_list_cs );
                        winebluetooth_device_free( event.device );
                        winebluetooth_gatt_service_free( event.service );
                        return;
                    }
                }
                LeaveCriticalSection( &device->props_cs );

                ERR( "=== Adding GATT service %s for remote device %p name='%s' address=%I64x ===\n", debugstr_guid( &event.uuid ),
                       (void *)event.device.handle, device->props.name, device->props.address.ullLong );

                service = calloc( 1, sizeof( *service ) );
                if (!service)
                {
                    ERR( "=== FAILED to allocate service! ===\n" );
                    LeaveCriticalSection( &device_list_cs );
                    return;
                }

                service->service = event.service;
                service->uuid = event.uuid;
                service->primary = !!event.is_primary;
                service->handle = event.attr_handle;
                service->refcount = 1;
                bluetooth_device_enable_le_iface( device );
                list_init( &service->characteristics );

                EnterCriticalSection( &device->props_cs );
                list_add_tail( &device->gatt_services, &service->entry );
                ERR( "=== Service added to gatt_services list! ===\n" );
                LeaveCriticalSection( &device->props_cs );
                LeaveCriticalSection( &device_list_cs );
                winebluetooth_device_free( event.device );
                return;
            }
        }
    }
    LeaveCriticalSection( &device_list_cs );

    winebluetooth_device_free( event.device );
    winebluetooth_gatt_service_free( event.service );
}

static void bluetooth_gatt_service_remove( winebluetooth_gatt_service_t service )
{
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            struct bluetooth_gatt_service *svc;

            EnterCriticalSection( &device->props_cs );
            if (!device->le)
            {
                LeaveCriticalSection( &device->props_cs );
                continue;
            }
            LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
            {
                if (winebluetooth_gatt_service_equal( svc->service, service ))
                {
                    struct bluetooth_gatt_characteristic *cur, *next;

                    list_remove( &svc->entry );
                    LeaveCriticalSection( &device->props_cs );
                    LeaveCriticalSection( &device_list_cs );
                    winebluetooth_gatt_service_free( svc->service );
                    LIST_FOR_EACH_ENTRY_SAFE( cur, next, &svc->characteristics, struct bluetooth_gatt_characteristic, entry )
                    {
                        winebluetooth_gatt_characteristic_free( cur->characteristic );
                        free( cur );
                    }
                    free( svc );
                    winebluetooth_gatt_service_free( service );
                    return;
                }
            }
            LeaveCriticalSection( &device->props_cs );
        }
    }
    LeaveCriticalSection( &device_list_cs );
    winebluetooth_gatt_service_free( service );
}

static void
bluetooth_gatt_service_add_characteristic( struct winebluetooth_watcher_event_gatt_characteristic_added characteristic )
{
    struct bluetooth_radio *radio;
    GUID char_uuid;

    le_to_uuid( &characteristic.props.CharacteristicUuid, &char_uuid );

    

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            struct bluetooth_gatt_service *svc;

            EnterCriticalSection( &device->props_cs );
            if (!device->le)
            {
                LeaveCriticalSection( &device->props_cs );
                continue;
            }
            LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
            {
                if (winebluetooth_gatt_service_equal( svc->service, characteristic.service ))
                {
                    struct bluetooth_gatt_characteristic *entry;

                    LIST_FOR_EACH_ENTRY( entry, &svc->characteristics, struct bluetooth_gatt_characteristic, entry )
                    {
                        if (entry->props.AttributeHandle == characteristic.props.AttributeHandle)
                        {
                            TRACE( "=== GATT characteristic %u already exists in service %p ===\n", characteristic.props.AttributeHandle, (void *)svc->service.handle );

                            LeaveCriticalSection( &device->props_cs );
                            LeaveCriticalSection( &device_list_cs );

                            winebluetooth_gatt_service_free( characteristic.service );
                            winebluetooth_gatt_characteristic_free( characteristic.characteristic );
                            return;
                        }
                    }

                    if (!(entry = calloc( 1, sizeof( *entry ) )))
                    {
                        LeaveCriticalSection( &device->props_cs );
                        goto failed;
                    }

                    entry->characteristic = characteristic.characteristic;
                    entry->props = characteristic.props;
                    list_add_tail( &svc->characteristics, &entry->entry );
                    LeaveCriticalSection( &device->props_cs );
                    LeaveCriticalSection( &device_list_cs );
                    winebluetooth_gatt_service_free( characteristic.service );
                    return;
                }
            }
            LeaveCriticalSection( &device->props_cs );
        }
    }
failed:
    
    LeaveCriticalSection( &device_list_cs );
    winebluetooth_gatt_characteristic_free( characteristic.characteristic );
    winebluetooth_gatt_service_free( characteristic.service );
}

static void bluetooth_gatt_characteristic_remove( winebluetooth_gatt_characteristic_t handle )
{
     struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            struct bluetooth_gatt_service *svc;

            EnterCriticalSection( &device->props_cs );
            if (!device->le)
            {
                LeaveCriticalSection( &device->props_cs );
                continue;
            }
            LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
            {
                struct bluetooth_gatt_characteristic *chrc;
                LIST_FOR_EACH_ENTRY( chrc, &svc->characteristics, struct bluetooth_gatt_characteristic, entry )
                {
                    if (winebluetooth_gatt_characteristic_equal( chrc->characteristic, handle ))
                    {
                        list_remove( &chrc->entry );
                        LeaveCriticalSection( &device->props_cs );
                        LeaveCriticalSection( &device_list_cs );

                        winebluetooth_gatt_characteristic_free( chrc->characteristic );
                        winebluetooth_gatt_characteristic_free( handle );
                        free( chrc );
                        return;
                    }
                }
            }
            LeaveCriticalSection( &device->props_cs );
        }
    }
    LeaveCriticalSection( &device_list_cs );
    winebluetooth_gatt_characteristic_free( handle );
}

static DWORD CALLBACK bluetooth_event_loop_thread_proc( void *arg )
{
    NTSTATUS status;
    while (TRUE)
    {
        struct winebluetooth_event result = {0};

        status = winebluetooth_get_event( &result );
        if (status != STATUS_PENDING) break;

        switch (result.status)
        {
            case WINEBLUETOOTH_EVENT_WATCHER_EVENT:
            {
                struct winebluetooth_watcher_event *event = &result.data.watcher_event;
                TRACE("Received watcher event type: %#x\n", event->event_type);
                switch (event->event_type)
                {
                    case BLUETOOTH_WATCHER_EVENT_TYPE_RADIO_ADDED:
                        add_bluetooth_radio( event->event_data.radio_added );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_RADIO_REMOVED:
                        remove_bluetooth_radio( event->event_data.radio_removed );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_RADIO_PROPERTIES_CHANGED:
                        update_bluetooth_radio_properties( event->event_data.radio_props_changed );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_ADDED:
                        bluetooth_radio_add_remote_device( event->event_data.device_added );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_REMOVED:
                        bluetooth_radio_remove_remote_device( event->event_data.device_removed );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_PROPERTIES_CHANGED:
                        ERR("=== RECEIVED DEVICE_PROPERTIES_CHANGED event! mask=%#x connected=%d address=%llx ===\n",
                            event->event_data.device_props_changed.changed_props_mask,
                            event->event_data.device_props_changed.props.connected,
                            (unsigned long long)event->event_data.device_props_changed.props.address.ullLong);
                        bluetooth_radio_update_device_props( event->event_data.device_props_changed);
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_PAIRING_FINISHED:
                        complete_irp( event->event_data.pairing_finished.irp,
                                      event->event_data.pairing_finished.result );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_GATT_SERVICE_ADDED:
                        ERR("=== RECEIVED GATT_SERVICE_ADDED event! service_handle=%p uuid=%s ===\n",
                            (void*)(ULONG_PTR)event->event_data.gatt_service_added.service.handle,
                            debugstr_guid(&event->event_data.gatt_service_added.uuid));
                        bluetooth_device_add_gatt_service( event->event_data.gatt_service_added );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_DEVICE_GATT_SERVICE_REMOVED:
                        bluetooth_gatt_service_remove( event->event_data.gatt_service_removed );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_GATT_CHARACTERISTIC_ADDED:
                        ERR("=== RECEIVED GATT_CHARACTERISTIC_ADDED event! char_handle=%p svc_handle=%p ===\n",
                            (void*)(ULONG_PTR)event->event_data.gatt_characteristic_added.characteristic.handle,
                            (void*)(ULONG_PTR)event->event_data.gatt_characteristic_added.service.handle);
                        bluetooth_gatt_service_add_characteristic( event->event_data.gatt_characteristic_added );
                        break;
                    case BLUETOOTH_WATCHER_EVENT_TYPE_GATT_CHARACTERISTIC_REMOVED:
                        bluetooth_gatt_characteristic_remove( event->event_data.gatt_characterisic_removed );
                        break;
                    default:
                        FIXME( "Unknown bluetooth watcher event code: %#x\n", event->event_type );
                }
                break;
            }
            case WINEBLUETOOTH_EVENT_AUTH_EVENT:
                bluetooth_radio_report_auth_event( result.data.auth_event);
                winebluetooth_device_free( result.data.auth_event.device );
                break;
            default:
                FIXME( "Unknown bluetooth event loop status code: %#x\n", result.status );
        }
    }

    if (status != STATUS_SUCCESS)
        ERR( "Bluetooth event loop terminated with %#lx", status );
    else
        TRACE( "Exiting bluetooth event loop\n" );
    return 0;
}

static NTSTATUS WINAPI fdo_pnp( DEVICE_OBJECT *device_obj, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );

    TRACE( "irp %p, minor function %s.\n", irp, debugstr_minor_function_code( stack->MinorFunction ) );

    switch (stack->MinorFunction)
    {
        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            struct bluetooth_radio *radio;
            DEVICE_RELATIONS *devices;
            SIZE_T i = 0;

            if (stack->Parameters.QueryDeviceRelations.Type != BusRelations)
            {
                FIXME( "Unhandled Device Relation %x\n",
                       stack->Parameters.QueryDeviceRelations.Type );
                break;
            }

            EnterCriticalSection( &device_list_cs );
            devices = ExAllocatePool(
                PagedPool, offsetof( DEVICE_RELATIONS, Objects[list_count( &device_list )] ) );
            if (devices == NULL)
            {
                LeaveCriticalSection( &device_list_cs );
                irp->IoStatus.Status = STATUS_NO_MEMORY;
                break;
            }

            LIST_FOR_EACH_ENTRY(radio, &device_list, struct bluetooth_radio, entry)
            {
                devices->Objects[i++] = radio->device_obj;
                call_fastcall_func1( ObfReferenceObject, radio->device_obj );
            }
            LeaveCriticalSection( &device_list_cs );

            devices->Count = i;
            irp->IoStatus.Information = (ULONG_PTR)devices;
            irp->IoStatus.Status = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_START_DEVICE:
            event_loop_thread =
                CreateThread( NULL, 0, bluetooth_event_loop_thread_proc, NULL, 0, NULL );
            irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            irp->IoStatus.Status = STATUS_SUCCESS;
            break;
        case IRP_MN_REMOVE_DEVICE:
        {
            struct bluetooth_radio *device, *cur;
            NTSTATUS ret;

            winebluetooth_shutdown();
            WaitForSingleObject( event_loop_thread, INFINITE );
            CloseHandle( event_loop_thread );
            EnterCriticalSection( &device_list_cs );
            LIST_FOR_EACH_ENTRY_SAFE( device, cur, &device_list, struct bluetooth_radio, entry )
            {
                if (device->state == BLUETOOTH_STATE_REMOVING)
                {
                    WARN( "Device already removed during FDO shutdown, skipping\n" );
                    continue;
                }
                winebluetooth_radio_free( device->radio );
                list_remove( &device->entry );
                IoDeleteDevice( device->device_obj );
            }
            LeaveCriticalSection( &device_list_cs );
            IoSkipCurrentIrpStackLocation( irp );
            ret = IoCallDriver( bus_pdo, irp );
            IoDetachDevice( bus_pdo );
            IoDeleteDevice( bus_fdo );
            return ret;
        }

        case IRP_MN_QUERY_ID:
            break;

        default:
            FIXME( "Unhandled minor function %s.\n", debugstr_minor_function_code( stack->MinorFunction ) );
    }

    IoSkipCurrentIrpStackLocation( irp );
    return IoCallDriver( bus_pdo, irp );
}

static NTSTATUS remote_device_query_id( struct bluetooth_remote_device *ext, IRP *irp, BUS_QUERY_ID_TYPE type )
{
    struct string_buffer buf = {0};

    TRACE( "(%p, %p, %s)\n", ext, irp, debugstr_BUS_QUERY_ID_TYPE( type ) );
    switch (type)
    {
    case BusQueryDeviceID:
        append_id( &buf, L"WINEBTH\\DEVICE" );
        TRACE( "BusQueryDeviceID -> %s\n", debugstr_w( buf.string ) );
        break;
    case BusQueryInstanceID:
    {
        BLUETOOTH_ADDRESS addr;

        EnterCriticalSection( &ext->props_cs );
        addr = ext->props.address;
        LeaveCriticalSection( &ext->props_cs );

        if (ext->radio->instance_prefix)
            append_id( &buf, L"%s&%02X%02X%02X%02X%02X%02X", ext->radio->instance_prefix, addr.rgBytes[0], addr.rgBytes[1],
                       addr.rgBytes[2], addr.rgBytes[3], addr.rgBytes[4], addr.rgBytes[5] );
        else
            append_id( &buf, L"%02X%02X%02X%02X%02X%02X", addr.rgBytes[0], addr.rgBytes[1],
                       addr.rgBytes[2], addr.rgBytes[3], addr.rgBytes[4], addr.rgBytes[5] );
        TRACE( "BusQueryInstanceID -> %s\n", debugstr_w( buf.string ) );
        break;
    }
    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs:
        append_id( &buf, L"" );
        break;
    default:
        return irp->IoStatus.Status;
    }

    if (!buf.string)
        return STATUS_NO_MEMORY;

    irp->IoStatus.Information = (ULONG_PTR)buf.string;
    return STATUS_SUCCESS;
}

static NTSTATUS radio_query_id( const struct bluetooth_radio *ext, IRP *irp, BUS_QUERY_ID_TYPE type )
{
    struct string_buffer buf = {0};

    TRACE( "(%p, %p, %s)\n", ext, irp, debugstr_BUS_QUERY_ID_TYPE( type ) );
    switch (type)
    {
    case BusQueryDeviceID:
        append_id( &buf, L"WINEBTH\\RADIO" );
        break;
    case BusQueryInstanceID:
        append_id( &buf, L"%s", ext->hw_name );
        break;
    case BusQueryHardwareIDs:
    case BusQueryCompatibleIDs:
        append_id( &buf, L"" );
        break;
    default:
        return irp->IoStatus.Status;
    }

    if (!buf.string)
        return STATUS_NO_MEMORY;

    irp->IoStatus.Information = (ULONG_PTR)buf.string;
    return STATUS_SUCCESS;
}

/* Caller must hold props_cs */
static void bluetooth_radio_set_properties( DEVICE_OBJECT *obj,
                                            winebluetooth_radio_props_mask_t mask,
                                            struct winebluetooth_radio_properties *props )
{
    if (mask & WINEBLUETOOTH_RADIO_PROPERTY_ADDRESS)
    {
        BTH_ADDR addr = RtlUlonglongByteSwap( props->address.ullLong ) >> 16;
        IoSetDevicePropertyData( obj, &DEVPKEY_BluetoothRadio_Address, LOCALE_NEUTRAL, 0,
                                 DEVPROP_TYPE_UINT64, sizeof( addr ), &addr );
    }
    if (mask & WINEBLUETOOTH_RADIO_PROPERTY_MANUFACTURER)
    {
        UINT16 manufacturer = props->manufacturer;
        IoSetDevicePropertyData( obj, &DEVPKEY_BluetoothRadio_Manufacturer, LOCALE_NEUTRAL,
                                 0, DEVPROP_TYPE_UINT16, sizeof( manufacturer ), &manufacturer );
    }
    if (mask & WINEBLUETOOTH_RADIO_PROPERTY_NAME)
    {
        WCHAR buf[BLUETOOTH_MAX_NAME_SIZE * sizeof(WCHAR)];
        INT ret;

        if ((ret = MultiByteToWideChar( CP_ACP, 0, props->name, -1, buf, BLUETOOTH_MAX_NAME_SIZE)))
            IoSetDevicePropertyData( obj, &DEVPKEY_NAME, LOCALE_NEUTRAL, 0, DEVPROP_TYPE_STRING, ret, buf );
    }
    if (mask & WINEBLUETOOTH_RADIO_PROPERTY_VERSION)
        IoSetDevicePropertyData( obj, &DEVPKEY_BluetoothRadio_LMPVersion, LOCALE_NEUTRAL, 0, DEVPROP_TYPE_BYTE,
                                 sizeof( props->version ), &props->version );
}

/* Caller should hold device_list_cs. */
static void remove_pending_irps( struct bluetooth_radio *radio )
{
    LIST_ENTRY *entry;
    IRP *irp;

    while ((entry = RemoveHeadList( &radio->irp_list )) != &radio->irp_list)
    {
        irp = CONTAINING_RECORD( entry, IRP, Tail.Overlay.ListEntry );
        irp->IoStatus.Status = STATUS_DELETE_PENDING;
        irp->IoStatus.Information = 0;
        IoCompleteRequest( irp, IO_NO_INCREMENT );
    }
}

static void remote_device_destroy( struct bluetooth_remote_device *ext )
{
    struct bluetooth_gatt_service *svc, *next;

    if (ext->bthle_symlink_name.Buffer)
    {
        IoSetDeviceInterfaceState( &ext->bthle_symlink_name, FALSE );
        RtlFreeUnicodeString( &ext->bthle_symlink_name );
    }
    ext->props_cs.DebugInfo->Spare[0] = 0;
    DeleteCriticalSection( &ext->props_cs );
    winebluetooth_device_free( ext->device );
    LIST_FOR_EACH_ENTRY_SAFE( svc, next, &ext->gatt_services, struct bluetooth_gatt_service, entry )
    {
        winebluetooth_gatt_service_free( svc->service );
        list_remove( &svc->entry );
        free( svc );
    }
    IoDeleteDevice( ext->device_obj );
}

static NTSTATUS WINAPI remote_device_pdo_pnp( DEVICE_OBJECT *device_obj, struct bluetooth_remote_device *ext, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    NTSTATUS ret = irp->IoStatus.Status;

    switch (stack->MinorFunction)
    {
    case IRP_MN_QUERY_ID:
        ret = remote_device_query_id( ext, irp, stack->Parameters.QueryId.IdType );
        break;
    case IRP_MN_QUERY_CAPABILITIES:
    {
        DEVICE_CAPABILITIES *caps = stack->Parameters.DeviceCapabilities.Capabilities;
        caps->Removable = TRUE;
        caps->SurpriseRemovalOK = TRUE;
        caps->RawDeviceOK = TRUE;
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_START_DEVICE:
    {
        BLUETOOTH_ADDRESS adapter_addr;
        BOOL already_started;
        struct winebluetooth_device_properties props_copy;
        winebluetooth_device_props_mask_t props_mask_copy;
        BOOL is_le;
        BOOL need_interface_reg;

        EnterCriticalSection( &device_list_cs );
        adapter_addr = ext->radio->props.address;
        LeaveCriticalSection( &device_list_cs );

        EnterCriticalSection( &ext->props_cs );
        already_started = (ext->state == BLUETOOTH_STATE_ACTIVE);

        if (already_started)
        {
            TRACE( "Device already started (unexpected - should only start at START_DEVICE)\n" );
            LeaveCriticalSection( &ext->props_cs );
            ret = STATUS_SUCCESS;
            break;
        }

        ext->state = BLUETOOTH_STATE_ACTIVE;

        /* Copy data needed for expensive operations, then release lock */
        props_copy = ext->props;
        props_mask_copy = ext->props_mask;
        is_le = ext->le;
        need_interface_reg = ext->le && !ext->bthle_symlink_name.Buffer;
        LeaveCriticalSection( &ext->props_cs );

        /* Set properties - this is where FriendlyName gets written to registry.
         * Done outside lock since it does expensive registry operations. */
        {
            NTSTATUS st;
            st = bluetooth_device_set_properties( ext, adapter_addr.rgBytes, &props_copy, props_mask_copy, is_le );
            (void)st;
        }

        if (need_interface_reg)
        {
            NTSTATUS iface_st;
            UNICODE_STRING symlink_name = {0};

            iface_st = IoRegisterDeviceInterface( device_obj, &GUID_BLUETOOTHLE_DEVICE_INTERFACE, NULL,
                                                  &symlink_name );
            if (iface_st == STATUS_SUCCESS)
            {
                TRACE( "Registered LE interface, symlink='%s'\n", debugstr_w(symlink_name.Buffer) );
                iface_st = IoSetDeviceInterfaceState( &symlink_name, TRUE );
                if (iface_st == STATUS_SUCCESS)
                {
                    EnterCriticalSection( &ext->props_cs );
                    if (!ext->bthle_symlink_name.Buffer)
                        ext->bthle_symlink_name = symlink_name;
                    else
                        RtlFreeUnicodeString( &symlink_name );
                    LeaveCriticalSection( &ext->props_cs );
                }
                else
                {
                    ERR( "IoSetDeviceInterfaceState failed: %#lx\n", iface_st );
                    RtlFreeUnicodeString( &symlink_name );
                }
            }
            else
            {
                ERR( "IoRegisterDeviceInterface failed: %#lx\n", iface_st );
            }
        }

        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_REMOVE_DEVICE:
    {
        EnterCriticalSection( &device_list_cs );
        if (ext->state != BLUETOOTH_STATE_REMOVING)
        {
            WARN( "IRP_MN_REMOVE_DEVICE called without prior SURPRISE_REMOVAL for device %s\n", ext->props.name );
            ext->state = BLUETOOTH_STATE_REMOVING;
            list_remove( &ext->entry );
        }
        LeaveCriticalSection( &device_list_cs );
        remote_device_destroy( ext );
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_SURPRISE_REMOVAL:
    {
        EnterCriticalSection( &device_list_cs);
        if (ext->state != BLUETOOTH_STATE_REMOVING)
        {
            ext->state = BLUETOOTH_STATE_REMOVING;
            list_remove( &ext->entry );
        }
        LeaveCriticalSection( &device_list_cs );
        ret = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_DEVICE_TEXT:
    {
        WCHAR *name;
        TRACE( "IRP_MN_QUERY_DEVICE_TEXT for remote device ext=%p, text_type=%d\n",
               ext, stack->Parameters.QueryDeviceText.DeviceTextType );
        if (stack->Parameters.QueryDeviceText.DeviceTextType != DeviceTextDescription) break;

        EnterCriticalSection( &ext->props_cs );
        TRACE( "Device name from props: '%s'\n", ext->props.name );
        if (ext->props.name[0])
        {
            DWORD len = MultiByteToWideChar( CP_UTF8, 0, ext->props.name, -1, NULL, 0 );
            name = ExAllocatePool( PagedPool, len * sizeof(WCHAR) );
            if (name)
            {
                MultiByteToWideChar( CP_UTF8, 0, ext->props.name, -1, name, len );
                irp->IoStatus.Information = (ULONG_PTR)name;
                ret = STATUS_SUCCESS;
            }
            else ret = STATUS_NO_MEMORY;
        }
        LeaveCriticalSection( &ext->props_cs );
        break;
    }

    default:
        FIXME( "Unhandled minor function %#x.\n", stack->MinorFunction );
    }

    irp->IoStatus.Status = ret;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return ret;
}

static NTSTATUS WINAPI radio_pdo_pnp( DEVICE_OBJECT *device_obj, struct bluetooth_radio *device, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS ret = irp->IoStatus.Status;

    TRACE( "device_obj %p, device %p, irp %p, minor function %s\n", device_obj, device, irp,
           debugstr_minor_function_code( stack->MinorFunction ) );
    switch (stack->MinorFunction)
    {
        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            struct bluetooth_remote_device *remote_device;
            DEVICE_RELATIONS *devices;
            SIZE_T i = 0;

            if (stack->Parameters.QueryDeviceRelations.Type != BusRelations)
            {
                FIXME( "Unhandled Device Relation %x\n", stack->Parameters.QueryDeviceRelations.Type );
                break;
            }

            EnterCriticalSection( &device_list_cs );
            devices = ExAllocatePool( PagedPool,
                                      offsetof( DEVICE_RELATIONS, Objects[list_count( &device->remote_devices )] ) );
            if (!devices)
            {
                LeaveCriticalSection( &device_list_cs );
                irp->IoStatus.Status = STATUS_NO_MEMORY;
                break;
            }
            LIST_FOR_EACH_ENTRY( remote_device, &device->remote_devices, struct bluetooth_remote_device, entry )
            {
                devices->Objects[i++] = remote_device->device_obj;
                call_fastcall_func1( ObfReferenceObject, remote_device->device_obj );
            }
            LeaveCriticalSection( &device_list_cs );

            devices->Count = i;
            irp->IoStatus.Information = (ULONG_PTR)devices;
            ret = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_QUERY_ID:
            ret = radio_query_id( device, irp, stack->Parameters.QueryId.IdType );
            break;
        case IRP_MN_QUERY_CAPABILITIES:
        {
            DEVICE_CAPABILITIES *caps = stack->Parameters.DeviceCapabilities.Capabilities;
            caps->Removable = TRUE;
            caps->SurpriseRemovalOK = TRUE;
            caps->RawDeviceOK = TRUE;
            ret = STATUS_SUCCESS;
            break;
        }
        case IRP_MN_START_DEVICE:
            EnterCriticalSection( &device_list_cs );
            bluetooth_radio_set_properties( device_obj, device->props_mask, &device->props );
            device->state = BLUETOOTH_STATE_ACTIVE;
            LeaveCriticalSection( &device_list_cs );

            if (IoRegisterDeviceInterface( device_obj, &GUID_BTHPORT_DEVICE_INTERFACE, NULL,
                                          &device->bthport_symlink_name ) == STATUS_SUCCESS)
                IoSetDeviceInterfaceState( &device->bthport_symlink_name, TRUE );

            if (IoRegisterDeviceInterface( device_obj, &GUID_BLUETOOTH_RADIO_INTERFACE, NULL,
                                          &device->bthradio_symlink_name ) == STATUS_SUCCESS)
                IoSetDeviceInterfaceState( &device->bthradio_symlink_name, TRUE );
            ret = STATUS_SUCCESS;
            break;
        case IRP_MN_REMOVE_DEVICE:
            EnterCriticalSection( &device_list_cs );
            if (device->state != BLUETOOTH_STATE_REMOVING)
            {
                WARN( "IRP_MN_REMOVE_DEVICE called without prior SURPRISE_REMOVAL for radio device\n" );
                device->state = BLUETOOTH_STATE_REMOVING;
                list_remove( &device->entry );
            }
            remove_pending_irps( device );
            LeaveCriticalSection( &device_list_cs );

            if (device->bthport_symlink_name.Buffer)
            {
                IoSetDeviceInterfaceState(&device->bthport_symlink_name, FALSE);
                RtlFreeUnicodeString( &device->bthport_symlink_name );
            }
            if (device->bthradio_symlink_name.Buffer)
            {
                IoSetDeviceInterfaceState(&device->bthradio_symlink_name, FALSE);
                RtlFreeUnicodeString( &device->bthradio_symlink_name );
            }
            free( device->hw_name );
            winebluetooth_radio_free( device->radio );
            IoDeleteDevice( device->device_obj );
            ret = STATUS_SUCCESS;
            break;
        case IRP_MN_SURPRISE_REMOVAL:
            EnterCriticalSection( &device_list_cs );
            remove_pending_irps( device );
            if (device->state != BLUETOOTH_STATE_REMOVING)
            {
                device->state = BLUETOOTH_STATE_REMOVING;
                list_remove( &device->entry );
            }
            LeaveCriticalSection( &device_list_cs );
            ret = STATUS_SUCCESS;
            break;
        case IRP_MN_QUERY_DEVICE_TEXT:
            WARN("Unhandled IRP_MN_QUERY_DEVICE_TEXT text type %u.\n", stack->Parameters.QueryDeviceText.DeviceTextType);
            break;
        default:
            FIXME( "Unhandled minor function %s.\n", debugstr_minor_function_code( stack->MinorFunction ) );
            break;
    }

    irp->IoStatus.Status = ret;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return ret;
}

static NTSTATUS auth_pnp( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS ret = irp->IoStatus.Status;

    TRACE( "device_obj %p, irp %p, minor function %s\n", device, irp, debugstr_minor_function_code( stack->MinorFunction ) );
    switch (stack->MinorFunction)
    {
    case IRP_MN_QUERY_ID:
    case IRP_MN_START_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        ret = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE:
        IoDeleteDevice( device );
        ret = STATUS_SUCCESS;
        break;
        ret = STATUS_SUCCESS;
    default:
        FIXME( "Unhandled minor function %s.\n", debugstr_minor_function_code( stack->MinorFunction ) );
        break;
    }

    irp->IoStatus.Status = ret;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return ret;
}

static NTSTATUS WINAPI bluetooth_pnp( DEVICE_OBJECT *device, IRP *irp )
{
    struct bluetooth_pdo_ext *ext;

    if (device == bus_fdo)
        return fdo_pnp( device, irp );
    else if (device == device_auth)
        return auth_pnp( device, irp );

    ext = device->DeviceExtension;
    switch (ext->type)
    {
    case BLUETOOTH_PDO_EXT_RADIO:
        return radio_pdo_pnp( device, &ext->radio, irp );
    case BLUETOOTH_PDO_EXT_REMOTE_DEVICE:
        return remote_device_pdo_pnp( device, &ext->remote_device, irp );
    DEFAULT_UNREACHABLE;
    }
}

static NTSTATUS WINAPI driver_add_device( DRIVER_OBJECT *driver, DEVICE_OBJECT *pdo )
{
    NTSTATUS ret;

    TRACE( "(%p, %p)\n", driver, pdo );
    ret = IoCreateDevice( driver, 0, NULL, FILE_DEVICE_BUS_EXTENDER, 0, FALSE, &bus_fdo );
    if (ret != STATUS_SUCCESS)
    {
        ERR( "failed to create FDO: %#lx\n", ret );
        return ret;
    }

    IoAttachDeviceToDeviceStack( bus_fdo, pdo );
    bus_pdo = pdo;
    bus_fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

static void WINAPI driver_unload( DRIVER_OBJECT *driver ) {}

static NTSTATUS WINAPI dispatch_create( DEVICE_OBJECT *device, IRP *irp )
{
    TRACE( "IRP_MJ_CREATE: device=%p, irp=%p\n", device, irp );
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI dispatch_close( DEVICE_OBJECT *device, IRP *irp )
{
    TRACE( "IRP_MJ_CLOSE: device=%p, irp=%p\n", device, irp );
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI DriverEntry( DRIVER_OBJECT *driver, UNICODE_STRING *path )
{
    UNICODE_STRING device_winebth_auth = RTL_CONSTANT_STRING( L"\\Device\\WINEBTHAUTH" );
    UNICODE_STRING object_winebth_auth = RTL_CONSTANT_STRING( WINEBTH_AUTH_DEVICE_PATH );
    NTSTATUS status;

    TRACE( "(%p, %s)\n", driver, debugstr_w( path->Buffer ) );

    status = winebluetooth_init();
    if (status)
        return status;

    driver_obj = driver;

    driver->DriverExtension->AddDevice = driver_add_device;
    driver->DriverUnload = driver_unload;
    driver->MajorFunction[IRP_MJ_CREATE] = dispatch_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = dispatch_close;
    driver->MajorFunction[IRP_MJ_PNP] = bluetooth_pnp;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_bluetooth;

    status = IoCreateDevice( driver, 0, &device_winebth_auth, 0, 0, FALSE, &device_auth );
    if (!status)
    {
        status = IoCreateSymbolicLink( &object_winebth_auth, &device_winebth_auth );
        if (status)
            ERR( "IoCreateSymbolicLink failed: %#lx\n", status );
    }
    else
        ERR( "IoCreateDevice failed: %#lx\n", status );
    return STATUS_SUCCESS;
}


