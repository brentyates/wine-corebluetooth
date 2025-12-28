/* windows.Devices.Bluetooth.Advertisement Implementation
 *
 * Copyright 2025 Vibhav Pant
 * Copyright 2025 Brent Yates (CoreBluetooth backend integration)
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

#include "private.h"

#define INITGUID
#include <setupapi.h>
#include <devguid.h>
#include <devpkey.h>
#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include <bthdef.h>
#include <bthioctl.h>
#include <winioctl.h>
#include <ddk/bthguid.h>
#include <winreg.h>
#include <winternl.h>
#undef INITGUID

#include "wine/debug.h"

#define IOCTL_WINEBTH_RADIO_START_DISCOVERY CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xa6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WINEBTH_RADIO_STOP_DISCOVERY  CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xa7, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define SHADOW_HASH_BUCKETS 256
#define SHADOW_TTL_MS       30000

WINE_DEFAULT_DEBUG_CHANNEL( bluetooth );

static HRESULT ble_adv_create( IBluetoothLEAdvertisement **adv );
static HRESULT ble_adv_create_with_name( HSTRING name, IBluetoothLEAdvertisement **adv );
static HRESULT adv_received_event_args_create( UINT64 address, INT16 rssi, const WCHAR *name,
                                                IBluetoothLEAdvertisementReceivedEventArgs **out );

struct shadow_entry
{
    UINT64 addr;
    WCHAR name[256];
    INT16 rssi;
    boolean connectable;
    ULONGLONG last_seen;
    struct shadow_entry *next;
};

struct shadow_table
{
    struct shadow_entry *buckets[SHADOW_HASH_BUCKETS];
};

struct pending_event
{
    UINT64 addr;
    INT16 rssi;
    WCHAR name[256];
    struct pending_event *next;
};

struct adv_watcher
{
    IBluetoothLEAdvertisementWatcher IBluetoothLEAdvertisementWatcher_iface;
    IBluetoothLEAdvertisementWatcher2 IBluetoothLEAdvertisementWatcher2_iface;
    LONG ref;

    CRITICAL_SECTION cs;
    BluetoothLEAdvertisementWatcherStatus status;
    BluetoothLEScanningMode scanning_mode;
    boolean allow_extended_advertisements;

    HANDLE radio;
    HANDLE event_thread;
    volatile BOOL running;

    ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs *received_handler;
    EventRegistrationToken received_token;
    LONG next_token;

    ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementWatcherStoppedEventArgs *stopped_handler;
    EventRegistrationToken stopped_token;

    IBluetoothLEAdvertisementFilter *filter;

    struct shadow_table shadow;

    struct pending_event *event_head;
    struct pending_event *event_tail;
};

static inline unsigned shadow_hash( UINT64 addr )
{
    addr ^= addr >> 33;
    addr *= 0xff51afd7ed558ccdULL;
    addr ^= addr >> 33;
    addr *= 0xc4ceb9fe1a85ec53ULL;
    addr ^= addr >> 33;
    return addr & (SHADOW_HASH_BUCKETS - 1);
}

static struct shadow_entry *shadow_get_or_create( struct shadow_table *t, UINT64 addr )
{
    unsigned idx = shadow_hash( addr );
    struct shadow_entry *e = t->buckets[idx];
    while (e)
    {
        if (e->addr == addr) return e;
        e = e->next;
    }
    e = calloc( 1, sizeof(*e) );
    if (!e) return NULL;
    e->addr = addr;
    e->rssi = -50;
    e->connectable = TRUE;
    e->last_seen = 0;
    e->next = t->buckets[idx];
    t->buckets[idx] = e;
    return e;
}

static void shadow_prune( struct shadow_table *t, ULONGLONG now )
{
    for (unsigned i = 0; i < SHADOW_HASH_BUCKETS; ++i)
    {
        struct shadow_entry **pp = &t->buckets[i];
        while (*pp)
        {
            if (now - (*pp)->last_seen > SHADOW_TTL_MS)
            {
                struct shadow_entry *old = *pp;
                *pp = old->next;
                free( old );
                continue;
            }
            pp = &(*pp)->next;
        }
    }
}

static void enqueue_pending_event( struct adv_watcher *watcher, UINT64 addr, INT16 rssi, const WCHAR *name )
{
    struct pending_event *ev = calloc( 1, sizeof(*ev) );
    if (!ev) return;

    ev->addr = addr;
    ev->rssi = rssi;
    if (name) lstrcpynW( ev->name, name, ARRAY_SIZE( ev->name ) );

    if (watcher->event_tail)
        watcher->event_tail->next = ev;
    else
        watcher->event_head = ev;
    watcher->event_tail = ev;
}

static BOOL matches_advertisement_filter( IBluetoothLEAdvertisementFilter *filter, const WCHAR *device_name )
{
    IBluetoothLEAdvertisement *adv = NULL;
    HSTRING filter_name = NULL;
    const WCHAR *filter_str;
    UINT32 filter_len;
    BOOL matches = TRUE;

    if (!filter) return TRUE;

    if (FAILED( IBluetoothLEAdvertisementFilter_get_Advertisement( filter, &adv ) ) || !adv)
        return TRUE;

    if (FAILED( IBluetoothLEAdvertisement_get_LocalName( adv, &filter_name ) ) || !filter_name)
    {
        IBluetoothLEAdvertisement_Release( adv );
        return TRUE;
    }

    filter_str = WindowsGetStringRawBuffer( filter_name, &filter_len );
    if (filter_len > 0)
    {
        if (!device_name || !device_name[0])
            matches = FALSE;
        else
            matches = (wcsstr( device_name, filter_str ) != NULL);
    }

    WindowsDeleteString( filter_name );
    IBluetoothLEAdvertisement_Release( adv );
    return matches;
}

static void process_pending_events( struct adv_watcher *watcher )
{
    struct pending_event *ev;

    while (watcher && watcher->running && (ev = watcher->event_head))
    {
        watcher->event_head = ev->next;
        if (!watcher->event_head)
            watcher->event_tail = NULL;

        EnterCriticalSection( &watcher->cs );
        if (watcher->received_handler && watcher->running)
        {
            const WCHAR *name_ptr = ev->name[0] ? ev->name : NULL;

            if (!matches_advertisement_filter( watcher->filter, name_ptr ))
            {
                LeaveCriticalSection( &watcher->cs );
                free( ev );
                continue;
            }

            IBluetoothLEAdvertisementReceivedEventArgs *args;
            HRESULT hr = adv_received_event_args_create( ev->addr, ev->rssi, name_ptr, &args );
            if (SUCCEEDED( hr ))
            {
                ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs_Invoke(
                    watcher->received_handler, &watcher->IBluetoothLEAdvertisementWatcher_iface, args );
                IBluetoothLEAdvertisementReceivedEventArgs_Release( args );
            }
        }
        LeaveCriticalSection( &watcher->cs );
        free( ev );
    }
}

struct adv_received_event_args
{
    IBluetoothLEAdvertisementReceivedEventArgs IBluetoothLEAdvertisementReceivedEventArgs_iface;
    IBluetoothLEAdvertisementReceivedEventArgs2 IBluetoothLEAdvertisementReceivedEventArgs2_iface;
    LONG ref;

    UINT64 bluetooth_address;
    INT16 rssi;
    BluetoothLEAdvertisementType adv_type;
    DateTime timestamp;
    HSTRING local_name;
};

static inline struct adv_received_event_args *impl_from_IBluetoothLEAdvertisementReceivedEventArgs(
    IBluetoothLEAdvertisementReceivedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct adv_received_event_args,
                              IBluetoothLEAdvertisementReceivedEventArgs_iface );
}

static inline struct adv_received_event_args *impl_from_IBluetoothLEAdvertisementReceivedEventArgs2(
    IBluetoothLEAdvertisementReceivedEventArgs2 *iface )
{
    return CONTAINING_RECORD( iface, struct adv_received_event_args,
                              IBluetoothLEAdvertisementReceivedEventArgs2_iface );
}

static HRESULT WINAPI adv_received_args_QueryInterface( IBluetoothLEAdvertisementReceivedEventArgs *iface,
                                                         REFIID iid, void **out )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementReceivedEventArgs ))
    {
        IBluetoothLEAdvertisementReceivedEventArgs_AddRef( (*out = &impl->IBluetoothLEAdvertisementReceivedEventArgs_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementReceivedEventArgs2 ))
    {
        IBluetoothLEAdvertisementReceivedEventArgs2_AddRef( (*out = &impl->IBluetoothLEAdvertisementReceivedEventArgs2_iface) );
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI adv_received_args_AddRef( IBluetoothLEAdvertisementReceivedEventArgs *iface )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI adv_received_args_Release( IBluetoothLEAdvertisementReceivedEventArgs *iface )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->local_name) WindowsDeleteString( impl->local_name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI adv_received_args_GetIids( IBluetoothLEAdvertisementReceivedEventArgs *iface,
                                                  ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IBluetoothLEAdvertisementReceivedEventArgs };
    IID *ret;
    ULONG i;

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = ARRAY_SIZE( iids_arr );
    if (!(ret = CoTaskMemAlloc( sizeof( IID ) * ARRAY_SIZE( iids_arr ) ))) return E_OUTOFMEMORY;

    for (i = 0; i < ARRAY_SIZE( iids_arr ); i++)
        ret[i] = *iids_arr[i];

    *iids = ret;
    return S_OK;
}

static HRESULT WINAPI adv_received_args_GetRuntimeClassName( IBluetoothLEAdvertisementReceivedEventArgs *iface,
                                                              HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementReceivedEventArgs";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI adv_received_args_GetTrustLevel( IBluetoothLEAdvertisementReceivedEventArgs *iface,
                                                        TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI adv_received_args_get_RawSignalStrengthInDBm(
    IBluetoothLEAdvertisementReceivedEventArgs *iface, INT16 *value )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->rssi;
    return S_OK;
}

static HRESULT WINAPI adv_received_args_get_BluetoothAddress(
    IBluetoothLEAdvertisementReceivedEventArgs *iface, UINT64 *value )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->bluetooth_address;
    return S_OK;
}

static HRESULT WINAPI adv_received_args_get_AdvertisementType(
    IBluetoothLEAdvertisementReceivedEventArgs *iface, BluetoothLEAdvertisementType *value )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->adv_type;
    return S_OK;
}

static HRESULT WINAPI adv_received_args_get_Timestamp(
    IBluetoothLEAdvertisementReceivedEventArgs *iface, DateTime *value )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->timestamp;
    return S_OK;
}

static HRESULT WINAPI adv_received_args_get_Advertisement(
    IBluetoothLEAdvertisementReceivedEventArgs *iface, IBluetoothLEAdvertisement **value )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return ble_adv_create_with_name( impl->local_name, value );
}

static const IBluetoothLEAdvertisementReceivedEventArgsVtbl adv_received_args_vtbl =
{
    adv_received_args_QueryInterface,
    adv_received_args_AddRef,
    adv_received_args_Release,
    adv_received_args_GetIids,
    adv_received_args_GetRuntimeClassName,
    adv_received_args_GetTrustLevel,
    adv_received_args_get_RawSignalStrengthInDBm,
    adv_received_args_get_BluetoothAddress,
    adv_received_args_get_AdvertisementType,
    adv_received_args_get_Timestamp,
    adv_received_args_get_Advertisement,
};

static HRESULT WINAPI adv_received_args2_QueryInterface( IBluetoothLEAdvertisementReceivedEventArgs2 *iface,
                                                          REFIID iid, void **out )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs2( iface );
    return adv_received_args_QueryInterface( &impl->IBluetoothLEAdvertisementReceivedEventArgs_iface, iid, out );
}

static ULONG WINAPI adv_received_args2_AddRef( IBluetoothLEAdvertisementReceivedEventArgs2 *iface )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs2( iface );
    return adv_received_args_AddRef( &impl->IBluetoothLEAdvertisementReceivedEventArgs_iface );
}

static ULONG WINAPI adv_received_args2_Release( IBluetoothLEAdvertisementReceivedEventArgs2 *iface )
{
    struct adv_received_event_args *impl = impl_from_IBluetoothLEAdvertisementReceivedEventArgs2( iface );
    return adv_received_args_Release( &impl->IBluetoothLEAdvertisementReceivedEventArgs_iface );
}

static HRESULT WINAPI adv_received_args2_GetIids( IBluetoothLEAdvertisementReceivedEventArgs2 *iface,
                                                   ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IBluetoothLEAdvertisementReceivedEventArgs,
                                     &IID_IBluetoothLEAdvertisementReceivedEventArgs2 };
    IID *ret;
    ULONG i;

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = ARRAY_SIZE( iids_arr );
    if (!(ret = CoTaskMemAlloc( sizeof( IID ) * ARRAY_SIZE( iids_arr ) ))) return E_OUTOFMEMORY;

    for (i = 0; i < ARRAY_SIZE( iids_arr ); i++)
        ret[i] = *iids_arr[i];

    *iids = ret;
    return S_OK;
}

static HRESULT WINAPI adv_received_args2_GetRuntimeClassName( IBluetoothLEAdvertisementReceivedEventArgs2 *iface,
                                                               HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementReceivedEventArgs";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI adv_received_args2_GetTrustLevel( IBluetoothLEAdvertisementReceivedEventArgs2 *iface,
                                                         TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI adv_received_args2_get_BluetoothAddressType(
    IBluetoothLEAdvertisementReceivedEventArgs2 *iface, BluetoothAddressType *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = BluetoothAddressType_Public;
    return S_OK;
}

static HRESULT WINAPI adv_received_args2_get_TransmitPowerLevelInDBm(
    IBluetoothLEAdvertisementReceivedEventArgs2 *iface, IReference_INT16 **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI adv_received_args2_get_IsAnonymous(
    IBluetoothLEAdvertisementReceivedEventArgs2 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI adv_received_args2_get_IsConnectable(
    IBluetoothLEAdvertisementReceivedEventArgs2 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI adv_received_args2_get_IsScannable(
    IBluetoothLEAdvertisementReceivedEventArgs2 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI adv_received_args2_get_IsDirected(
    IBluetoothLEAdvertisementReceivedEventArgs2 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static HRESULT WINAPI adv_received_args2_get_IsScanResponse(
    IBluetoothLEAdvertisementReceivedEventArgs2 *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = FALSE;
    return S_OK;
}

static const IBluetoothLEAdvertisementReceivedEventArgs2Vtbl adv_received_args2_vtbl =
{
    adv_received_args2_QueryInterface,
    adv_received_args2_AddRef,
    adv_received_args2_Release,
    adv_received_args2_GetIids,
    adv_received_args2_GetRuntimeClassName,
    adv_received_args2_GetTrustLevel,
    adv_received_args2_get_BluetoothAddressType,
    adv_received_args2_get_TransmitPowerLevelInDBm,
    adv_received_args2_get_IsAnonymous,
    adv_received_args2_get_IsConnectable,
    adv_received_args2_get_IsScannable,
    adv_received_args2_get_IsDirected,
    adv_received_args2_get_IsScanResponse,
};

static HRESULT adv_received_event_args_create( UINT64 address, INT16 rssi, const WCHAR *name,
                                                IBluetoothLEAdvertisementReceivedEventArgs **out )
{
    struct adv_received_event_args *impl;
    FILETIME ft;
    ULARGE_INTEGER uli;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisementReceivedEventArgs_iface.lpVtbl = &adv_received_args_vtbl;
    impl->IBluetoothLEAdvertisementReceivedEventArgs2_iface.lpVtbl = &adv_received_args2_vtbl;
    impl->ref = 1;
    impl->bluetooth_address = address;
    impl->rssi = rssi;
    impl->adv_type = BluetoothLEAdvertisementType_ConnectableUndirected;

    GetSystemTimeAsFileTime( &ft );
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    impl->timestamp.UniversalTime = uli.QuadPart;

    if (name)
        WindowsCreateString( name, wcslen( name ), &impl->local_name );
    else
        WindowsCreateString( L"", 0, &impl->local_name );

    *out = &impl->IBluetoothLEAdvertisementReceivedEventArgs_iface;
    return S_OK;
}

static inline struct adv_watcher *impl_from_IBluetoothLEAdvertisementWatcher( IBluetoothLEAdvertisementWatcher *iface )
{
    return CONTAINING_RECORD( iface, struct adv_watcher, IBluetoothLEAdvertisementWatcher_iface );
}

static inline struct adv_watcher *impl_from_IBluetoothLEAdvertisementWatcher2( IBluetoothLEAdvertisementWatcher2 *iface )
{
    return CONTAINING_RECORD( iface, struct adv_watcher, IBluetoothLEAdvertisementWatcher2_iface );
}

static const GUID my_GUID_BTHPORT_DEVICE_INTERFACE = { 0x850302a, 0xb344, 0x4fda, { 0x9b, 0xe9, 0x90, 0x57, 0x6b, 0x8d, 0x46, 0xf0 } };

static HANDLE open_first_radio( void )
{
    char buffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + MAX_PATH * sizeof( WCHAR )];
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *iface_detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
    SP_DEVICE_INTERFACE_DATA iface_data;
    HDEVINFO devinfo;
    HANDLE radio = INVALID_HANDLE_VALUE;
    DWORD idx = 0;

    devinfo = SetupDiGetClassDevsW( &my_GUID_BTHPORT_DEVICE_INTERFACE, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE );
    if (devinfo == INVALID_HANDLE_VALUE)
    {
        WARN( "SetupDiGetClassDevsW failed: %lu\n", GetLastError() );
    }
    else
    {
        iface_detail->cbSize = sizeof( *iface_detail );
        iface_data.cbSize = sizeof( iface_data );

        while (SetupDiEnumDeviceInterfaces( devinfo, NULL, &my_GUID_BTHPORT_DEVICE_INTERFACE, idx++, &iface_data ))
        {
            if (!SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, iface_detail, sizeof( buffer ), NULL, NULL ))
                continue;
            radio = CreateFileW( iface_detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
            if (radio != INVALID_HANDLE_VALUE)
                break;
        }
        SetupDiDestroyDeviceInfoList( devinfo );
    }

    if (radio == INVALID_HANDLE_VALUE)
    {
        WCHAR direct_path[64];
        int i;
        for (i = 0; i < 4 && radio == INVALID_HANDLE_VALUE; i++)
        {
            swprintf( direct_path, ARRAY_SIZE( direct_path ), L"\\\\?\\GLOBALROOT\\Device\\WINEBTH-RADIO-%d", i );
            radio = CreateFileW( direct_path, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
        }
    }

    return radio;
}

struct device_name_cache_entry
{
    UINT64 address;
    WCHAR name[256];
    struct device_name_cache_entry *next;
};

static struct device_name_cache_entry *device_name_cache = NULL;
static CRITICAL_SECTION device_name_cache_cs = {0};
static BOOL device_name_cache_cs_initialized = FALSE;

static void init_device_name_cache( void )
{
    if (!device_name_cache_cs_initialized)
    {
        InitializeCriticalSectionEx( &device_name_cache_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
        device_name_cache_cs_initialized = TRUE;
    }
}

static void cache_device_name( UINT64 address, const WCHAR *name )
{
    struct device_name_cache_entry *entry;

    if (!device_name_cache_cs_initialized)
        init_device_name_cache();

    /* Don't cache placeholder names - we want to re-query until we get the real name */
    if (!name || !name[0] || !wcscmp( name, L"BLE Device" ))
        return;

    EnterCriticalSection( &device_name_cache_cs );

    for (entry = device_name_cache; entry; entry = entry->next)
    {
        if (entry->address == address)
        {
            lstrcpynW( entry->name, name, ARRAY_SIZE( entry->name ) );
            LeaveCriticalSection( &device_name_cache_cs );
            return;
        }
    }

    entry = malloc( sizeof( *entry ) );
    if (entry)
    {
        entry->address = address;
        lstrcpynW( entry->name, name, ARRAY_SIZE( entry->name ) );
        entry->next = device_name_cache;
        device_name_cache = entry;
    }

    LeaveCriticalSection( &device_name_cache_cs );
}

static DWORD WINAPI adv_watcher_event_thread( void *param )
{
    struct adv_watcher *watcher = param;
    BTH_DEVICE_INFO_LIST *list;
    BYTE *buffer = NULL;
    SIZE_T buffer_size;
    DWORD bytes_returned;
    DWORD i;

    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    init_device_name_cache();

    while (watcher->running)
    {
        ULONGLONG now = GetTickCount64();
        DWORD device_count = 0;

        shadow_prune( &watcher->shadow, now );

        /* Query driver directly for all known BLE devices via IOCTL.
         * This is MUCH faster than waiting for PnP enumeration via SetupDi.
         * The driver knows about devices as soon as CoreBluetooth discovers them. */
        if (watcher->radio == INVALID_HANDLE_VALUE)
            goto sleep_and_continue;

        /* First call to get device count */
        buffer_size = sizeof(BTH_DEVICE_INFO_LIST);
        buffer = malloc( buffer_size );
        if (!buffer) goto sleep_and_continue;
        memset( buffer, 0, buffer_size );
        list = (BTH_DEVICE_INFO_LIST *)buffer;

        if (!DeviceIoControl( watcher->radio, IOCTL_BTH_GET_DEVICE_INFO, NULL, 0,
                              list, buffer_size, &bytes_returned, NULL ))
        {
            free( buffer );
            buffer = NULL;
            goto sleep_and_continue;
        }

        device_count = list->numOfDevices;
        if (device_count == 0)
        {
            free( buffer );
            buffer = NULL;
            goto sleep_and_continue;
        }

        /* Reallocate for full device list */
        free( buffer );
        if (device_count == 1)
            buffer_size = sizeof(BTH_DEVICE_INFO_LIST);
        else
            buffer_size = sizeof(BTH_DEVICE_INFO_LIST) + sizeof(BTH_DEVICE_INFO) * (device_count - 1);

        buffer = malloc( buffer_size );
        if (!buffer) goto sleep_and_continue;
        memset( buffer, 0, buffer_size );
        list = (BTH_DEVICE_INFO_LIST *)buffer;
        list->numOfDevices = device_count;

        if (!DeviceIoControl( watcher->radio, IOCTL_BTH_GET_DEVICE_INFO, NULL, 0,
                              list, buffer_size, &bytes_returned, NULL ))
        {
            free( buffer );
            buffer = NULL;
            goto sleep_and_continue;
        }

        /* Iterate over ALL devices from the driver - no PnP dependency */
        for (i = 0; i < device_count && watcher->running; i++)
        {
            BTH_DEVICE_INFO *info = &list->deviceList[i];
            UINT64 addr = info->address;
            WCHAR device_name[256];
            struct shadow_entry *entry;
            BOOL should_notify = FALSE;
            UINT64 addr_proper;

            if (!addr) continue;

            /* Convert address: driver uses LSB-first, WinRT expects MSB-first */
            addr_proper = RtlUlonglongByteSwap(addr) >> 16;

            entry = shadow_get_or_create( &watcher->shadow, addr );
            if (!entry) continue;

            /* Get device name from driver info */
            device_name[0] = 0;
            if (info->flags & BDIF_NAME && info->name[0])
            {
                MultiByteToWideChar( CP_UTF8, 0, info->name, -1, device_name, ARRAY_SIZE( device_name ) );
            }

            /* Skip generic "BLE Device" names */
            if (device_name[0] && !wcscmp( device_name, L"BLE Device" ))
                device_name[0] = 0;

            /* Check if this is a new device or name changed */
            if (entry->last_seen == 0)
            {
                /* First time seeing this device */
                if (device_name[0])
                {
                    lstrcpynW( entry->name, device_name, ARRAY_SIZE( entry->name ) );
                    cache_device_name( addr, entry->name );
                }
                should_notify = TRUE;
            }
            else if (device_name[0] && wcscmp( device_name, entry->name ))
            {
                /* Name changed (e.g., scan response arrived with LocalName) */
                lstrcpynW( entry->name, device_name, ARRAY_SIZE( entry->name ) );
                cache_device_name( addr, entry->name );
                should_notify = TRUE;
            }

            entry->last_seen = now;

            if (should_notify)
                enqueue_pending_event( watcher, addr_proper, entry->rssi,
                                       entry->name[0] ? entry->name : NULL );
        }

        free( buffer );
        buffer = NULL;

sleep_and_continue:
        if (watcher->running)
            process_pending_events( watcher );
        if (watcher->running)
            Sleep( 100 );
    }

    if (buffer) free( buffer );
    CoUninitialize();
    return 0;
}

static HRESULT WINAPI adv_watcher_QueryInterface( IBluetoothLEAdvertisementWatcher *iface, REFIID iid, void **out )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementWatcher ))
    {
        IBluetoothLEAdvertisementWatcher_AddRef(( *out = &impl->IBluetoothLEAdvertisementWatcher_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementWatcher2 ))
    {
        IBluetoothLEAdvertisementWatcher2_AddRef(( *out = &impl->IBluetoothLEAdvertisementWatcher2_iface ));
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI adv_watcher_AddRef( IBluetoothLEAdvertisementWatcher *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI adv_watcher_Release( IBluetoothLEAdvertisementWatcher *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        BOOL was_running = FALSE;
        EnterCriticalSection( &impl->cs );
        if (impl->running)
        {
            impl->running = FALSE;
            impl->event_head = NULL;
            impl->event_tail = NULL;
            was_running = TRUE;
        }
        LeaveCriticalSection( &impl->cs );
        
        if (was_running)
        {
            if (impl->event_thread)
            {
                WaitForSingleObject( impl->event_thread, 5000 );
                CloseHandle( impl->event_thread );
            }
        }
        if (impl->radio != INVALID_HANDLE_VALUE)
            CloseHandle( impl->radio );
        if (impl->received_handler)
            ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs_Release(
                impl->received_handler );
        if (impl->stopped_handler)
            ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementWatcherStoppedEventArgs_Release(
                impl->stopped_handler );
        if (impl->filter)
            IBluetoothLEAdvertisementFilter_Release( impl->filter );
        while (impl->event_head)
        {
            struct pending_event *next = impl->event_head->next;
            free( impl->event_head );
            impl->event_head = next;
        }
        impl->event_tail = NULL;
        if (impl->cs.DebugInfo && impl->cs.DebugInfo != (RTL_CRITICAL_SECTION_DEBUG *)-1)
            impl->cs.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &impl->cs );
        impl->IBluetoothLEAdvertisementWatcher_iface.lpVtbl = NULL;
        impl->IBluetoothLEAdvertisementWatcher2_iface.lpVtbl = NULL;
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI adv_watcher_GetIids( IBluetoothLEAdvertisementWatcher *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IBluetoothLEAdvertisementWatcher };
    IID *ret;
    ULONG i;

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = ARRAY_SIZE( iids_arr );
    if (!(ret = CoTaskMemAlloc( sizeof( IID ) * ARRAY_SIZE( iids_arr ) ))) return E_OUTOFMEMORY;

    for (i = 0; i < ARRAY_SIZE( iids_arr ); i++)
        ret[i] = *iids_arr[i];

    *iids = ret;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_GetRuntimeClassName( IBluetoothLEAdvertisementWatcher *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI adv_watcher_GetTrustLevel( IBluetoothLEAdvertisementWatcher *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MinSamplingInternal( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 1000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MaxSamplingInternal( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 255000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MinOutOfRangeTimeout( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 10000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_MaxOutOfRangeTimeout( IBluetoothLEAdvertisementWatcher *iface, TimeSpan *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    value->Duration = 600000000;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_Status( IBluetoothLEAdvertisementWatcher *iface,
                                               BluetoothLEAdvertisementWatcherStatus *status )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p, %p)\n", iface, status );
    if (!status) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    *status = impl->status;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_ScanningMode( IBluetoothLEAdvertisementWatcher *iface,
                                                     BluetoothLEScanningMode *value )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    *value = impl->scanning_mode;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI adv_watcher_put_ScanningMode( IBluetoothLEAdvertisementWatcher *iface,
                                                     BluetoothLEScanningMode value )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    TRACE( "(%p, %d)\n", iface, value );
    EnterCriticalSection( &impl->cs );
    impl->scanning_mode = value;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_SignalStrengthFilter( IBluetoothLEAdvertisementWatcher *iface,
                                                            IBluetoothSignalStrengthFilter **filter )
{
    TRACE( "(%p, %p)\n", iface, filter );
    if (!filter) return E_POINTER;
    *filter = NULL;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_put_SignalStrengthFilter( IBluetoothLEAdvertisementWatcher *iface,
                                                            IBluetoothSignalStrengthFilter *filter )
{
    TRACE( "(%p, %p)\n", iface, filter );
    return S_OK;
}

static HRESULT WINAPI adv_watcher_get_AdvertisementFilter( IBluetoothLEAdvertisementWatcher *iface,
                                                           IBluetoothLEAdvertisementFilter **filter )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );

    TRACE( "(%p, %p)\n", iface, filter );

    if (!filter) return E_POINTER;

    EnterCriticalSection( &impl->cs );
    *filter = impl->filter;
    if (*filter) IBluetoothLEAdvertisementFilter_AddRef( *filter );
    LeaveCriticalSection( &impl->cs );

    return S_OK;
}

static HRESULT WINAPI adv_watcher_put_AdvertisementFilter( IBluetoothLEAdvertisementWatcher *iface,
                                                           IBluetoothLEAdvertisementFilter *filter )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );

    TRACE( "(%p, %p)\n", iface, filter );

    EnterCriticalSection( &impl->cs );
    if (impl->filter) IBluetoothLEAdvertisementFilter_Release( impl->filter );
    impl->filter = filter;
    if (filter) IBluetoothLEAdvertisementFilter_AddRef( filter );
    LeaveCriticalSection( &impl->cs );

    return S_OK;
}

static HRESULT WINAPI adv_watcher_Start( IBluetoothLEAdvertisementWatcher *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    DWORD bytes;

    TRACE( "(%p)\n", iface );

    EnterCriticalSection( &impl->cs );

    if (impl->status == BluetoothLEAdvertisementWatcherStatus_Started)
    {
        LeaveCriticalSection( &impl->cs );
        return S_OK;
    }

    if (impl->radio == INVALID_HANDLE_VALUE)
    {
        impl->radio = open_first_radio();
        if (impl->radio == INVALID_HANDLE_VALUE)
        {
            WARN( "No Bluetooth radio found\n" );
            impl->status = BluetoothLEAdvertisementWatcherStatus_Aborted;
            LeaveCriticalSection( &impl->cs );
            return HRESULT_FROM_WIN32( ERROR_NOT_FOUND );
        }
    }

    if (!DeviceIoControl( impl->radio, IOCTL_WINEBTH_RADIO_START_DISCOVERY, NULL, 0, NULL, 0, &bytes, NULL ))
    {
        DWORD err = GetLastError();
        WARN( "IOCTL_WINEBTH_RADIO_START_DISCOVERY failed: %lu\n", err );
        impl->status = BluetoothLEAdvertisementWatcherStatus_Aborted;
        LeaveCriticalSection( &impl->cs );
        return HRESULT_FROM_WIN32( err );
    }

    impl->running = TRUE;
    impl->event_thread = CreateThread( NULL, 0, adv_watcher_event_thread, impl, 0, NULL );
    if (!impl->event_thread)
    {
        DWORD err = GetLastError();
        WARN( "CreateThread failed: %lu\n", err );
        impl->running = FALSE;
        DeviceIoControl( impl->radio, IOCTL_WINEBTH_RADIO_STOP_DISCOVERY, NULL, 0, NULL, 0, &bytes, NULL );
        impl->status = BluetoothLEAdvertisementWatcherStatus_Aborted;
        LeaveCriticalSection( &impl->cs );
        return HRESULT_FROM_WIN32( err );
    }

    impl->status = BluetoothLEAdvertisementWatcherStatus_Started;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI adv_watcher_Stop( IBluetoothLEAdvertisementWatcher *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );
    DWORD bytes;

    TRACE( "(%p)\n", iface );

    EnterCriticalSection( &impl->cs );

    if (impl->status != BluetoothLEAdvertisementWatcherStatus_Started)
    {
        LeaveCriticalSection( &impl->cs );
        return S_OK;
    }

    impl->running = FALSE;
    LeaveCriticalSection( &impl->cs );

    if (impl->event_thread)
    {
        WaitForSingleObject( impl->event_thread, 5000 );
        CloseHandle( impl->event_thread );
        impl->event_thread = NULL;
    }

    EnterCriticalSection( &impl->cs );

    if (impl->radio != INVALID_HANDLE_VALUE)
    {
        DeviceIoControl( impl->radio, IOCTL_WINEBTH_RADIO_STOP_DISCOVERY, NULL, 0, NULL, 0, &bytes, NULL );
    }

    impl->status = BluetoothLEAdvertisementWatcherStatus_Stopped;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI adv_watcher_add_Received( IBluetoothLEAdvertisementWatcher *iface,
                                                ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs *handler,
                                                EventRegistrationToken *token )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );

    TRACE( "(%p, %p, %p)\n", iface, handler, token );

    if (!handler || !token) return E_POINTER;

    EnterCriticalSection( &impl->cs );

    if (impl->received_handler)
        ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs_Release(
            impl->received_handler );

    impl->received_handler = handler;
    ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs_AddRef( handler );

    impl->received_token.value = InterlockedIncrement( &impl->next_token );
    *token = impl->received_token;

    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI adv_watcher_remove_Received( IBluetoothLEAdvertisementWatcher *iface,
                                                    EventRegistrationToken token )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );

    TRACE( "(%p, %I64x)\n", iface, token.value );

    EnterCriticalSection( &impl->cs );

    if (impl->received_token.value == token.value && impl->received_handler)
    {
        ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs_Release(
            impl->received_handler );
        impl->received_handler = NULL;
        impl->received_token.value = 0;
    }

    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI adv_watcher_add_Stopped( IBluetoothLEAdvertisementWatcher *iface,
                                               ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementWatcherStoppedEventArgs *handler,
                                               EventRegistrationToken *token )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );

    TRACE( "(%p, %p, %p)\n", iface, handler, token );

    if (!handler || !token) return E_POINTER;

    EnterCriticalSection( &impl->cs );

    if (impl->stopped_handler)
        ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementWatcherStoppedEventArgs_Release(
            impl->stopped_handler );

    impl->stopped_handler = handler;
    ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementWatcherStoppedEventArgs_AddRef( handler );

    impl->stopped_token.value = InterlockedIncrement( &impl->next_token );
    *token = impl->stopped_token;

    LeaveCriticalSection( &impl->cs );
    return S_OK;
}


static HRESULT WINAPI adv_watcher_remove_Stopped( IBluetoothLEAdvertisementWatcher *iface, EventRegistrationToken token )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher( iface );

    TRACE( "(%p, %I64x)\n", iface, token.value );

    EnterCriticalSection( &impl->cs );

    if (impl->stopped_token.value == token.value && impl->stopped_handler)
    {
        ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementWatcherStoppedEventArgs_Release(
            impl->stopped_handler );
        impl->stopped_handler = NULL;
        impl->stopped_token.value = 0;
    }

    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static const IBluetoothLEAdvertisementWatcherVtbl adv_watcher_vtbl =
{
    /* IUnknown */
    adv_watcher_QueryInterface,
    adv_watcher_AddRef,
    adv_watcher_Release,
    /* IInspectable */
    adv_watcher_GetIids,
    adv_watcher_GetRuntimeClassName,
    adv_watcher_GetTrustLevel,
    /* IBluetoothLEAdvertisementWatcher */
    adv_watcher_get_MinSamplingInternal,
    adv_watcher_get_MaxSamplingInternal,
    adv_watcher_get_MinOutOfRangeTimeout,
    adv_watcher_get_MaxOutOfRangeTimeout,
    adv_watcher_get_Status,
    adv_watcher_get_ScanningMode,
    adv_watcher_put_ScanningMode,
    adv_watcher_get_SignalStrengthFilter,
    adv_watcher_put_SignalStrengthFilter,
    adv_watcher_get_AdvertisementFilter,
    adv_watcher_put_AdvertisementFilter,
    adv_watcher_Start,
    adv_watcher_Stop,
    adv_watcher_add_Received,
    adv_watcher_remove_Received,
    adv_watcher_add_Stopped,
    adv_watcher_remove_Stopped
};

static HRESULT WINAPI adv_watcher2_QueryInterface( IBluetoothLEAdvertisementWatcher2 *iface, REFIID iid, void **out )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher2( iface );
    return adv_watcher_QueryInterface( &impl->IBluetoothLEAdvertisementWatcher_iface, iid, out );
}

static ULONG WINAPI adv_watcher2_AddRef( IBluetoothLEAdvertisementWatcher2 *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher2( iface );
    return adv_watcher_AddRef( &impl->IBluetoothLEAdvertisementWatcher_iface );
}

static ULONG WINAPI adv_watcher2_Release( IBluetoothLEAdvertisementWatcher2 *iface )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher2( iface );
    return adv_watcher_Release( &impl->IBluetoothLEAdvertisementWatcher_iface );
}

static HRESULT WINAPI adv_watcher2_GetIids( IBluetoothLEAdvertisementWatcher2 *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IBluetoothLEAdvertisementWatcher,
                                     &IID_IBluetoothLEAdvertisementWatcher2 };
    IID *ret;
    ULONG i;

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = ARRAY_SIZE( iids_arr );
    if (!(ret = CoTaskMemAlloc( sizeof( IID ) * ARRAY_SIZE( iids_arr ) ))) return E_OUTOFMEMORY;

    for (i = 0; i < ARRAY_SIZE( iids_arr ); i++)
        ret[i] = *iids_arr[i];

    *iids = ret;
    return S_OK;
}

static HRESULT WINAPI adv_watcher2_GetRuntimeClassName( IBluetoothLEAdvertisementWatcher2 *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI adv_watcher2_GetTrustLevel( IBluetoothLEAdvertisementWatcher2 *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI adv_watcher2_get_AllowExtendedAdvertisements( IBluetoothLEAdvertisementWatcher2 *iface,
                                                                     boolean *value )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher2( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    EnterCriticalSection( &impl->cs );
    *value = impl->allow_extended_advertisements;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static HRESULT WINAPI adv_watcher2_put_AllowExtendedAdvertisements( IBluetoothLEAdvertisementWatcher2 *iface,
                                                                     boolean value )
{
    struct adv_watcher *impl = impl_from_IBluetoothLEAdvertisementWatcher2( iface );
    TRACE( "(%p, %d)\n", iface, value );
    EnterCriticalSection( &impl->cs );
    impl->allow_extended_advertisements = value;
    LeaveCriticalSection( &impl->cs );
    return S_OK;
}

static const IBluetoothLEAdvertisementWatcher2Vtbl adv_watcher2_vtbl =
{
    adv_watcher2_QueryInterface,
    adv_watcher2_AddRef,
    adv_watcher2_Release,
    adv_watcher2_GetIids,
    adv_watcher2_GetRuntimeClassName,
    adv_watcher2_GetTrustLevel,
    adv_watcher2_get_AllowExtendedAdvertisements,
    adv_watcher2_put_AllowExtendedAdvertisements
};

static HRESULT adv_watcher_create( IBluetoothLEAdvertisementWatcher **watcher )
{
    struct adv_watcher *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisementWatcher_iface.lpVtbl = &adv_watcher_vtbl;
    impl->IBluetoothLEAdvertisementWatcher2_iface.lpVtbl = &adv_watcher2_vtbl;
    impl->ref = 1;
    impl->radio = INVALID_HANDLE_VALUE;
    impl->status = BluetoothLEAdvertisementWatcherStatus_Created;
    impl->scanning_mode = BluetoothLEScanningMode_Passive;
    impl->allow_extended_advertisements = FALSE;
    InitializeCriticalSectionEx( &impl->cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
    impl->cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": adv_watcher.cs");

    *watcher = &impl->IBluetoothLEAdvertisementWatcher_iface;
    return S_OK;
}

struct adv_watcher_factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct adv_watcher_factory *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct adv_watcher_factory, IActivationFactory_iface );
}

static HRESULT WINAPI adv_watcher_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct adv_watcher_factory *impl = impl_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI adv_watcher_factory_AddRef( IActivationFactory *iface )
{
    struct adv_watcher_factory *impl = impl_from_IActivationFactory( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI adv_watcher_factory_Release( IActivationFactory *iface )
{
    struct adv_watcher_factory *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    return ref;
}

static HRESULT WINAPI adv_watcher_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IActivationFactory };
    IID *ret;
    ULONG i;

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = ARRAY_SIZE( iids_arr );
    if (!(ret = CoTaskMemAlloc( sizeof( IID ) * ARRAY_SIZE( iids_arr ) ))) return E_OUTOFMEMORY;

    for (i = 0; i < ARRAY_SIZE( iids_arr ); i++)
        ret[i] = *iids_arr[i];

    *iids = ret;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementWatcher";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI adv_watcher_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI adv_watcher_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "(%p, %p)\n", iface, instance );
    return adv_watcher_create( (IBluetoothLEAdvertisementWatcher **)instance );
}

static const struct IActivationFactoryVtbl adv_watcher_factory_vtbl =
{
    adv_watcher_factory_QueryInterface,
    adv_watcher_factory_AddRef,
    adv_watcher_factory_Release,
    /* IInspectable */
    adv_watcher_factory_GetIids,
    adv_watcher_factory_GetRuntimeClassName,
    adv_watcher_factory_GetTrustLevel,
    /* IActivationFactory */
    adv_watcher_factory_ActivateInstance
};

static struct adv_watcher_factory adv_watcher_factory =
{
    {&adv_watcher_factory_vtbl},
    1
};

IActivationFactory *advertisement_watcher_factory = &adv_watcher_factory.IActivationFactory_iface;

struct adv_filter
{
    IBluetoothLEAdvertisementFilter IBluetoothLEAdvertisementFilter_iface;
    LONG ref;
    IBluetoothLEAdvertisement *advertisement;
};

static inline struct adv_filter *impl_from_IBluetoothLEAdvertisementFilter( IBluetoothLEAdvertisementFilter *iface )
{
    return CONTAINING_RECORD( iface, struct adv_filter, IBluetoothLEAdvertisementFilter_iface );
}

static HRESULT WINAPI adv_filter_QueryInterface( IBluetoothLEAdvertisementFilter *iface, REFIID iid, void **out )
{
    struct adv_filter *impl = impl_from_IBluetoothLEAdvertisementFilter( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisementFilter ))
    {
        IBluetoothLEAdvertisementFilter_AddRef(( *out = &impl->IBluetoothLEAdvertisementFilter_iface ));
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI adv_filter_AddRef( IBluetoothLEAdvertisementFilter *iface )
{
    struct adv_filter *impl = impl_from_IBluetoothLEAdvertisementFilter( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI adv_filter_Release( IBluetoothLEAdvertisementFilter *iface )
{
    struct adv_filter *impl = impl_from_IBluetoothLEAdvertisementFilter( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->advertisement) IBluetoothLEAdvertisement_Release( impl->advertisement );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI adv_filter_GetIids( IBluetoothLEAdvertisementFilter *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IBluetoothLEAdvertisementFilter };
    IID *ret;
    ULONG i;

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = ARRAY_SIZE( iids_arr );
    if (!(ret = CoTaskMemAlloc( sizeof( IID ) * ARRAY_SIZE( iids_arr ) ))) return E_OUTOFMEMORY;

    for (i = 0; i < ARRAY_SIZE( iids_arr ); i++)
        ret[i] = *iids_arr[i];

    *iids = ret;
    return S_OK;
}

static HRESULT WINAPI adv_filter_GetRuntimeClassName( IBluetoothLEAdvertisementFilter *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementFilter";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI adv_filter_GetTrustLevel( IBluetoothLEAdvertisementFilter *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI adv_filter_get_Advertisement( IBluetoothLEAdvertisementFilter *iface,
                                                     IBluetoothLEAdvertisement **value )
{
    struct adv_filter *impl = impl_from_IBluetoothLEAdvertisementFilter( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->advertisement;
    if (*value) IBluetoothLEAdvertisement_AddRef( *value );
    return S_OK;
}

static HRESULT WINAPI adv_filter_put_Advertisement( IBluetoothLEAdvertisementFilter *iface,
                                                     IBluetoothLEAdvertisement *value )
{
    struct adv_filter *impl = impl_from_IBluetoothLEAdvertisementFilter( iface );
    TRACE( "(%p, %p)\n", iface, value );

    if (impl->advertisement) IBluetoothLEAdvertisement_Release( impl->advertisement );
    impl->advertisement = value;
    if (value) IBluetoothLEAdvertisement_AddRef( value );
    return S_OK;
}

static HRESULT WINAPI adv_filter_get_BytePatterns( IBluetoothLEAdvertisementFilter *iface,
                                                    IVector_BluetoothLEAdvertisementBytePattern **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static const IBluetoothLEAdvertisementFilterVtbl adv_filter_vtbl =
{
    adv_filter_QueryInterface,
    adv_filter_AddRef,
    adv_filter_Release,
    adv_filter_GetIids,
    adv_filter_GetRuntimeClassName,
    adv_filter_GetTrustLevel,
    adv_filter_get_Advertisement,
    adv_filter_put_Advertisement,
    adv_filter_get_BytePatterns
};

static HRESULT adv_filter_create( IBluetoothLEAdvertisementFilter **filter )
{
    struct adv_filter *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisementFilter_iface.lpVtbl = &adv_filter_vtbl;
    impl->ref = 1;

    *filter = &impl->IBluetoothLEAdvertisementFilter_iface;
    return S_OK;
}

struct adv_filter_factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct adv_filter_factory *impl_from_IActivationFactory_filter( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct adv_filter_factory, IActivationFactory_iface );
}

static HRESULT WINAPI adv_filter_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct adv_filter_factory *impl = impl_from_IActivationFactory_filter( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI adv_filter_factory_AddRef( IActivationFactory *iface )
{
    struct adv_filter_factory *impl = impl_from_IActivationFactory_filter( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI adv_filter_factory_Release( IActivationFactory *iface )
{
    struct adv_filter_factory *impl = impl_from_IActivationFactory_filter( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    return ref;
}

static HRESULT WINAPI adv_filter_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IActivationFactory };
    IID *ret;
    ULONG i;

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = ARRAY_SIZE( iids_arr );
    if (!(ret = CoTaskMemAlloc( sizeof( IID ) * ARRAY_SIZE( iids_arr ) ))) return E_OUTOFMEMORY;

    for (i = 0; i < ARRAY_SIZE( iids_arr ); i++)
        ret[i] = *iids_arr[i];

    *iids = ret;
    return S_OK;
}

static HRESULT WINAPI adv_filter_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementFilter";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI adv_filter_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI adv_filter_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "(%p, %p)\n", iface, instance );
    return adv_filter_create( (IBluetoothLEAdvertisementFilter **)instance );
}

static const struct IActivationFactoryVtbl adv_filter_factory_vtbl =
{
    adv_filter_factory_QueryInterface,
    adv_filter_factory_AddRef,
    adv_filter_factory_Release,
    adv_filter_factory_GetIids,
    adv_filter_factory_GetRuntimeClassName,
    adv_filter_factory_GetTrustLevel,
    adv_filter_factory_ActivateInstance
};

static struct adv_filter_factory adv_filter_factory =
{
    {&adv_filter_factory_vtbl},
    1
};

IActivationFactory *advertisement_filter_factory = &adv_filter_factory.IActivationFactory_iface;

struct ble_advertisement
{
    IBluetoothLEAdvertisement IBluetoothLEAdvertisement_iface;
    LONG ref;
    HSTRING local_name;
};

static inline struct ble_advertisement *impl_from_IBluetoothLEAdvertisement( IBluetoothLEAdvertisement *iface )
{
    return CONTAINING_RECORD( iface, struct ble_advertisement, IBluetoothLEAdvertisement_iface );
}

static HRESULT WINAPI ble_adv_QueryInterface( IBluetoothLEAdvertisement *iface, REFIID iid, void **out )
{
    struct ble_advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEAdvertisement ))
    {
        IBluetoothLEAdvertisement_AddRef(( *out = &impl->IBluetoothLEAdvertisement_iface ));
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI ble_adv_AddRef( IBluetoothLEAdvertisement *iface )
{
    struct ble_advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI ble_adv_Release( IBluetoothLEAdvertisement *iface )
{
    struct ble_advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->local_name) WindowsDeleteString( impl->local_name );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI ble_adv_GetIids( IBluetoothLEAdvertisement *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IBluetoothLEAdvertisement };
    IID *ret;
    ULONG i;

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = ARRAY_SIZE( iids_arr );
    if (!(ret = CoTaskMemAlloc( sizeof( IID ) * ARRAY_SIZE( iids_arr ) ))) return E_OUTOFMEMORY;

    for (i = 0; i < ARRAY_SIZE( iids_arr ); i++)
        ret[i] = *iids_arr[i];

    *iids = ret;
    return S_OK;
}

static HRESULT WINAPI ble_adv_GetRuntimeClassName( IBluetoothLEAdvertisement *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisement";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI ble_adv_GetTrustLevel( IBluetoothLEAdvertisement *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI ble_adv_get_Flags( IBluetoothLEAdvertisement *iface,
                                          IReference_BluetoothLEAdvertisementFlags **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI ble_adv_put_Flags( IBluetoothLEAdvertisement *iface,
                                          IReference_BluetoothLEAdvertisementFlags *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI ble_adv_get_LocalName( IBluetoothLEAdvertisement *iface, HSTRING *value )
{
    struct ble_advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->local_name, value );
}

static HRESULT WINAPI ble_adv_put_LocalName( IBluetoothLEAdvertisement *iface, HSTRING value )
{
    struct ble_advertisement *impl = impl_from_IBluetoothLEAdvertisement( iface );
    TRACE( "(%p, %s)\n", iface, debugstr_hstring( value ) );
    if (impl->local_name) WindowsDeleteString( impl->local_name );
    return WindowsDuplicateString( value, &impl->local_name );
}

static HRESULT WINAPI ble_adv_get_ServiceUuids( IBluetoothLEAdvertisement *iface, IVector_GUID **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI ble_adv_get_ManufacturerData( IBluetoothLEAdvertisement *iface,
                                                     IVector_BluetoothLEManufacturerData **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI ble_adv_get_DataSections( IBluetoothLEAdvertisement *iface,
                                                 IVector_BluetoothLEAdvertisementDataSection **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI ble_adv_GetManufacturerDataByCompanyId( IBluetoothLEAdvertisement *iface, UINT16 id,
                                                               IVectorView_BluetoothLEManufacturerData **value )
{
    TRACE( "(%p, %u, %p)\n", iface, id, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI ble_adv_GetSectionsByType( IBluetoothLEAdvertisement *iface, BYTE type,
                                                  IVectorView_BluetoothLEAdvertisementDataSection **value )
{
    TRACE( "(%p, %u, %p)\n", iface, type, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static const IBluetoothLEAdvertisementVtbl ble_adv_vtbl =
{
    ble_adv_QueryInterface,
    ble_adv_AddRef,
    ble_adv_Release,
    ble_adv_GetIids,
    ble_adv_GetRuntimeClassName,
    ble_adv_GetTrustLevel,
    ble_adv_get_Flags,
    ble_adv_put_Flags,
    ble_adv_get_LocalName,
    ble_adv_put_LocalName,
    ble_adv_get_ServiceUuids,
    ble_adv_get_ManufacturerData,
    ble_adv_get_DataSections,
    ble_adv_GetManufacturerDataByCompanyId,
    ble_adv_GetSectionsByType
};

static HRESULT ble_adv_create( IBluetoothLEAdvertisement **adv )
{
    struct ble_advertisement *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisement_iface.lpVtbl = &ble_adv_vtbl;
    impl->ref = 1;
    WindowsCreateString( L"", 0, &impl->local_name );

    *adv = &impl->IBluetoothLEAdvertisement_iface;
    return S_OK;
}

static HRESULT ble_adv_create_with_name( HSTRING name, IBluetoothLEAdvertisement **adv )
{
    struct ble_advertisement *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEAdvertisement_iface.lpVtbl = &ble_adv_vtbl;
    impl->ref = 1;

    if (name)
        WindowsDuplicateString( name, &impl->local_name );
    else
        WindowsCreateString( L"", 0, &impl->local_name );

    *adv = &impl->IBluetoothLEAdvertisement_iface;
    return S_OK;
}

struct ble_adv_factory
{
    IActivationFactory IActivationFactory_iface;
    LONG ref;
};

static inline struct ble_adv_factory *impl_from_IActivationFactory_adv( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct ble_adv_factory, IActivationFactory_iface );
}

static HRESULT WINAPI ble_adv_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct ble_adv_factory *impl = impl_from_IActivationFactory_adv( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI ble_adv_factory_AddRef( IActivationFactory *iface )
{
    struct ble_adv_factory *impl = impl_from_IActivationFactory_adv( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI ble_adv_factory_Release( IActivationFactory *iface )
{
    struct ble_adv_factory *impl = impl_from_IActivationFactory_adv( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    return ref;
}

static HRESULT WINAPI ble_adv_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IActivationFactory };
    IID *ret;
    ULONG i;

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = ARRAY_SIZE( iids_arr );
    if (!(ret = CoTaskMemAlloc( sizeof( IID ) * ARRAY_SIZE( iids_arr ) ))) return E_OUTOFMEMORY;

    for (i = 0; i < ARRAY_SIZE( iids_arr ); i++)
        ret[i] = *iids_arr[i];

    *iids = ret;
    return S_OK;
}

static HRESULT WINAPI ble_adv_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisement";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI ble_adv_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI ble_adv_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "(%p, %p)\n", iface, instance );
    return ble_adv_create( (IBluetoothLEAdvertisement **)instance );
}

static const struct IActivationFactoryVtbl ble_adv_factory_vtbl =
{
    ble_adv_factory_QueryInterface,
    ble_adv_factory_AddRef,
    ble_adv_factory_Release,
    ble_adv_factory_GetIids,
    ble_adv_factory_GetRuntimeClassName,
    ble_adv_factory_GetTrustLevel,
    ble_adv_factory_ActivateInstance
};

static struct ble_adv_factory ble_adv_factory =
{
    {&ble_adv_factory_vtbl},
    1
};

IActivationFactory *advertisement_factory = &ble_adv_factory.IActivationFactory_iface;
