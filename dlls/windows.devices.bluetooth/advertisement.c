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
#undef INITGUID

#include "wine/debug.h"

#define IOCTL_WINEBTH_RADIO_START_DISCOVERY CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xa6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_WINEBTH_RADIO_STOP_DISCOVERY  CTL_CODE(FILE_DEVICE_BLUETOOTH, 0xa7, METHOD_BUFFERED, FILE_ANY_ACCESS)

WINE_DEFAULT_DEBUG_CHANNEL( bluetooth );

static HRESULT ble_adv_create( IBluetoothLEAdvertisement **adv );
static HRESULT ble_adv_create_with_name( HSTRING name, IBluetoothLEAdvertisement **adv );

static BOOL get_iface_friendly_name( HDEVINFO devinfo, SP_DEVICE_INTERFACE_DATA *iface_data,
                                     SP_DEVINFO_DATA *devinfo_data, WCHAR *out, DWORD out_len )
{
    DEVPROPTYPE prop_type;
    DWORD size = 0;

    if (SetupDiGetDeviceInterfacePropertyW( devinfo, iface_data, &DEVPKEY_DeviceInterface_FriendlyName,
                                            &prop_type, (BYTE *)out, out_len * sizeof(WCHAR), &size, 0 ))
        return TRUE;
    if (SetupDiGetDevicePropertyW( devinfo, devinfo_data, &DEVPKEY_Device_FriendlyName,
                                   &prop_type, (BYTE *)out, out_len * sizeof(WCHAR), &size, 0 ))
        return TRUE;
    if (SetupDiGetDevicePropertyW( devinfo, devinfo_data, &DEVPKEY_Device_DeviceDesc,
                                   &prop_type, (BYTE *)out, out_len * sizeof(WCHAR), &size, 0 ))
        return TRUE;

    if (SetupDiGetDeviceRegistryPropertyW( devinfo, devinfo_data, SPDRP_FRIENDLYNAME,
                                           NULL, (BYTE *)out, out_len * sizeof(WCHAR), &size ))
        return TRUE;
    if (SetupDiGetDeviceRegistryPropertyW( devinfo, devinfo_data, SPDRP_DEVICEDESC,
                                           NULL, (BYTE *)out, out_len * sizeof(WCHAR), &size ))
        return TRUE;

    HKEY hkey = SetupDiOpenDeviceInterfaceRegKey( devinfo, iface_data, 0, KEY_READ );
    if (hkey != INVALID_HANDLE_VALUE)
    {
        DWORD type = 0, cb = out_len * sizeof(WCHAR);
        LONG rc = RegQueryValueExW( hkey, L"FriendlyName", NULL, &type, (BYTE *)out, &cb );
        if (rc == ERROR_SUCCESS && type == REG_SZ)
        {
            RegCloseKey( hkey );
            return TRUE;
        }
        cb = out_len * sizeof(WCHAR);
        rc = RegQueryValueExW( hkey, L"DeviceDesc", NULL, &type, (BYTE *)out, &cb );
        RegCloseKey( hkey );
        if (rc == ERROR_SUCCESS && type == REG_SZ)
            return TRUE;
    }

    return FALSE;
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
};

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

    ERR( "=== open_first_radio CALLED ===\n" );

    devinfo = SetupDiGetClassDevsW( &my_GUID_BTHPORT_DEVICE_INTERFACE, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE );
    if (devinfo == INVALID_HANDLE_VALUE)
    {
        ERR( "SetupDiGetClassDevsW failed: %lu\n", GetLastError() );
        WARN( "SetupDiGetClassDevsW failed: %lu\n", GetLastError() );
    }
    else
    {
        ERR( "SetupDiGetClassDevsW succeeded, devinfo=%p\n", devinfo );

        iface_detail->cbSize = sizeof( *iface_detail );
        iface_data.cbSize = sizeof( iface_data );

        while (SetupDiEnumDeviceInterfaces( devinfo, NULL, &my_GUID_BTHPORT_DEVICE_INTERFACE, idx++, &iface_data ))
        {
            ERR( "Found device interface idx=%lu\n", idx-1 );
            if (!SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, iface_detail, sizeof( buffer ), NULL, NULL ))
            {
                ERR( "SetupDiGetDeviceInterfaceDetailW failed: %lu\n", GetLastError() );
                continue;
            }
            ERR( "Device path: %s\n", debugstr_w( iface_detail->DevicePath ) );
            radio = CreateFileW( iface_detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
            if (radio != INVALID_HANDLE_VALUE)
            {
                ERR( "Opened radio device: %s radio=%p\n", debugstr_w( iface_detail->DevicePath ), radio );
                TRACE( "Opened radio device: %s\n", debugstr_w( iface_detail->DevicePath ) );
                break;
            }
            ERR( "CreateFileW failed: %lu\n", GetLastError() );
        }
        SetupDiDestroyDeviceInfoList( devinfo );
    }

    ERR( "=== FALLBACK CHECK: radio=%p INVALID=%p equal=%d ===\n", radio, INVALID_HANDLE_VALUE, radio == INVALID_HANDLE_VALUE );
    if (radio == INVALID_HANDLE_VALUE)
    {
        WCHAR direct_path[64];
        int i;
        ERR( "=== ENTERING GLOBALROOT FALLBACK ===\n" );
        for (i = 0; i < 4 && radio == INVALID_HANDLE_VALUE; i++)
        {
            swprintf( direct_path, ARRAY_SIZE( direct_path ), L"\\\\?\\GLOBALROOT\\Device\\WINEBTH-RADIO-%d", i );
            ERR( "Trying direct path: %s\n", debugstr_w( direct_path ) );
            radio = CreateFileW( direct_path, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
            if (radio != INVALID_HANDLE_VALUE)
            {
                ERR( "Opened radio via direct path: %s radio=%p\n", debugstr_w( direct_path ), radio );
                TRACE( "Opened radio via direct path: %s\n", debugstr_w( direct_path ) );
            }
            else
            {
                ERR( "CreateFileW direct path failed: %lu\n", GetLastError() );
            }
        }
    }

    ERR( "=== open_first_radio returning radio=%p (enumerated %lu devices) ===\n", radio, idx );
    return radio;
}

static const GUID GUID_BLUETOOTHLE_DEVICE_INTERFACE = { 0x781aee18, 0x7733, 0x4ce4, { 0xad, 0xd0, 0x91, 0xf4, 0x1c, 0x67, 0xb5, 0x92 } };

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

    EnterCriticalSection( &device_name_cache_cs );

    for (entry = device_name_cache; entry; entry = entry->next)
    {
        if (entry->address == address)
        {
            if (name && name[0])
                lstrcpynW( entry->name, name, ARRAY_SIZE( entry->name ) );
            LeaveCriticalSection( &device_name_cache_cs );
            return;
        }
    }

    entry = malloc( sizeof( *entry ) );
    if (entry)
    {
        entry->address = address;
        if (name && name[0])
            lstrcpynW( entry->name, name, ARRAY_SIZE( entry->name ) );
        else
            entry->name[0] = 0;
        entry->next = device_name_cache;
        device_name_cache = entry;
    }

    LeaveCriticalSection( &device_name_cache_cs );
}

static UINT64 byte_swap_uint64( UINT64 val )
{
    return ((val & 0xff00000000000000ULL) >> 56) |
           ((val & 0x00ff000000000000ULL) >> 40) |
           ((val & 0x0000ff0000000000ULL) >> 24) |
           ((val & 0x000000ff00000000ULL) >> 8) |
           ((val & 0x00000000ff000000ULL) << 8) |
           ((val & 0x0000000000ff0000ULL) << 24) |
           ((val & 0x000000000000ff00ULL) << 40) |
           ((val & 0x00000000000000ffULL) << 56);
}

static BOOL get_device_name_from_driver( HANDLE radio, UINT64 address, WCHAR *out, DWORD out_len )
{
    BTH_DEVICE_INFO_LIST *list;
    DWORD bytes_returned;
    BTH_ADDR bth_addr;
    BOOL found = FALSE;
    DWORD i;
    DWORD device_count = 0;
    SIZE_T buffer_size;
    BYTE *buffer = NULL;

    ERR( "=== get_device_name_from_driver: address=%I64x ===\n", address );
    
    buffer_size = sizeof(BTH_DEVICE_INFO_LIST);
    buffer = malloc( buffer_size );
    if (!buffer) return FALSE;
    memset( buffer, 0, buffer_size );
    list = (BTH_DEVICE_INFO_LIST *)buffer;

    if (DeviceIoControl( radio, IOCTL_BTH_GET_DEVICE_INFO, NULL, 0, list, buffer_size, &bytes_returned, NULL ))
    {
        device_count = list->numOfDevices;
        ERR( "=== IOCTL succeeded with minimal buffer, found %lu devices ===\n", device_count );
        if (device_count == 0)
        {
            free( buffer );
            return FALSE;
        }
        
        free( buffer );
        if (device_count == 1)
            buffer_size = sizeof(BTH_DEVICE_INFO_LIST);
        else
            buffer_size = sizeof(BTH_DEVICE_INFO_LIST) + sizeof(BTH_DEVICE_INFO) * (device_count - 1);
        
        buffer = malloc( buffer_size );
        if (!buffer) return FALSE;
        memset( buffer, 0, buffer_size );
        list = (BTH_DEVICE_INFO_LIST *)buffer;
        list->numOfDevices = device_count;
        
        ERR( "=== Retrying IOCTL with exact_size=%lu for %lu devices ===\n", buffer_size, device_count );
        if (!DeviceIoControl( radio, IOCTL_BTH_GET_DEVICE_INFO, NULL, 0, list, buffer_size, &bytes_returned, NULL ))
        {
            ERR( "=== Retry IOCTL failed: %lu ===\n", GetLastError() );
            free( buffer );
            return FALSE;
        }
    }
    else
    {
        DWORD err = GetLastError();
        ERR( "=== First IOCTL failed: %lu, numOfDevices=%lu ===\n", err, list->numOfDevices );
        free( buffer );
        return FALSE;
    }

    UINT64 search_addr_48 = address & 0xFFFFFFFFFFFFULL;
    ERR( "=== Searching %lu devices for address %I64x (48-bit=%I64x) ===\n", device_count, address, search_addr_48 );
    for (i = 0; i < device_count; i++)
    {
        ERR( "=== Device %lu: driver_addr=%I64x flags=%lu name='%s' ===\n", i, list->deviceList[i].address, list->deviceList[i].flags, list->deviceList[i].name );
        if (list->deviceList[i].address == search_addr_48 && (list->deviceList[i].flags & BDIF_NAME))
        {
            int len = MultiByteToWideChar( CP_UTF8, 0, list->deviceList[i].name, -1, out, out_len );
            if (len > 0)
            {
                ERR( "=== MATCH! Found device name: %s ===\n", debugstr_w( out ) );
                found = TRUE;
                break;
            }
        }
    }
    if (!found)
        ERR( "=== No matching device found in driver list ===\n" );

    free( buffer );
    return found;
}

static BOOL get_cached_device_name( UINT64 address, WCHAR *out, DWORD out_len )
{
    struct device_name_cache_entry *entry;

    if (!device_name_cache_cs_initialized)
        return FALSE;

    EnterCriticalSection( &device_name_cache_cs );
    for (entry = device_name_cache; entry; entry = entry->next)
    {
        if (entry->address == address && entry->name[0])
        {
            lstrcpynW( out, entry->name, out_len );
            LeaveCriticalSection( &device_name_cache_cs );
            return TRUE;
        }
    }
    LeaveCriticalSection( &device_name_cache_cs );
    return FALSE;
}

static DWORD WINAPI adv_watcher_event_thread( void *param )
{
    struct adv_watcher *watcher = param;
    BYTE buffer[sizeof( SP_DEVICE_INTERFACE_DETAIL_DATA_W ) + MAX_PATH * sizeof( WCHAR )];
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (void *)buffer;
    SP_DEVICE_INTERFACE_DATA iface_data;
    SP_DEVINFO_DATA devinfo_data;
    HDEVINFO devinfo;
    DWORD idx;

    init_device_name_cache();

    ERR( "=== Event thread STARTED for watcher %p ===\n", watcher );

    detail->cbSize = sizeof( *detail );
    iface_data.cbSize = sizeof( iface_data );
    devinfo_data.cbSize = sizeof( devinfo_data );

    while (watcher->running)
    {
        devinfo = SetupDiGetClassDevsW( &GUID_BLUETOOTHLE_DEVICE_INTERFACE, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE );
        ERR( "=== SetupDiGetClassDevsW(BTLE) = %p (INVALID=%d) ===\n", devinfo, devinfo == INVALID_HANDLE_VALUE );
        if (devinfo != INVALID_HANDLE_VALUE)
        {
            idx = 0;
            while (SetupDiEnumDeviceInterfaces( devinfo, NULL, &GUID_BLUETOOTHLE_DEVICE_INTERFACE, idx++, &iface_data ) && watcher->running)
            {
                UINT64 addr = 0;
                const WCHAR *path;
                const WCHAR *addr_ptr;
                WCHAR device_name[256];

                ERR( "=== Found BTLE device interface idx=%lu ===\n", idx - 1 );

                if (!SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, detail, sizeof( buffer ), NULL, &devinfo_data ))
                    continue;

                path = detail->DevicePath;
                ERR( "=== BTLE device path: %s ===\n", debugstr_w( path ) );

                addr_ptr = wcsstr( path, L"&" );
                if (addr_ptr)
                {
                    addr_ptr++;
                    swscanf( addr_ptr, L"%I64x", &addr );
                    ERR( "=== Parsed address from '&': %I64x ===\n", addr );
                }
                else
                {
                    addr_ptr = wcsstr( path, L"_" );
                    if (addr_ptr)
                    {
                        addr_ptr++;
                        swscanf( addr_ptr, L"%I64x", &addr );
                        ERR( "=== Parsed address from '_': %I64x ===\n", addr );
                    }
                    else
                    {
                        ERR( "=== No address separator found in path! ===\n" );
                    }
                }

                device_name[0] = 0;
                if (addr)
                {
                    ERR( "=== Looking up name for address %I64x, radio=%p (INVALID=%p) ===\n", addr, watcher->radio, INVALID_HANDLE_VALUE );
                    if (!get_cached_device_name( addr, device_name, ARRAY_SIZE( device_name ) ))
                    {
                        ERR( "=== Not in cache, checking driver... ===\n" );
                        if (watcher->radio != INVALID_HANDLE_VALUE)
                        {
                            ERR( "=== Calling get_device_name_from_driver ===\n" );
                            if (get_device_name_from_driver( watcher->radio, addr, device_name, ARRAY_SIZE( device_name ) ))
                            {
                                ERR( "=== Driver returned name: %s ===\n", debugstr_w( device_name ) );
                                cache_device_name( addr, device_name );
                            }
                            else
                            {
                                ERR( "=== Driver lookup failed ===\n" );
                            }
                        }
                        else
                        {
                            ERR( "=== Radio handle is INVALID, skipping driver lookup ===\n" );
                        }
                        if (!device_name[0])
                        {
                            ERR( "=== Falling back to SetupAPI lookup ===\n" );
                            if (!get_iface_friendly_name( devinfo, &iface_data, &devinfo_data, device_name, ARRAY_SIZE( device_name ) ))
                            {
                                Sleep( 200 );
                                if (!get_iface_friendly_name( devinfo, &iface_data, &devinfo_data, device_name, ARRAY_SIZE( device_name ) ))
                                {
                                    Sleep( 300 );
                                    get_iface_friendly_name( devinfo, &iface_data, &devinfo_data, device_name, ARRAY_SIZE( device_name ) );
                                }
                            }
                            if (device_name[0])
                            {
                                ERR( "=== SetupAPI returned name: %s ===\n", debugstr_w( device_name ) );
                                cache_device_name( addr, device_name );
                            }
                        }
                    }
                    else
                    {
                        ERR( "=== Found name in cache: %s ===\n", debugstr_w( device_name ) );
                    }
                }
                if (device_name[0])
                    ERR( "=== Device FriendlyName: %s ===\n", debugstr_w( device_name ) );
                else
                    ERR( "=== Device name is empty, will use empty string ===\n" );

                if (!wcscmp( device_name, L"BLE Device" ))
                {
                    ERR( "=== Skipping device with placeholder name 'BLE Device' ===\n" );
                    continue;
                }

                if (addr)
                {
                    EnterCriticalSection( &watcher->cs );
                    if (watcher->received_handler && watcher->running)
                    {
                        IBluetoothLEAdvertisementReceivedEventArgs *args;
                        HRESULT hr = adv_received_event_args_create( addr, -50, device_name[0] ? device_name : NULL, &args );
                        if (SUCCEEDED( hr ))
                        {
                            ERR( "=== Invoking Received handler for BLE device %I64x, name=%s ===\n", addr, debugstr_w( device_name ) );
                            ITypedEventHandler_BluetoothLEAdvertisementWatcher_BluetoothLEAdvertisementReceivedEventArgs_Invoke(
                                watcher->received_handler, &watcher->IBluetoothLEAdvertisementWatcher_iface, args );
                            IBluetoothLEAdvertisementReceivedEventArgs_Release( args );
                        }
                    }
                    LeaveCriticalSection( &watcher->cs );
                }
            }
            SetupDiDestroyDeviceInfoList( devinfo );
        }

        Sleep( 1000 );
    }

    TRACE( "Event thread exiting for watcher %p\n", watcher );
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
        if (impl->running)
        {
            impl->running = FALSE;
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
        DeleteCriticalSection( &impl->cs );
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

    ERR( "=== adv_watcher_Start CALLED === iface=%p impl=%p\n", iface, impl );
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
    TRACE( "Watcher started successfully\n" );

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
    TRACE( "Watcher stopped successfully\n" );

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

    TRACE( "Registered Received handler, token=%I64x\n", token->value );
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
    TRACE( "Created advertisement watcher %p\n", impl );
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
    HRESULT hr;
    ERR( "=== adv_watcher_factory_ActivateInstance CALLED === iface=%p instance=%p\n", iface, instance );
    hr = adv_watcher_create( (IBluetoothLEAdvertisementWatcher **)instance );
    ERR( "=== adv_watcher_factory_ActivateInstance result: hr=0x%08lx instance=%p ===\n", hr, *instance );
    return hr;
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
    if (!ref) free( impl );
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
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI adv_filter_put_Advertisement( IBluetoothLEAdvertisementFilter *iface,
                                                     IBluetoothLEAdvertisement *value )
{
    TRACE( "(%p, %p): stub!\n", iface, value );
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
    TRACE( "Created advertisement filter %p\n", impl );
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
    TRACE( "Created BLE advertisement %p\n", impl );
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
    TRACE( "Created BLE advertisement %p with name %s\n", impl, debugstr_hstring( name ) );
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
