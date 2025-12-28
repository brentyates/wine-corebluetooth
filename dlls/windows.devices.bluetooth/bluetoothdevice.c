/* BluetoothDevice, BluetoothLEDevice Implementation
 *
 * Copyright 2025 Vibhav Pant
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
#include <stdio.h>

#include <assert.h>

#define INITGUID
#include <setupapi.h>
#include <devguid.h>
#include <devpkey.h>
#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include <bthdef.h>
#include <bthioctl.h>
#include <bthledef.h>
#include <winioctl.h>
#include <ddk/bthguid.h>
#include <winreg.h>
#include <wine/winebth.h>
#undef INITGUID

static void bth_le_uuid_to_guid( const BTH_LE_UUID *bth, GUID *guid )
{
    if (bth->IsShortUuid)
    {
        guid->Data1 = bth->Value.ShortUuid;
        guid->Data2 = 0x0000;
        guid->Data3 = 0x1000;
        guid->Data4[0] = 0x80; guid->Data4[1] = 0x00;
        guid->Data4[2] = 0x00; guid->Data4[3] = 0x80;
        guid->Data4[4] = 0x5f; guid->Data4[5] = 0x9b;
        guid->Data4[6] = 0x34; guid->Data4[7] = 0xfb;
    }
    else
    {
        *guid = bth->Value.LongUuid;
    }
}

#define INITGUID
#include "robuffer.h"
#define WIDL_using_Windows_Storage_Streams  
#include "windows.storage.streams.h"
#undef INITGUID

#include <initguid.h>
DEFINE_GUID(IID___x_ABI_CWindows_CStorage_CStreams_CIBufferByteAccess, 0x905a0fef, 0xbc53, 0x11df, 0x8c,0x49, 0x00,0x1e,0x4f,0xc6,0x86,0xda);

#include "wine/debug.h"
#include "wine/list.h"

WINE_DEFAULT_DEBUG_CHANNEL( bluetooth );

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
    if (devinfo != INVALID_HANDLE_VALUE)
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


struct handler_entry
{
    struct list entry;
    ITypedEventHandler_BluetoothLEDevice_IInspectable *handler;
    EventRegistrationToken token;
};

#define IBuffer __x_ABI_CWindows_CStorage_CStreams_CIBuffer
#define IBufferVtbl __x_ABI_CWindows_CStorage_CStreams_CIBufferVtbl
#define IID_IBuffer IID___x_ABI_CWindows_CStorage_CStreams_CIBuffer
#define IBuffer_AddRef __x_ABI_CWindows_CStorage_CStreams_CIBuffer_AddRef
#define IBuffer_Release __x_ABI_CWindows_CStorage_CStreams_CIBuffer_Release
#define IBuffer_QueryInterface __x_ABI_CWindows_CStorage_CStreams_CIBuffer_QueryInterface
#define IBuffer_get_Length __x_ABI_CWindows_CStorage_CStreams_CIBuffer_get_Length
#define IBufferByteAccess_Release __x_ABI_CWindows_CStorage_CStreams_CIBufferByteAccess_Release
#define IBufferByteAccess_Buffer __x_ABI_CWindows_CStorage_CStreams_CIBufferByteAccess_Buffer
#define IBufferByteAccess __x_ABI_CWindows_CStorage_CStreams_CIBufferByteAccess
#define IBufferByteAccessVtbl __x_ABI_CWindows_CStorage_CStreams_CIBufferByteAccessVtbl
#define IID_IBufferByteAccess IID___x_ABI_CWindows_CStorage_CStreams_CIBufferByteAccess
#define IBufferByteAccess_AddRef __x_ABI_CWindows_CStorage_CStreams_CIBufferByteAccess_AddRef
#define IGattCharacteristic __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic
#define IGattCharacteristicVtbl __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicVtbl
#define IID_IGattCharacteristic IID___x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristic
typedef enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattCharacteristicProperties GattCharacteristicProperties;
typedef enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattProtectionLevel GattProtectionLevel;
typedef enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattWriteOption GattWriteOption;
typedef enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattClientCharacteristicConfigurationDescriptorValue GattClientCharacteristicConfigurationDescriptorValue;

static const GUID btle_device_interface_guid = { 0x781aee18, 0x7733, 0x4ce4, { 0xad, 0xd0, 0x91, 0xf4, 0x1c, 0x67, 0xb5, 0x92 } };
static const GUID btle_att_base_guid = { 0, 0, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };

/* Internal IBuffer implementation for GATT data */
struct gatt_buffer
{
    IBuffer IBuffer_iface;
    IBufferByteAccess IBufferByteAccess_iface;
    LONG ref;
    UINT32 capacity;
    UINT32 length;
    BYTE data[];
};

static inline struct gatt_buffer *impl_from_IBuffer( IBuffer *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_buffer, IBuffer_iface );
}

static HRESULT WINAPI gatt_buffer_QueryInterface( IBuffer *iface, REFIID iid, void **out )
{
    struct gatt_buffer *impl = impl_from_IBuffer( iface );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBuffer ))
    {
        IBuffer_AddRef( (*out = &impl->IBuffer_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBufferByteAccess ))
    {
        IBufferByteAccess_AddRef( (*out = &impl->IBufferByteAccess_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_buffer_AddRef( IBuffer *iface )
{
    struct gatt_buffer *impl = impl_from_IBuffer( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_buffer_Release( IBuffer *iface )
{
    struct gatt_buffer *impl = impl_from_IBuffer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_buffer_GetIids( IBuffer *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "(%p, %p, %p)\n", iface, iid_count, iids );
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI gatt_buffer_GetRuntimeClassName( IBuffer *iface, HSTRING *class_name )
{
    return WindowsCreateString( L"Windows.Storage.Streams.Buffer", 30, class_name );
}

static HRESULT WINAPI gatt_buffer_GetTrustLevel( IBuffer *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_buffer_get_Capacity( IBuffer *iface, UINT32 *value )
{
    struct gatt_buffer *impl = impl_from_IBuffer( iface );
    if (!value) return E_POINTER;
    *value = impl->capacity;
    return S_OK;
}

static HRESULT WINAPI gatt_buffer_get_Length( IBuffer *iface, UINT32 *value )
{
    struct gatt_buffer *impl = impl_from_IBuffer( iface );
    if (!value) return E_POINTER;
    *value = impl->length;
    return S_OK;
}

static HRESULT WINAPI gatt_buffer_put_Length( IBuffer *iface, UINT32 value )
{
    struct gatt_buffer *impl = impl_from_IBuffer( iface );
    if (value > impl->capacity) return E_INVALIDARG;
    impl->length = value;
    return S_OK;
}

static const IBufferVtbl gatt_buffer_vtbl =
{
    gatt_buffer_QueryInterface,
    gatt_buffer_AddRef,
    gatt_buffer_Release,
    gatt_buffer_GetIids,
    gatt_buffer_GetRuntimeClassName,
    gatt_buffer_GetTrustLevel,
    gatt_buffer_get_Capacity,
    gatt_buffer_get_Length,
    gatt_buffer_put_Length,
};

static inline struct gatt_buffer *impl_from_IBufferByteAccess( IBufferByteAccess *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_buffer, IBufferByteAccess_iface );
}

static HRESULT WINAPI gatt_buffer_byte_access_QueryInterface( IBufferByteAccess *iface, REFIID iid, void **out )
{
    struct gatt_buffer *impl = impl_from_IBufferByteAccess( iface );
    return gatt_buffer_QueryInterface( &impl->IBuffer_iface, iid, out );
}

static ULONG WINAPI gatt_buffer_byte_access_AddRef( IBufferByteAccess *iface )
{
    struct gatt_buffer *impl = impl_from_IBufferByteAccess( iface );
    return gatt_buffer_AddRef( &impl->IBuffer_iface );
}

static ULONG WINAPI gatt_buffer_byte_access_Release( IBufferByteAccess *iface )
{
    struct gatt_buffer *impl = impl_from_IBufferByteAccess( iface );
    return gatt_buffer_Release( &impl->IBuffer_iface );
}

static HRESULT WINAPI gatt_buffer_byte_access_Buffer( IBufferByteAccess *iface, byte **value )
{
    struct gatt_buffer *impl = impl_from_IBufferByteAccess( iface );
    if (!value) return E_POINTER;
    *value = impl->data;
    return S_OK;
}

static const IBufferByteAccessVtbl gatt_buffer_byte_access_vtbl =
{
    gatt_buffer_byte_access_QueryInterface,
    gatt_buffer_byte_access_AddRef,
    gatt_buffer_byte_access_Release,
    gatt_buffer_byte_access_Buffer,
};

static HRESULT gatt_buffer_create( const BYTE *data, UINT32 size, IBuffer **out )
{
    struct gatt_buffer *impl;
    SIZE_T alloc_size = offsetof( struct gatt_buffer, data[size] );
    if (!(impl = malloc( alloc_size ))) return E_OUTOFMEMORY;
    impl->IBuffer_iface.lpVtbl = &gatt_buffer_vtbl;
    impl->IBufferByteAccess_iface.lpVtbl = &gatt_buffer_byte_access_vtbl;
    impl->ref = 1;
    impl->capacity = size;
    impl->length = size;
    if (data && size > 0)
        memcpy( impl->data, data, size );
    *out = &impl->IBuffer_iface;
    return S_OK;
}

struct bluetoothdevice_statics
{
    IActivationFactory IActivationFactory_iface;
    IBluetoothDeviceStatics IBluetoothDeviceStatics_iface;
    LONG ref;
};

static inline struct bluetoothdevice_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct bluetoothdevice_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct bluetoothdevice_statics *impl = impl_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p) %ld\n", iface, debugstr_guid( iid ), out, impl->ref );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothDeviceStatics))
    {
        IBluetoothDeviceStatics_AddRef(( *out = &impl->IBluetoothDeviceStatics_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct bluetoothdevice_statics *impl = impl_from_IActivationFactory( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct bluetoothdevice_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IActivationFactory, &IID_IBluetoothDeviceStatics };
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

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.BluetoothDevice";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "(%p, %p)\n", iface, instance );
    if (!instance) return E_POINTER;
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory */
    factory_ActivateInstance
};

DEFINE_IINSPECTABLE( bluetoothdevice_statics, IBluetoothDeviceStatics, struct bluetoothdevice_statics, IActivationFactory_iface );

static HRESULT bluetooth_device_create( HSTRING id, UINT64 address, IBluetoothDevice **out );
static HRESULT async_bt_device_op_create( IBluetoothDevice *device, IAsyncOperation_BluetoothDevice **out );
static HRESULT gatt_services_vector_create( BTH_LE_GATT_SERVICE *services, ULONG count, HANDLE device_handle, BOOL is_radio_handle, const WCHAR *device_id, UINT64 device_address, IVectorView_GattDeviceService **out );

static BOOL get_device_name_from_id( const WCHAR *device_id, UINT64 address, WCHAR *name, DWORD name_len )
{
    HDEVINFO devinfo;
    SP_DEVICE_INTERFACE_DATA iface_data;
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
    SP_DEVINFO_DATA devinfo_data;
    BYTE buffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + MAX_PATH * sizeof(WCHAR)];
    DWORD idx = 0;
    BOOL found = FALSE;
    DEVPROPTYPE prop_type;
    DWORD size = 0;

    name[0] = 0;

    devinfo = SetupDiGetClassDevsW( &btle_device_interface_guid, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT );
    if (devinfo == INVALID_HANDLE_VALUE)
        return FALSE;

    detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    iface_data.cbSize = sizeof(iface_data);
    devinfo_data.cbSize = sizeof(devinfo_data);

    while (SetupDiEnumDeviceInterfaces( devinfo, NULL, &btle_device_interface_guid, idx++, &iface_data ))
    {
        if (!SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, detail, sizeof(buffer), NULL, &devinfo_data ))
            continue;

        {
            const WCHAR *path = detail->DevicePath;
            UINT64 path_addr = 0;
            const WCHAR *addr_ptr = wcsstr( path, L"&" );
            if (addr_ptr)
                swscanf( addr_ptr + 1, L"%I64x", &path_addr );
            else if ((addr_ptr = wcsstr( path, L"_" )))
                swscanf( addr_ptr + 1, L"%I64x", &path_addr );

            if (path_addr == address)
            {
                if (SetupDiGetDeviceInterfacePropertyW( devinfo, &iface_data, &DEVPKEY_DeviceInterface_FriendlyName,
                                                       &prop_type, (BYTE *)name, name_len * sizeof(WCHAR), &size, 0 ))
                {
                    found = TRUE;
                    break;
                }
                if (SetupDiGetDevicePropertyW( devinfo, &devinfo_data, &DEVPKEY_Device_FriendlyName,
                                              &prop_type, (BYTE *)name, name_len * sizeof(WCHAR), &size, 0 ))
                {
                    found = TRUE;
                    break;
                }
                if (SetupDiGetDevicePropertyW( devinfo, &devinfo_data, &DEVPKEY_Device_DeviceDesc,
                                              &prop_type, (BYTE *)name, name_len * sizeof(WCHAR), &size, 0 ))
                {
                    found = TRUE;
                    break;
                }
                if (SetupDiGetDeviceRegistryPropertyW( devinfo, &devinfo_data, SPDRP_FRIENDLYNAME,
                                                      NULL, (BYTE *)name, name_len * sizeof(WCHAR), &size ))
                {
                    found = TRUE;
                    break;
                }
                if (SetupDiGetDeviceRegistryPropertyW( devinfo, &devinfo_data, SPDRP_DEVICEDESC,
                                                      NULL, (BYTE *)name, name_len * sizeof(WCHAR), &size ))
                {
                    found = TRUE;
                    break;
                }
            }
        }
    }

    SetupDiDestroyDeviceInfoList( devinfo );
    return found;
}

static HRESULT WINAPI bluetoothdevice_statics_FromIdAsync( IBluetoothDeviceStatics *iface, HSTRING id, IAsyncOperation_BluetoothDevice **async_op )
{
    HRESULT hr;
    IBluetoothDevice *device;
    UINT64 addr = 0;
    const WCHAR *str;

    TRACE( "(%s, %p)\n", debugstr_hstring( id ), async_op );
    if (!async_op) return E_POINTER;

    str = WindowsGetStringRawBuffer( id, NULL );
    {
        const WCHAR *addr_ptr = wcsstr( str, L"&" );
        if (addr_ptr) swscanf( addr_ptr + 1, L"%I64x", &addr );
        else if ((addr_ptr = wcsstr( str, L"_" ))) swscanf( addr_ptr + 1, L"%I64x", &addr );
    }

    hr = bluetooth_device_create( id, addr, &device );
    if (FAILED(hr)) return hr;

    hr = async_bt_device_op_create( device, async_op );
    IBluetoothDevice_Release( device );
    return hr;
}

static HRESULT WINAPI bluetoothdevice_statics_FromHostNameAsync( IBluetoothDeviceStatics *iface, IHostName *name, IAsyncOperation_BluetoothDevice **async_op )
{
    TRACE( "(%p, %p, %p)\n", iface, name, async_op );
    if (!async_op) return E_POINTER;
    *async_op = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI bluetoothdevice_statics_FromBluetoothAddressAsync( IBluetoothDeviceStatics *iface,
                                                                        UINT64 address,
                                                                        IAsyncOperation_BluetoothDevice **async_op )
{
    HRESULT hr;
    IBluetoothDevice *device;
    WCHAR buf[64];
    HSTRING id;

    TRACE( "(%p, %#I64x, %p)\n", iface, address, async_op );
    if (!async_op) return E_POINTER;

    swprintf( buf, ARRAY_SIZE(buf), L"BT:%I64u", address );
    WindowsCreateString( buf, wcslen(buf), &id );

    hr = bluetooth_device_create( id, address, &device );
    WindowsDeleteString( id );
    if (FAILED(hr)) return hr;

    hr = async_bt_device_op_create( device, async_op );
    IBluetoothDevice_Release( device );
    return hr;
}

static HRESULT WINAPI bluetoothdevice_statics_GetDeviceSelector( IBluetoothDeviceStatics *iface, HSTRING *value )
{
    static const WCHAR selector[] = L"System.Devices.InterfaceClassGuid:=\"{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}\"";
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsCreateString( selector, ARRAY_SIZE( selector ) - 1, value );
}

static const IBluetoothDeviceStaticsVtbl bluetoothdevice_statics_vtbl =
{
    bluetoothdevice_statics_QueryInterface,
    bluetoothdevice_statics_AddRef,
    bluetoothdevice_statics_Release,
    /* IInspectable */
    bluetoothdevice_statics_GetIids,
    bluetoothdevice_statics_GetRuntimeClassName,
    bluetoothdevice_statics_GetTrustLevel,
    /* IBluetoothDeviceStatics */
    bluetoothdevice_statics_FromIdAsync,
    bluetoothdevice_statics_FromHostNameAsync,
    bluetoothdevice_statics_FromBluetoothAddressAsync,
    bluetoothdevice_statics_GetDeviceSelector,
};

struct bluetoothledevice_statics
{
    IActivationFactory IActivationFactory_iface;
    IBluetoothLEDeviceStatics IBluetoothLEDeviceStatics_iface;
    IBluetoothLEDeviceStatics2 IBluetoothLEDeviceStatics2_iface;
    LONG ref;
};

static inline struct bluetoothledevice_statics *ble_device_impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct bluetoothledevice_statics, IActivationFactory_iface );
}

static HRESULT WINAPI ble_device_factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct bluetoothledevice_statics *impl = ble_device_impl_from_IActivationFactory( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        IActivationFactory_AddRef(( *out = &impl->IActivationFactory_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothLEDeviceStatics ))
    {
        IBluetoothLEDeviceStatics_AddRef(( *out = &impl->IBluetoothLEDeviceStatics_iface ));
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IBluetoothLEDeviceStatics2 ))
    {
        IBluetoothLEDeviceStatics2_AddRef(( *out = &impl->IBluetoothLEDeviceStatics2_iface ));
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI ble_device_factory_AddRef( IActivationFactory *iface )
{
    struct bluetoothledevice_statics *impl = ble_device_impl_from_IActivationFactory( iface );
    TRACE( "(%p)\n", iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI ble_device_factory_Release( IActivationFactory *iface )
{
    struct bluetoothledevice_statics *impl = ble_device_impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p)\n", iface );
    return ref;
}

static HRESULT WINAPI ble_device_factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IActivationFactory, &IID_IBluetoothLEDeviceStatics };
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

static HRESULT WINAPI ble_device_factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.BluetoothLEDevice";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI ble_device_factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI ble_device_factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "(%p, %p)\n", iface, instance );
    if (!instance) return E_POINTER;
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl ble_device_factory_vtbl =
{
    ble_device_factory_QueryInterface,
    ble_device_factory_AddRef,
    ble_device_factory_Release,
    /* IInspectable */
    ble_device_factory_GetIids,
    ble_device_factory_GetRuntimeClassName,
    ble_device_factory_GetTrustLevel,
    /* IActivationFactory */
    ble_device_factory_ActivateInstance
};



struct bluetooth_device
{
    IBluetoothDevice IBluetoothDevice_iface;
    IClosable IClosable_iface;
    LONG ref;
    HSTRING id;
    UINT64 address;
    WCHAR name[256];
};

static inline struct bluetooth_device *impl_from_IBluetoothDevice( IBluetoothDevice *iface )
{
    return CONTAINING_RECORD( iface, struct bluetooth_device, IBluetoothDevice_iface );
}

static HRESULT WINAPI bt_device_QueryInterface( IBluetoothDevice *iface, REFIID iid, void **out )
{
    struct bluetooth_device *impl = impl_from_IBluetoothDevice( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothDevice ))
    {
        IBluetoothDevice_AddRef( (*out = &impl->IBluetoothDevice_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IClosable_AddRef( (*out = &impl->IClosable_iface) );
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI bt_device_AddRef( IBluetoothDevice *iface )
{
    struct bluetooth_device *impl = impl_from_IBluetoothDevice( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI bt_device_Release( IBluetoothDevice *iface )
{
    struct bluetooth_device *impl = impl_from_IBluetoothDevice( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->id) WindowsDeleteString( impl->id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI bt_device_GetIids( IBluetoothDevice *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IBluetoothDevice, &IID_IClosable };
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

static HRESULT WINAPI bt_device_GetRuntimeClassName( IBluetoothDevice *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.BluetoothDevice";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI bt_device_GetTrustLevel( IBluetoothDevice *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI bt_device_get_DeviceId( IBluetoothDevice *iface, HSTRING *value )
{
    struct bluetooth_device *impl = impl_from_IBluetoothDevice( iface );
    WCHAR buf[32];
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    swprintf( buf, ARRAY_SIZE(buf), L"BT:%012I64X", impl->address );
    return WindowsCreateString( buf, wcslen(buf), value );
}

static HRESULT WINAPI bt_device_get_HostName( IBluetoothDevice *iface, IHostName **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI bt_device_get_Name( IBluetoothDevice *iface, HSTRING *value )
{
    struct bluetooth_device *impl = impl_from_IBluetoothDevice( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsCreateString( impl->name, wcslen(impl->name), value );
}

static HRESULT WINAPI bt_device_get_ClassOfDevice( IBluetoothDevice *iface, IBluetoothClassOfDevice **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI bt_device_get_SdpRecords( IBluetoothDevice *iface, IVectorView_IBuffer **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI bt_device_get_RfcommServices( IBluetoothDevice *iface, IVectorView_RfcommDeviceService **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI bt_device_get_ConnectionStatus( IBluetoothDevice *iface, BluetoothConnectionStatus *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = BluetoothConnectionStatus_Connected;
    return S_OK;
}

static HRESULT WINAPI bt_device_get_BluetoothAddress( IBluetoothDevice *iface, UINT64 *value )
{
    struct bluetooth_device *impl = impl_from_IBluetoothDevice( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->address;
    return S_OK;
}

static HRESULT WINAPI bt_device_add_NameChanged( IBluetoothDevice *iface, ITypedEventHandler_BluetoothDevice_IInspectable *handler, EventRegistrationToken *token )
{
    TRACE( "(%p, %p, %p)\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI bt_device_remove_NameChanged( IBluetoothDevice *iface, EventRegistrationToken token )
{
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI bt_device_add_SdpRecordsChanged( IBluetoothDevice *iface, ITypedEventHandler_BluetoothDevice_IInspectable *handler, EventRegistrationToken *token )
{
    TRACE( "(%p, %p, %p)\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI bt_device_remove_SdpRecordsChanged( IBluetoothDevice *iface, EventRegistrationToken token )
{
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI bt_device_add_ConnectionStatusChanged( IBluetoothDevice *iface, ITypedEventHandler_BluetoothDevice_IInspectable *handler, EventRegistrationToken *token )
{
    TRACE( "(%p, %p, %p)\n", iface, handler, token );
    if (!token) return E_POINTER;
    token->value = 0;
    return S_OK;
}

static HRESULT WINAPI bt_device_remove_ConnectionStatusChanged( IBluetoothDevice *iface, EventRegistrationToken token )
{
    TRACE( "(%p, %I64x)\n", iface, token.value );
    return S_OK;
}

static const IBluetoothDeviceVtbl bt_device_vtbl =
{
    bt_device_QueryInterface,
    bt_device_AddRef,
    bt_device_Release,
    bt_device_GetIids,
    bt_device_GetRuntimeClassName,
    bt_device_GetTrustLevel,
    bt_device_get_DeviceId,
    bt_device_get_HostName,
    bt_device_get_Name,
    bt_device_get_ClassOfDevice,
    bt_device_get_SdpRecords,
    bt_device_get_RfcommServices,
    bt_device_get_ConnectionStatus,
    bt_device_get_BluetoothAddress,
    bt_device_add_NameChanged,
    bt_device_remove_NameChanged,
    bt_device_add_SdpRecordsChanged,
    bt_device_remove_SdpRecordsChanged,
    bt_device_add_ConnectionStatusChanged,
    bt_device_remove_ConnectionStatusChanged
};

static inline struct bluetooth_device *impl_from_IClosable_bt( IClosable *iface )
{
    return CONTAINING_RECORD( iface, struct bluetooth_device, IClosable_iface );
}

static HRESULT WINAPI closable_bt_QueryInterface( IClosable *iface, REFIID iid, void **out )
{
    struct bluetooth_device *impl = impl_from_IClosable_bt( iface );
    return bt_device_QueryInterface( &impl->IBluetoothDevice_iface, iid, out );
}

static ULONG WINAPI closable_bt_AddRef( IClosable *iface )
{
    struct bluetooth_device *impl = impl_from_IClosable_bt( iface );
    return bt_device_AddRef( &impl->IBluetoothDevice_iface );
}

static ULONG WINAPI closable_bt_Release( IClosable *iface )
{
    struct bluetooth_device *impl = impl_from_IClosable_bt( iface );
    return bt_device_Release( &impl->IBluetoothDevice_iface );
}

static HRESULT WINAPI closable_bt_GetIids( IClosable *iface, ULONG *iid_count, IID **iids )
{
    struct bluetooth_device *impl = impl_from_IClosable_bt( iface );
    return bt_device_GetIids( &impl->IBluetoothDevice_iface, iid_count, iids );
}

static HRESULT WINAPI closable_bt_GetRuntimeClassName( IClosable *iface, HSTRING *class_name )
{
    struct bluetooth_device *impl = impl_from_IClosable_bt( iface );
    return bt_device_GetRuntimeClassName( &impl->IBluetoothDevice_iface, class_name );
}

static HRESULT WINAPI closable_bt_GetTrustLevel( IClosable *iface, TrustLevel *level )
{
    struct bluetooth_device *impl = impl_from_IClosable_bt( iface );
    return bt_device_GetTrustLevel( &impl->IBluetoothDevice_iface, level );
}

static HRESULT WINAPI closable_bt_Close( IClosable *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IClosableVtbl closable_bt_vtbl =
{
    closable_bt_QueryInterface,
    closable_bt_AddRef,
    closable_bt_Release,
    closable_bt_GetIids,
    closable_bt_GetRuntimeClassName,
    closable_bt_GetTrustLevel,
    closable_bt_Close
};

static BOOL get_classic_device_name_from_id( const WCHAR *device_id, UINT64 address, WCHAR *name, DWORD name_len )
{
    HDEVINFO devinfo;
    SP_DEVICE_INTERFACE_DATA iface_data;
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
    SP_DEVINFO_DATA devinfo_data;
    BYTE buffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + MAX_PATH * sizeof(WCHAR)];
    DWORD idx = 0;
    BOOL found = FALSE;
    DEVPROPTYPE prop_type;
    DWORD size = 0;
    static const GUID guid_bt_classic = {0xe0cbf06c, 0xcd8b, 0x4647, {0xbb, 0x8a, 0x26, 0x3b, 0x43, 0xf0, 0xf9, 0x74}};

    name[0] = 0;

    devinfo = SetupDiGetClassDevsW( &guid_bt_classic, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT );
    if (devinfo == INVALID_HANDLE_VALUE)
        return FALSE;

    detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    iface_data.cbSize = sizeof(iface_data);
    devinfo_data.cbSize = sizeof(devinfo_data);

    while (SetupDiEnumDeviceInterfaces( devinfo, NULL, &guid_bt_classic, idx++, &iface_data ))
    {
        if (!SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, detail, sizeof(buffer), NULL, &devinfo_data ))
            continue;

        if (SetupDiGetDeviceInterfacePropertyW( devinfo, &iface_data, &DEVPKEY_DeviceInterface_FriendlyName,
                                               &prop_type, (BYTE *)name, name_len * sizeof(WCHAR), &size, 0 ))
        {
            found = TRUE;
            break;
        }
        if (SetupDiGetDevicePropertyW( devinfo, &devinfo_data, &DEVPKEY_Device_FriendlyName,
                                      &prop_type, (BYTE *)name, name_len * sizeof(WCHAR), &size, 0 ))
        {
            found = TRUE;
            break;
        }
        if (SetupDiGetDevicePropertyW( devinfo, &devinfo_data, &DEVPKEY_Device_DeviceDesc,
                                      &prop_type, (BYTE *)name, name_len * sizeof(WCHAR), &size, 0 ))
        {
            found = TRUE;
            break;
        }
        if (SetupDiGetDeviceRegistryPropertyW( devinfo, &devinfo_data, SPDRP_FRIENDLYNAME,
                                              NULL, (BYTE *)name, name_len * sizeof(WCHAR), &size ))
        {
            found = TRUE;
            break;
        }
        if (SetupDiGetDeviceRegistryPropertyW( devinfo, &devinfo_data, SPDRP_DEVICEDESC,
                                              NULL, (BYTE *)name, name_len * sizeof(WCHAR), &size ))
        {
            found = TRUE;
            break;
        }
    }

    SetupDiDestroyDeviceInfoList( devinfo );
    return found;
}

static HRESULT bluetooth_device_create( HSTRING id, UINT64 address, IBluetoothDevice **out )
{
    struct bluetooth_device *impl;
    WCHAR device_name[256];

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothDevice_iface.lpVtbl = &bt_device_vtbl;
    impl->IClosable_iface.lpVtbl = &closable_bt_vtbl;
    impl->ref = 1;
    impl->address = address;
    WindowsDuplicateString( id, &impl->id );

    device_name[0] = 0;
    if (!get_classic_device_name_from_id( WindowsGetStringRawBuffer( id, NULL ), impl->address, device_name, ARRAY_SIZE( device_name ) ))
        wcscpy( device_name, L"" );
    wcscpy( impl->name, device_name );

    *out = &impl->IBluetoothDevice_iface;
    TRACE( "Created BluetoothDevice %p, name=%s address=%I64x\n", impl, debugstr_w( device_name ), impl->address );
    return S_OK;
}

struct async_bt_device_op
{
    IAsyncOperation_BluetoothDevice IAsyncOperation_BluetoothDevice_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IAsyncOperationCompletedHandler_BluetoothDevice *handler;
    IBluetoothDevice *result;
    AsyncStatus status;
};

static inline struct async_bt_device_op *impl_from_IAsyncOperation_BluetoothDevice( IAsyncOperation_BluetoothDevice *iface )
{
    return CONTAINING_RECORD( iface, struct async_bt_device_op, IAsyncOperation_BluetoothDevice_iface );
}

static HRESULT WINAPI async_bt_op_QueryInterface( IAsyncOperation_BluetoothDevice *iface, REFIID iid, void **out )
{
    struct async_bt_device_op *impl = impl_from_IAsyncOperation_BluetoothDevice( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_BluetoothDevice ))
    {
        IAsyncOperation_BluetoothDevice_AddRef( (*out = &impl->IAsyncOperation_BluetoothDevice_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IAsyncInfo_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI async_bt_op_AddRef( IAsyncOperation_BluetoothDevice *iface )
{
    struct async_bt_device_op *impl = impl_from_IAsyncOperation_BluetoothDevice( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_bt_op_Release( IAsyncOperation_BluetoothDevice *iface )
{
    struct async_bt_device_op *impl = impl_from_IAsyncOperation_BluetoothDevice( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->handler) IAsyncOperationCompletedHandler_BluetoothDevice_Release( impl->handler );
        if (impl->result) IBluetoothDevice_Release( impl->result );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_bt_op_GetIids( IAsyncOperation_BluetoothDevice *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IAsyncOperation_BluetoothDevice, &IID_IAsyncInfo };
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

static HRESULT WINAPI async_bt_op_GetRuntimeClassName( IAsyncOperation_BluetoothDevice *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Foundation.IAsyncOperation`1<Windows.Devices.Bluetooth.BluetoothDevice>";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI async_bt_op_GetTrustLevel( IAsyncOperation_BluetoothDevice *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI async_bt_op_put_Completed( IAsyncOperation_BluetoothDevice *iface,
                                                 IAsyncOperationCompletedHandler_BluetoothDevice *handler )
{
    struct async_bt_device_op *impl = impl_from_IAsyncOperation_BluetoothDevice( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;
    impl->handler = handler;
    IAsyncOperationCompletedHandler_BluetoothDevice_AddRef( handler );
    if (impl->status == Started)
        impl->status = Completed;
    IAsyncOperationCompletedHandler_BluetoothDevice_Invoke( handler, iface, impl->status );
    return S_OK;
}

static HRESULT WINAPI async_bt_op_get_Completed( IAsyncOperation_BluetoothDevice *iface,
                                                 IAsyncOperationCompletedHandler_BluetoothDevice **handler )
{
    struct async_bt_device_op *impl = impl_from_IAsyncOperation_BluetoothDevice( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;
    *handler = impl->handler;
    if (*handler) IAsyncOperationCompletedHandler_BluetoothDevice_AddRef( *handler );
    return S_OK;
}

static HRESULT WINAPI async_bt_op_GetResults( IAsyncOperation_BluetoothDevice *iface, IBluetoothDevice **results )
{
    struct async_bt_device_op *impl = impl_from_IAsyncOperation_BluetoothDevice( iface );
    TRACE( "(%p, %p)\n", iface, results );
    if (!results) return E_POINTER;
    *results = impl->result;
    if (*results) IBluetoothDevice_AddRef( *results );
    return S_OK;
}

static const IAsyncOperation_BluetoothDeviceVtbl async_bt_op_vtbl =
{
    async_bt_op_QueryInterface,
    async_bt_op_AddRef,
    async_bt_op_Release,
    async_bt_op_GetIids,
    async_bt_op_GetRuntimeClassName,
    async_bt_op_GetTrustLevel,
    async_bt_op_put_Completed,
    async_bt_op_get_Completed,
    async_bt_op_GetResults
};

static inline struct async_bt_device_op *impl_from_IAsyncInfo_bt( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct async_bt_device_op, IAsyncInfo_iface );
}

static HRESULT WINAPI async_info_bt_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    struct async_bt_device_op *impl = impl_from_IAsyncInfo_bt( iface );
    return IAsyncOperation_BluetoothDevice_QueryInterface( &impl->IAsyncOperation_BluetoothDevice_iface, iid, out );
}

static ULONG WINAPI async_info_bt_AddRef( IAsyncInfo *iface )
{
    struct async_bt_device_op *impl = impl_from_IAsyncInfo_bt( iface );
    return IAsyncOperation_BluetoothDevice_AddRef( &impl->IAsyncOperation_BluetoothDevice_iface );
}

static ULONG WINAPI async_info_bt_Release( IAsyncInfo *iface )
{
    struct async_bt_device_op *impl = impl_from_IAsyncInfo_bt( iface );
    return IAsyncOperation_BluetoothDevice_Release( &impl->IAsyncOperation_BluetoothDevice_iface );
}

static HRESULT WINAPI async_info_bt_GetIids( IAsyncInfo *iface, ULONG *iid_count, IID **iids )
{
    struct async_bt_device_op *impl = impl_from_IAsyncInfo_bt( iface );
    return IAsyncOperation_BluetoothDevice_GetIids( &impl->IAsyncOperation_BluetoothDevice_iface, iid_count, iids );
}

static HRESULT WINAPI async_info_bt_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *class_name )
{
    struct async_bt_device_op *impl = impl_from_IAsyncInfo_bt( iface );
    return IAsyncOperation_BluetoothDevice_GetRuntimeClassName( &impl->IAsyncOperation_BluetoothDevice_iface, class_name );
}

static HRESULT WINAPI async_info_bt_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    struct async_bt_device_op *impl = impl_from_IAsyncInfo_bt( iface );
    return IAsyncOperation_BluetoothDevice_GetTrustLevel( &impl->IAsyncOperation_BluetoothDevice_iface, level );
}

static HRESULT WINAPI async_info_bt_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI async_info_bt_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_bt_device_op *impl = impl_from_IAsyncInfo_bt( iface );
    *status = impl->status;
    return S_OK;
}

static HRESULT WINAPI async_info_bt_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    *error_code = S_OK;
    return S_OK;
}

static HRESULT WINAPI async_info_bt_Cancel( IAsyncInfo *iface )
{
    return S_OK;
}

static HRESULT WINAPI async_info_bt_Close( IAsyncInfo *iface )
{
    return S_OK;
}

static const IAsyncInfoVtbl async_info_bt_vtbl =
{
    async_info_bt_QueryInterface,
    async_info_bt_AddRef,
    async_info_bt_Release,
    async_info_bt_GetIids,
    async_info_bt_GetRuntimeClassName,
    async_info_bt_GetTrustLevel,
    async_info_bt_get_Id,
    async_info_bt_get_Status,
    async_info_bt_get_ErrorCode,
    async_info_bt_Cancel,
    async_info_bt_Close
};

static HRESULT async_bt_device_op_create( IBluetoothDevice *device, IAsyncOperation_BluetoothDevice **out )
{
    struct async_bt_device_op *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_BluetoothDevice_iface.lpVtbl = &async_bt_op_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_info_bt_vtbl;
    impl->ref = 1;
    impl->result = device;
    impl->status = device ? Completed : Error;
    if (device) IBluetoothDevice_AddRef( device );

    *out = &impl->IAsyncOperation_BluetoothDevice_iface;
    return S_OK;
}

static struct bluetoothdevice_statics bluetoothdevice_statics =
{
    {&factory_vtbl},
    {&bluetoothdevice_statics_vtbl},
    1
};

IActivationFactory *bluetoothdevice_statics_factory = &bluetoothdevice_statics.IActivationFactory_iface;


struct bluetooth_le_device
{
    IBluetoothLEDevice IBluetoothLEDevice_iface;
    IBluetoothLEDevice3 IBluetoothLEDevice3_iface;
    IClosable IClosable_iface;
    LONG ref;
    HSTRING id;
    UINT64 address;
    WCHAR name[256];
    HANDLE device_handle;
    struct list name_changed_handlers;
    struct list connection_status_changed_handlers;
    struct list gatt_services_changed_handlers;
    HANDLE connection_monitor_thread;
    BOOL connection_monitor_running;
    BluetoothConnectionStatus last_connection_status;
};

static inline struct bluetooth_le_device *impl_from_IBluetoothLEDevice( IBluetoothLEDevice *iface )
{
    return CONTAINING_RECORD( iface, struct bluetooth_le_device, IBluetoothLEDevice_iface );
}

static HRESULT WINAPI le_device_QueryInterface( IBluetoothLEDevice *iface, REFIID iid, void **out )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothLEDevice ))
    {
        IBluetoothLEDevice_AddRef( (*out = &impl->IBluetoothLEDevice_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IClosable_AddRef( (*out = &impl->IClosable_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IBluetoothLEDevice3 ))
    {
        IBluetoothLEDevice3_AddRef( (*out = &impl->IBluetoothLEDevice3_iface) );
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI le_device_AddRef( IBluetoothLEDevice *iface )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI le_device_Release( IBluetoothLEDevice *iface )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        struct handler_entry *entry, *next;

        if (impl->connection_monitor_thread)
        {
            impl->connection_monitor_running = FALSE;
            WaitForSingleObject( impl->connection_monitor_thread, 1000 );
            CloseHandle( impl->connection_monitor_thread );
        }

        if (impl->id) WindowsDeleteString( impl->id );
        if (impl->device_handle != INVALID_HANDLE_VALUE)
            CloseHandle( impl->device_handle );

        LIST_FOR_EACH_ENTRY_SAFE( entry, next, &impl->name_changed_handlers, struct handler_entry, entry )
        {
            list_remove( &entry->entry );
            ITypedEventHandler_BluetoothLEDevice_IInspectable_Release( entry->handler );
            free( entry );
        }
        LIST_FOR_EACH_ENTRY_SAFE( entry, next, &impl->connection_status_changed_handlers, struct handler_entry, entry )
        {
            list_remove( &entry->entry );
            ITypedEventHandler_BluetoothLEDevice_IInspectable_Release( entry->handler );
            free( entry );
        }
        LIST_FOR_EACH_ENTRY_SAFE( entry, next, &impl->gatt_services_changed_handlers, struct handler_entry, entry )
        {
            list_remove( &entry->entry );
            ITypedEventHandler_BluetoothLEDevice_IInspectable_Release( entry->handler );
            free( entry );
        }

        free( impl );
    }
    return ref;
}

static HRESULT WINAPI le_device_GetIids( IBluetoothLEDevice *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IBluetoothLEDevice, &IID_IClosable };
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

static HRESULT WINAPI le_device_GetRuntimeClassName( IBluetoothLEDevice *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.BluetoothLEDevice";
    if (!class_name) return E_POINTER;
    TRACE("Returning standard RuntimeClassName: %ls\n", class_name_str);
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI le_device_GetTrustLevel( IBluetoothLEDevice *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI le_device_get_DeviceId( IBluetoothLEDevice *iface, HSTRING *value )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    WCHAR buf[32];
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    swprintf( buf, ARRAY_SIZE(buf), L"BT:%012I64X", impl->address );
    return WindowsCreateString( buf, wcslen(buf), value );
}

static HRESULT WINAPI le_device_get_Name( IBluetoothLEDevice *iface, HSTRING *value )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsCreateString( impl->name, wcslen(impl->name), value );
}

static HRESULT WINAPI le_device_get_GattServices( IBluetoothLEDevice *iface, IVectorView_GattDeviceService **value )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    struct winebth_le_device_get_gatt_services_params *params;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    HRESULT hr = S_OK;

    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        *value = NULL;
        return HRESULT_FROM_WIN32( ERROR_DEVICE_NOT_AVAILABLE );
    }

    buffer_size = offsetof( struct winebth_le_device_get_gatt_services_params, services[16] );
    params = malloc( buffer_size );
    if (!params)
    {
        *value = NULL;
        return E_OUTOFMEMORY;
    }

    memset( params, 0, buffer_size );
    params->count = 0;

    if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES, NULL, 0,
                        params, buffer_size, &bytes_returned, NULL ))
    {
        if (params->count > 0)
        {
            TRACE( "Found %lu GATT services, creating vector for synchronous property\n", params->count );
            hr = gatt_services_vector_create( params->services, params->count,
                                               impl->device_handle, FALSE, WindowsGetStringRawBuffer( impl->id, NULL ),
                                               impl->address, value );
        }
        else
        {
            *value = NULL;
        }
    }
    else
    {
        DWORD err = GetLastError();
        WARN( "IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES failed: %lu\n", err );
        hr = HRESULT_FROM_WIN32( err );
        *value = NULL;
    }

    free( params );
    return hr;
}

static HRESULT WINAPI le_device_get_ConnectionStatus( IBluetoothLEDevice *iface, BluetoothConnectionStatus *value )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    DWORD bytes_returned;
    BOOL connected = FALSE;

    TRACE( "(%p, %p) device_handle=%p (%s)\n", iface, value, impl->device_handle,
           impl->device_handle == INVALID_HANDLE_VALUE ? "INVALID" : "valid" );

    if (!value) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        HANDLE radio_handle = open_first_radio();
        if (radio_handle != INVALID_HANDLE_VALUE)
        {
            struct winebth_radio_get_device_connection_status_params params;
            params.address = impl->address;
            params.connected = FALSE;

            if (DeviceIoControl( radio_handle, IOCTL_WINEBTH_RADIO_GET_DEVICE_CONNECTION_STATUS,
                                 &params, sizeof(params), &params, sizeof(params), &bytes_returned, NULL ))
            {
                *value = params.connected ? BluetoothConnectionStatus_Connected : BluetoothConnectionStatus_Disconnected;
                TRACE( "ConnectionStatus from radio: connected=%d\n", params.connected );
                CloseHandle( radio_handle );
                return S_OK;
            }
            CloseHandle( radio_handle );
        }
        *value = BluetoothConnectionStatus_Disconnected;
        return S_OK;
    }

    if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_CONNECTION_STATUS,
                         NULL, 0, &connected, sizeof(connected), &bytes_returned, NULL ))
    {
        *value = connected ? BluetoothConnectionStatus_Connected : BluetoothConnectionStatus_Disconnected;
        TRACE( "ConnectionStatus from driver: connected=%d status=%d (0=Disconnected,1=Connected)\n", connected, *value );
    }
    else
    {
        WARN( "IOCTL_WINEBTH_LE_DEVICE_GET_CONNECTION_STATUS failed: error=%lu\n", GetLastError() );
        *value = BluetoothConnectionStatus_Disconnected;
        TRACE( "ConnectionStatus IOCTL FAILED, returning Disconnected\n" );
    }

    return S_OK;
}

static HRESULT WINAPI le_device_get_BluetoothAddress( IBluetoothLEDevice *iface, UINT64 *value )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->address;
    return S_OK;
}

static HRESULT WINAPI le_device_GetGattService( IBluetoothLEDevice *iface, GUID service_uuid, void **service )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    struct winebth_le_device_get_gatt_services_params *params;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    DWORD i;
    HRESULT hr = HRESULT_FROM_WIN32( ERROR_NOT_FOUND );

    FIXME( "=== GetGattService (SYNC): iface=%p UUID=%s \n", iface, debugstr_guid( &service_uuid ) );
    if (!service) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        *service = NULL;
        return HRESULT_FROM_WIN32( ERROR_DEVICE_NOT_AVAILABLE );
    }

    buffer_size = offsetof( struct winebth_le_device_get_gatt_services_params, services[16] );
    params = malloc( buffer_size );
    if (!params)
    {
        *service = NULL;
        return E_OUTOFMEMORY;
    }

    memset( params, 0, buffer_size );
    params->count = 0;

    if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES, NULL, 0,
                        params, buffer_size, &bytes_returned, NULL ))
    {
        for (i = 0; i < params->count; i++)
        {
            GUID service_guid;
            if (params->services[i].ServiceUuid.IsShortUuid)
            {
                service_guid = btle_att_base_guid;
                service_guid.Data1 = params->services[i].ServiceUuid.Value.ShortUuid;
            }
            else
            {
                service_guid = params->services[i].ServiceUuid.Value.LongUuid;
            }

            if (IsEqualGUID( &service_guid, &service_uuid ))
            {
                FIXME( "Found matching GATT service, but IGattDeviceService not implemented\n" );
                *service = NULL;
                hr = E_NOTIMPL;
                break;
            }
        }
    }
    else
    {
        DWORD err = GetLastError();
        WARN( "IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES failed: %lu\n", err );
        hr = HRESULT_FROM_WIN32( err );
    }

    if (hr == HRESULT_FROM_WIN32( ERROR_NOT_FOUND ))
        *service = NULL;

    free( params );
    return hr;
}

static HRESULT WINAPI le_device_add_NameChanged( IBluetoothLEDevice *iface, ITypedEventHandler_BluetoothLEDevice_IInspectable *handler, EventRegistrationToken *token )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    struct handler_entry *entry;

    TRACE( "(%p, %p, %p)\n", iface, handler, token );

    if (!handler || !token) return E_POINTER;

    entry = malloc( sizeof(*entry) );
    if (!entry) return E_OUTOFMEMORY;

    entry->handler = handler;
    ITypedEventHandler_BluetoothLEDevice_IInspectable_AddRef( handler );
    entry->token.value = (UINT64)(uintptr_t)entry;
    *token = entry->token;

    list_add_tail( &impl->name_changed_handlers, &entry->entry );
    return S_OK;
}

static HRESULT WINAPI le_device_remove_NameChanged( IBluetoothLEDevice *iface, EventRegistrationToken token )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    struct handler_entry *entry, *next;

    TRACE( "(%p, %I64x)\n", iface, token.value );

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &impl->name_changed_handlers, struct handler_entry, entry )
    {
        if (entry->token.value == token.value)
        {
            list_remove( &entry->entry );
            ITypedEventHandler_BluetoothLEDevice_IInspectable_Release( entry->handler );
            free( entry );
            return S_OK;
        }
    }
    return S_OK;
}

static HRESULT WINAPI le_device_add_GattServicesChanged( IBluetoothLEDevice *iface, ITypedEventHandler_BluetoothLEDevice_IInspectable *handler, EventRegistrationToken *token )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    struct handler_entry *entry;

    TRACE( "(%p, %p, %p)\n", iface, handler, token );

    if (!handler || !token) return E_POINTER;

    entry = malloc( sizeof(*entry) );
    if (!entry) return E_OUTOFMEMORY;

    entry->handler = handler;
    ITypedEventHandler_BluetoothLEDevice_IInspectable_AddRef( handler );
    entry->token.value = (UINT64)(uintptr_t)entry;
    *token = entry->token;

    list_add_tail( &impl->gatt_services_changed_handlers, &entry->entry );
    return S_OK;
}

static HRESULT WINAPI le_device_remove_GattServicesChanged( IBluetoothLEDevice *iface, EventRegistrationToken token )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    struct handler_entry *entry, *next;

    TRACE( "(%p, %I64x)\n", iface, token.value );

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &impl->gatt_services_changed_handlers, struct handler_entry, entry )
    {
        if (entry->token.value == token.value)
        {
            list_remove( &entry->entry );
            ITypedEventHandler_BluetoothLEDevice_IInspectable_Release( entry->handler );
            free( entry );
            return S_OK;
        }
    }
    return S_OK;
}

static DWORD WINAPI connection_status_monitor_thread( void *arg )
{
    struct bluetooth_le_device *impl = arg;
    BluetoothConnectionStatus current_status;
    struct handler_entry *entry;
    DWORD bytes_returned;
    HRESULT hr;
    HANDLE radio_handle = INVALID_HANDLE_VALUE;
    struct winebth_radio_get_device_connection_status_params params;

    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    TRACE( "Connection monitor thread started for device %p address=%I64x\n", impl, impl->address );

    while (impl->connection_monitor_running)
    {
        current_status = BluetoothConnectionStatus_Disconnected;

        if (impl->device_handle != INVALID_HANDLE_VALUE)
        {
            BOOL connected = FALSE;
            if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_CONNECTION_STATUS,
                                NULL, 0, &connected, sizeof(connected), &bytes_returned, NULL ))
            {
                current_status = connected ? BluetoothConnectionStatus_Connected : BluetoothConnectionStatus_Disconnected;
            }
        }
        else
        {
            if (radio_handle == INVALID_HANDLE_VALUE)
                radio_handle = open_first_radio();

            if (radio_handle != INVALID_HANDLE_VALUE)
            {
                params.address = impl->address;
                params.connected = FALSE;
                if (DeviceIoControl( radio_handle, IOCTL_WINEBTH_RADIO_GET_DEVICE_CONNECTION_STATUS,
                                    &params, sizeof(params), &params, sizeof(params), &bytes_returned, NULL ))
                {
                    current_status = params.connected ? BluetoothConnectionStatus_Connected : BluetoothConnectionStatus_Disconnected;
                }
            }
        }

        if (current_status != impl->last_connection_status)
        {
            TRACE( "Connection status CHANGED: %d -> %d, firing %d handlers\n",
                   impl->last_connection_status, current_status,
                   list_count( &impl->connection_status_changed_handlers ) );
            impl->last_connection_status = current_status;

            LIST_FOR_EACH_ENTRY( entry, &impl->connection_status_changed_handlers, struct handler_entry, entry )
            {
                hr = ITypedEventHandler_BluetoothLEDevice_IInspectable_Invoke(
                    entry->handler, &impl->IBluetoothLEDevice_iface, NULL );
                TRACE( "Invoked ConnectionStatusChanged handler %p, hr=%#lx\n", entry->handler, hr );
            }
        }

        Sleep( 100 );
    }

    if (radio_handle != INVALID_HANDLE_VALUE)
        CloseHandle( radio_handle );

    TRACE( "Connection monitor thread exiting for device %p\n", impl );
    CoUninitialize();
    return 0;
}

static HRESULT WINAPI le_device_add_ConnectionStatusChanged( IBluetoothLEDevice *iface, ITypedEventHandler_BluetoothLEDevice_IInspectable *handler, EventRegistrationToken *token )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    struct handler_entry *entry;
    struct winebth_le_device_get_gatt_services_params params;
    DWORD bytes_returned;

    TRACE( "(%p, %p, %p)\n", iface, handler, token );

    if (!handler || !token) return E_POINTER;

    entry = malloc( sizeof(*entry) );
    if (!entry) return E_OUTOFMEMORY;

    entry->handler = handler;
    ITypedEventHandler_BluetoothLEDevice_IInspectable_AddRef( handler );
    entry->token.value = (UINT64)(uintptr_t)entry;
    *token = entry->token;

    list_add_tail( &impl->connection_status_changed_handlers, &entry->entry );

    /* Start monitoring thread if not already running */
    if (!impl->connection_monitor_thread)
    {
        impl->connection_monitor_running = TRUE;
        impl->connection_monitor_thread = CreateThread( NULL, 0, connection_status_monitor_thread, impl, 0, NULL );
        TRACE( "Started connection status monitor thread %p\n", impl->connection_monitor_thread );
    }

    /* Trigger connection if not already connected */
    if (impl->device_handle != INVALID_HANDLE_VALUE)
    {
        memset( &params, 0, sizeof( params ) );
        DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES, NULL, 0,
                        &params, sizeof( params ), &bytes_returned, NULL );
    }

    return S_OK;
}

static HRESULT WINAPI le_device_remove_ConnectionStatusChanged( IBluetoothLEDevice *iface, EventRegistrationToken token )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice( iface );
    struct handler_entry *entry, *next;

    TRACE( "(%p, %I64x)\n", iface, token.value );

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &impl->connection_status_changed_handlers, struct handler_entry, entry )
    {
        if (entry->token.value == token.value)
        {
            list_remove( &entry->entry );
            ITypedEventHandler_BluetoothLEDevice_IInspectable_Release( entry->handler );
            free( entry );
            return S_OK;
        }
    }
    return S_OK;
}

static const IBluetoothLEDeviceVtbl le_device_vtbl =
{
    le_device_QueryInterface,
    le_device_AddRef,
    le_device_Release,
    le_device_GetIids,
    le_device_GetRuntimeClassName,
    le_device_GetTrustLevel,
    le_device_get_DeviceId,
    le_device_get_Name,
    le_device_get_GattServices,
    le_device_get_ConnectionStatus,
    le_device_get_BluetoothAddress,
    (void *)le_device_GetGattService,
    le_device_add_NameChanged,
    le_device_remove_NameChanged,
    le_device_add_GattServicesChanged,
    le_device_remove_GattServicesChanged,
    le_device_add_ConnectionStatusChanged,
    le_device_remove_ConnectionStatusChanged
};

static inline struct bluetooth_le_device *impl_from_IClosable( IClosable *iface )
{
    return CONTAINING_RECORD( iface, struct bluetooth_le_device, IClosable_iface );
}

static HRESULT WINAPI closable_QueryInterface( IClosable *iface, REFIID iid, void **out )
{
    struct bluetooth_le_device *impl = impl_from_IClosable( iface );
    return le_device_QueryInterface( &impl->IBluetoothLEDevice_iface, iid, out );
}

static ULONG WINAPI closable_AddRef( IClosable *iface )
{
    struct bluetooth_le_device *impl = impl_from_IClosable( iface );
    return le_device_AddRef( &impl->IBluetoothLEDevice_iface );
}

static ULONG WINAPI closable_Release( IClosable *iface )
{
    struct bluetooth_le_device *impl = impl_from_IClosable( iface );
    return le_device_Release( &impl->IBluetoothLEDevice_iface );
}

static HRESULT WINAPI closable_GetIids( IClosable *iface, ULONG *iid_count, IID **iids )
{
    struct bluetooth_le_device *impl = impl_from_IClosable( iface );
    return le_device_GetIids( &impl->IBluetoothLEDevice_iface, iid_count, iids );
}

static HRESULT WINAPI closable_GetRuntimeClassName( IClosable *iface, HSTRING *class_name )
{
    struct bluetooth_le_device *impl = impl_from_IClosable( iface );
    return le_device_GetRuntimeClassName( &impl->IBluetoothLEDevice_iface, class_name );
}

static HRESULT WINAPI closable_GetTrustLevel( IClosable *iface, TrustLevel *level )
{
    struct bluetooth_le_device *impl = impl_from_IClosable( iface );
    return le_device_GetTrustLevel( &impl->IBluetoothLEDevice_iface, level );
}

static HRESULT WINAPI closable_Close( IClosable *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IClosableVtbl closable_vtbl =
{
    closable_QueryInterface,
    closable_AddRef,
    closable_Release,
    closable_GetIids,
    closable_GetRuntimeClassName,
    closable_GetTrustLevel,
    closable_Close
};

typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult IGattDeviceServicesResult;
typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResultVtbl IGattDeviceServicesResultVtbl;
typedef enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattCommunicationStatus GattCommunicationStatus;
#define IID_IGattDeviceServicesResult IID___x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult
#define IGattDeviceServicesResult_AddRef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult_AddRef
#define IGattDeviceServicesResult_Release __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServicesResult_Release
#define IGattDeviceService __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService
#define IGattDeviceServiceVtbl __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceServiceVtbl
#define IID_IGattDeviceService IID___x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService
#define IGattDeviceService3 __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3
#define IGattDeviceService3Vtbl __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3Vtbl
#define IID_IGattDeviceService3 IID___x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3
#define IGattDeviceService3_AddRef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_AddRef
#define IGattDeviceService3_Release __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattDeviceService3_Release
#define IGattCharacteristicsResult __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResult
#define IGattCharacteristicsResultVtbl __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResultVtbl
#define IID_IGattCharacteristicsResult IID___x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResult
#define IGattCharacteristicsResult_AddRef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResult_AddRef
#define IGattCharacteristicsResult_Release __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattCharacteristicsResult_Release
#define IGattReadResult __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattReadResult
#define IGattReadResultVtbl __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattReadResultVtbl
#define IID_IGattReadResult IID___x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattReadResult
#define IGattReadResult_AddRef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattReadResult_AddRef
#define IGattReadResult_Release __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattReadResult_Release
#define IGattValueChangedEventArgs __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs
#define IGattValueChangedEventArgsVtbl __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgsVtbl
#define IID_IGattValueChangedEventArgs IID___x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs
#define IGattValueChangedEventArgs_AddRef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs_AddRef
#define IGattValueChangedEventArgs_Release __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs_Release
#define IGattValueChangedEventArgs_get_Timestamp __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs_get_Timestamp
#define IGattValueChangedEventArgs_get_CharacteristicValue __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattValueChangedEventArgs_get_CharacteristicValue
typedef enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattSharingMode GattSharingMode;
#define IGattSession __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSession
#define IAsyncOperation_GattOpenStatus __FIAsyncOperation_1_GattOpenStatus
#define IDeviceAccessInformation __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceAccessInformation
#define IDeviceAccessInformationVtbl __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceAccessInformationVtbl
#define IID_IDeviceAccessInformation IID___x_ABI_CWindows_CDevices_CEnumeration_CIDeviceAccessInformation
#define IDeviceAccessInformation_AddRef __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceAccessInformation_AddRef
#define IDeviceAccessInformation_Release __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceAccessInformation_Release

typedef enum __x_ABI_CWindows_CDevices_CEnumeration_CDeviceAccessStatus DeviceAccessStatus;

static const IAsyncOperation_DeviceAccessStatusVtbl async_device_access_op_vtbl;
static const IAsyncInfoVtbl async_device_access_info_vtbl;
static HRESULT async_device_access_op_create( DeviceAccessStatus status, IAsyncOperation_DeviceAccessStatus **operation );

static CRITICAL_SECTION timestamp_cs;
static CRITICAL_SECTION_DEBUG timestamp_cs_debug =
{
    0, 0, &timestamp_cs,
    { &timestamp_cs_debug.ProcessLocksList, &timestamp_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": timestamp_cs") }
};
static CRITICAL_SECTION timestamp_cs = { &timestamp_cs_debug, -1, 0, 0, 0, 0 };

static BOOL le_uuid_equal( const BTH_LE_UUID *a, const GUID *b )
{
    GUID guid_a;
    bth_le_uuid_to_guid( a, &guid_a );
    return IsEqualGUID( &guid_a, b );
}

struct device_access_info
{
    IDeviceAccessInformation IDeviceAccessInformation_iface;
    LONG ref;
};

static inline struct device_access_info *impl_from_IDeviceAccessInformation( IDeviceAccessInformation *iface )
{
    return CONTAINING_RECORD( iface, struct device_access_info, IDeviceAccessInformation_iface );
}

static HRESULT WINAPI device_access_info_QueryInterface( IDeviceAccessInformation *iface, REFIID iid, void **out )
{
    struct device_access_info *impl = impl_from_IDeviceAccessInformation( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IDeviceAccessInformation ))
    {
        IDeviceAccessInformation_AddRef( (*out = &impl->IDeviceAccessInformation_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI device_access_info_AddRef( IDeviceAccessInformation *iface )
{
    struct device_access_info *impl = impl_from_IDeviceAccessInformation( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI device_access_info_Release( IDeviceAccessInformation *iface )
{
    struct device_access_info *impl = impl_from_IDeviceAccessInformation( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );

    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI device_access_info_GetIids( IDeviceAccessInformation *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI device_access_info_GetRuntimeClassName( IDeviceAccessInformation *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI device_access_info_GetTrustLevel( IDeviceAccessInformation *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI device_access_info_add_AccessChanged( IDeviceAccessInformation *iface,
    __FITypedEventHandler_2_Windows__CDevices__CEnumeration__CDeviceAccessInformation_Windows__CDevices__CEnumeration__CDeviceAccessChangedEventArgs *handler,
    EventRegistrationToken *cookie )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, handler, cookie );
    return S_OK;
}

static HRESULT WINAPI device_access_info_remove_AccessChanged( IDeviceAccessInformation *iface, EventRegistrationToken cookie )
{
    FIXME( "(%p, %I64x): stub!\n", iface, cookie.value );
    return S_OK;
}

static HRESULT WINAPI device_access_info_get_CurrentStatus( IDeviceAccessInformation *iface, __x_ABI_CWindows_CDevices_CEnumeration_CDeviceAccessStatus *status )
{
    TRACE( "(%p, %p)\n", iface, status );
    *status = DeviceAccessStatus_Allowed;
    return S_OK;
}

static const IDeviceAccessInformationVtbl device_access_info_vtbl =
{
    device_access_info_QueryInterface,
    device_access_info_AddRef,
    device_access_info_Release,
    device_access_info_GetIids,
    device_access_info_GetRuntimeClassName,
    device_access_info_GetTrustLevel,
    device_access_info_add_AccessChanged,
    device_access_info_remove_AccessChanged,
    device_access_info_get_CurrentStatus
};

static HRESULT device_access_info_create( IDeviceAccessInformation **out )
{
    struct device_access_info *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IDeviceAccessInformation_iface.lpVtbl = &device_access_info_vtbl;
    impl->ref = 1;

    *out = &impl->IDeviceAccessInformation_iface;
    return S_OK;
}

static HRESULT gatt_read_result_create( GattCommunicationStatus status, IBuffer *value, IGattReadResult **out );
static HRESULT async_gatt_read_op_create( IGattReadResult *result, IAsyncOperation_GattReadResult **out );
static HRESULT async_gatt_comm_status_op_create( GattCommunicationStatus result, IAsyncOperation_GattCommunicationStatus **out );
static HRESULT gatt_char_write_value_impl( IGattCharacteristic *iface, IBuffer *value, GattWriteOption option, IAsyncOperation_GattCommunicationStatus **operation );

struct gatt_device_service
{
    IGattDeviceService IGattDeviceService_iface;
    IGattDeviceService3 IGattDeviceService3_iface;
    IClosable IClosable_iface;
    LONG ref;
    HANDLE device_handle;
    BOOL is_radio_handle;
    BTH_LE_GATT_SERVICE service_info;
    WCHAR *device_id;
    UINT64 device_address;
    IGattCharacteristic **cached_chars;
    UINT32 cached_chars_count;
    CRITICAL_SECTION cache_cs;
};

static inline struct gatt_device_service *impl_from_IGattDeviceService( IGattDeviceService *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_device_service, IGattDeviceService_iface );
}

static HRESULT WINAPI gatt_service_QueryInterface( IGattDeviceService *iface, REFIID iid, void **out )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGattDeviceService ))
    {
        *out = &impl->IGattDeviceService_iface;
        IUnknown_AddRef( (IUnknown *)*out );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IGattDeviceService3 ))
    {
        IGattDeviceService3_AddRef( (*out = &impl->IGattDeviceService3_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        IClosable_AddRef( (*out = &impl->IClosable_iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_service_AddRef( IGattDeviceService *iface )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_service_Release( IGattDeviceService *iface )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        UINT32 i;
        if (impl->device_handle != INVALID_HANDLE_VALUE)
            CloseHandle( impl->device_handle );
        EnterCriticalSection( &impl->cache_cs );
        if (impl->cached_chars)
        {
            for (i = 0; i < impl->cached_chars_count; i++)
            {
                if (impl->cached_chars[i])
                    IUnknown_Release( (IUnknown *)impl->cached_chars[i] );
            }
            free( impl->cached_chars );
        }
        LeaveCriticalSection( &impl->cache_cs );
        DeleteCriticalSection( &impl->cache_cs );
        free( impl->device_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_service_GetIids( IGattDeviceService *iface, ULONG *iid_count, IID **iids )
{
    TRACE( "(%p, %p, %p)\n", iface, iid_count, iids );
    if (!iid_count || !iids) return E_POINTER;
    *iid_count = 0;
    *iids = NULL;
    return S_OK;
}

static HRESULT WINAPI gatt_service_GetRuntimeClassName( IGattDeviceService *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceService";
    if (!class_name) return E_POINTER;
    TRACE("Returning standard RuntimeClassName: %ls\n", class_name_str);
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI gatt_service_GetTrustLevel( IGattDeviceService *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_service_GetCharacteristics( IGattDeviceService *iface, GUID uuid,
                                                        IVectorView_GattCharacteristic **value )
{
    FIXME( "(%p, %s, %p): stub!\n", iface, debugstr_guid( &uuid ), value );
    if (!value) return E_POINTER;
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service_GetIncludedServices( IGattDeviceService *iface, GUID uuid,
                                                         IVectorView_GattDeviceService **value )
{
    FIXME( "(%p, %s, %p): stub!\n", iface, debugstr_guid( &uuid ), value );
    if (!value) return E_POINTER;
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service_get_DeviceId( IGattDeviceService *iface, HSTRING *value )
{
    static const WCHAR short_id[] = L"SVC";
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsCreateString( short_id, 3, value );
}

static HRESULT WINAPI gatt_service_get_Uuid( IGattDeviceService *iface, GUID *value )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    bth_le_uuid_to_guid( &impl->service_info.ServiceUuid, value );
    return S_OK;
}

static HRESULT WINAPI gatt_service_get_AttributeHandle( IGattDeviceService *iface, UINT16 *value )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->service_info.AttributeHandle;
    return S_OK;
}

static const IGattDeviceServiceVtbl gatt_device_service_vtbl =
{
    gatt_service_QueryInterface,
    gatt_service_AddRef,
    gatt_service_Release,
    gatt_service_GetIids,
    gatt_service_GetRuntimeClassName,
    gatt_service_GetTrustLevel,
    gatt_service_GetCharacteristics,
    gatt_service_GetIncludedServices,
    gatt_service_get_DeviceId,
    gatt_service_get_Uuid,
    gatt_service_get_AttributeHandle
};

static inline struct gatt_device_service *impl_from_service_IClosable( IClosable *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_device_service, IClosable_iface );
}

static HRESULT WINAPI service_closable_QueryInterface( IClosable *iface, REFIID iid, void **out )
{
    struct gatt_device_service *impl = impl_from_service_IClosable( iface );
    return gatt_service_QueryInterface( &impl->IGattDeviceService_iface, iid, out );
}

static ULONG WINAPI service_closable_AddRef( IClosable *iface )
{
    struct gatt_device_service *impl = impl_from_service_IClosable( iface );
    return gatt_service_AddRef( &impl->IGattDeviceService_iface );
}

static ULONG WINAPI service_closable_Release( IClosable *iface )
{
    struct gatt_device_service *impl = impl_from_service_IClosable( iface );
    return gatt_service_Release( &impl->IGattDeviceService_iface );
}

static HRESULT WINAPI service_closable_GetIids( IClosable *iface, ULONG *iid_count, IID **iids )
{
    struct gatt_device_service *impl = impl_from_service_IClosable( iface );
    return gatt_service_GetIids( &impl->IGattDeviceService_iface, iid_count, iids );
}

static HRESULT WINAPI service_closable_GetRuntimeClassName( IClosable *iface, HSTRING *class_name )
{
    struct gatt_device_service *impl = impl_from_service_IClosable( iface );
    return gatt_service_GetRuntimeClassName( &impl->IGattDeviceService_iface, class_name );
}

static HRESULT WINAPI service_closable_GetTrustLevel( IClosable *iface, TrustLevel *level )
{
    struct gatt_device_service *impl = impl_from_service_IClosable( iface );
    return gatt_service_GetTrustLevel( &impl->IGattDeviceService_iface, level );
}

static HRESULT WINAPI service_closable_Close( IClosable *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IClosableVtbl service_closable_vtbl =
{
    service_closable_QueryInterface,
    service_closable_AddRef,
    service_closable_Release,
    service_closable_GetIids,
    service_closable_GetRuntimeClassName,
    service_closable_GetTrustLevel,
    service_closable_Close
};

/* IGattCharacteristic implementation */
struct gatt_characteristic
{
    IGattCharacteristic IGattCharacteristic_iface;
    LONG ref;
    HANDLE device_handle;
    BOOL is_radio_handle;
    UINT64 device_address;
    BTH_LE_GATT_SERVICE service_info;
    BTH_LE_GATT_CHARACTERISTIC char_info;
    ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs *value_changed_handler;
    EventRegistrationToken value_changed_token;
    LONG next_token;
    HANDLE notification_thread;
    volatile BOOL notification_thread_stop;
    CRITICAL_SECTION handler_cs;
};

static inline struct gatt_characteristic *impl_from_IGattCharacteristic( IGattCharacteristic *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_characteristic, IGattCharacteristic_iface );
}

static HRESULT WINAPI gatt_char_QueryInterface( IGattCharacteristic *iface, REFIID iid, void **out )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGattCharacteristic ))
    {
        IUnknown_AddRef( (IUnknown *)iface );
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_char_AddRef( IGattCharacteristic *iface )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static void gatt_char_free( struct gatt_characteristic *impl )
{
    if (impl->device_handle != INVALID_HANDLE_VALUE) CloseHandle( impl->device_handle );
    if (impl->value_changed_handler) ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( impl->value_changed_handler );
    DeleteCriticalSection( &impl->handler_cs );
    free( impl );
}

static ULONG WINAPI gatt_char_Release( IGattCharacteristic *iface )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );

    TRACE( "gatt_char_Release: %p uuid=%s ref=%lu\n", iface, debugstr_guid( &impl->char_info.CharacteristicUuid.Value.LongUuid ), ref );

    if (!ref)
    {
        if (impl->notification_thread)
        {
            impl->notification_thread_stop = TRUE;
            /* We don't Wait here to avoid deadlock. The thread holds a ref. */
            CancelIoEx( impl->device_handle, NULL );
        }
        gatt_char_free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_char_GetIids( IGattCharacteristic *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_char_GetRuntimeClassName( IGattCharacteristic *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristic";
    TRACE( "(%p, %p)\n", iface, class_name );
    if (!class_name) return E_POINTER;
    return WindowsCreateString( name, ARRAY_SIZE( name ) - 1, class_name );
}

static HRESULT WINAPI gatt_char_GetTrustLevel( IGattCharacteristic *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_char_GetDescriptors( IGattCharacteristic *iface, GUID uuid, IVectorView_GattDescriptor **value )
{
    FIXME( "(%p, %s, %p): stub!\n", iface, debugstr_guid( &uuid ), value );
    if (!value) return E_POINTER;
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_char_get_CharacteristicProperties( IGattCharacteristic *iface, GattCharacteristicProperties *value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->char_info.CharacteristicValueHandle ? impl->char_info.IsBroadcastable : 0;
    if (impl->char_info.IsReadable) *value |= GattCharacteristicProperties_Read;
    if (impl->char_info.IsWritable) *value |= GattCharacteristicProperties_Write;
    if (impl->char_info.IsWritableWithoutResponse) *value |= GattCharacteristicProperties_WriteWithoutResponse;
    if (impl->char_info.IsNotifiable) *value |= GattCharacteristicProperties_Notify;
    if (impl->char_info.IsIndicatable) *value |= GattCharacteristicProperties_Indicate;
    return S_OK;
}

static HRESULT WINAPI gatt_char_get_ProtectionLevel( IGattCharacteristic *iface, GattProtectionLevel *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = GattProtectionLevel_Plain;
    return S_OK;
}

static HRESULT WINAPI gatt_char_put_ProtectionLevel( IGattCharacteristic *iface, GattProtectionLevel value )
{
    FIXME( "(%p, %d): stub!\n", iface, value );
    return S_OK;
}

static HRESULT WINAPI gatt_char_get_UserDescription( IGattCharacteristic *iface, HSTRING *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsCreateString( L"", 0, value );
}

static HRESULT WINAPI gatt_char_get_Uuid( IGattCharacteristic *iface, GUID *value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    bth_le_uuid_to_guid( &impl->char_info.CharacteristicUuid, value );
    return S_OK;
}

static HRESULT WINAPI gatt_char_get_AttributeHandle( IGattCharacteristic *iface, UINT16 *value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->char_info.AttributeHandle;
    return S_OK;
}

typedef __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattPresentationFormat IGattPresentationFormat;

struct gatt_presentation_formats_vector
{
    IVectorView_GattPresentationFormat IVectorView_GattPresentationFormat_iface;
    IIterable_GattPresentationFormat IIterable_GattPresentationFormat_iface;
    LONG ref;
};

static inline struct gatt_presentation_formats_vector *impl_from_IVectorView_GattPresentationFormat( IVectorView_GattPresentationFormat *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_presentation_formats_vector, IVectorView_GattPresentationFormat_iface );
}

static inline struct gatt_presentation_formats_vector *impl_from_IIterable_GattPresentationFormat( IIterable_GattPresentationFormat *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_presentation_formats_vector, IIterable_GattPresentationFormat_iface );
}

static HRESULT WINAPI gatt_presentation_formats_iterable_QueryInterface( IIterable_GattPresentationFormat *iface, REFIID iid, void **out )
{
    struct gatt_presentation_formats_vector *impl = impl_from_IIterable_GattPresentationFormat( iface );
    return IInspectable_QueryInterface( (IInspectable *)&impl->IVectorView_GattPresentationFormat_iface, iid, out );
}

static ULONG WINAPI gatt_presentation_formats_iterable_AddRef( IIterable_GattPresentationFormat *iface )
{
    struct gatt_presentation_formats_vector *impl = impl_from_IIterable_GattPresentationFormat( iface );
    return IVectorView_GattPresentationFormat_AddRef( &impl->IVectorView_GattPresentationFormat_iface );
}

static ULONG WINAPI gatt_presentation_formats_iterable_Release( IIterable_GattPresentationFormat *iface )
{
    struct gatt_presentation_formats_vector *impl = impl_from_IIterable_GattPresentationFormat( iface );
    return IVectorView_GattPresentationFormat_Release( &impl->IVectorView_GattPresentationFormat_iface );
}

static HRESULT WINAPI gatt_presentation_formats_iterable_GetIids( IIterable_GattPresentationFormat *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_presentation_formats_iterable_GetRuntimeClassName( IIterable_GattPresentationFormat *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_presentation_formats_iterable_GetTrustLevel( IIterable_GattPresentationFormat *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_presentation_formats_iterable_First( IIterable_GattPresentationFormat *iface, IIterator_GattPresentationFormat **value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    /* return E_NOTIMPL for now */
    return E_NOTIMPL;
}

static const IIterable_GattPresentationFormatVtbl gatt_presentation_formats_iterable_vtbl =
{
    gatt_presentation_formats_iterable_QueryInterface,
    gatt_presentation_formats_iterable_AddRef,
    gatt_presentation_formats_iterable_Release,
    gatt_presentation_formats_iterable_GetIids,
    gatt_presentation_formats_iterable_GetRuntimeClassName,
    gatt_presentation_formats_iterable_GetTrustLevel,
    gatt_presentation_formats_iterable_First,
};

static HRESULT WINAPI gatt_presentation_formats_vector_QueryInterface( IVectorView_GattPresentationFormat *iface, REFIID iid, void **out )
{
    struct gatt_presentation_formats_vector *impl = impl_from_IVectorView_GattPresentationFormat( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IVectorView_GattPresentationFormat ))
    {
        *out = &impl->IVectorView_GattPresentationFormat_iface;
        IUnknown_AddRef( (IUnknown*)*out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IIterable_GattPresentationFormat ))
    {
        *out = &impl->IIterable_GattPresentationFormat_iface;
        IUnknown_AddRef( (IUnknown*)*out );
        return S_OK;
    }

    FIXME( "(%p, %s, %p): interface not implemented\n", iface, debugstr_guid(iid), out );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_presentation_formats_vector_AddRef( IVectorView_GattPresentationFormat *iface )
{
    struct gatt_presentation_formats_vector *impl = impl_from_IVectorView_GattPresentationFormat( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_presentation_formats_vector_Release( IVectorView_GattPresentationFormat *iface )
{
    struct gatt_presentation_formats_vector *impl = impl_from_IVectorView_GattPresentationFormat( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_presentation_formats_vector_GetIids( IVectorView_GattPresentationFormat *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_presentation_formats_vector_GetRuntimeClassName( IVectorView_GattPresentationFormat *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_presentation_formats_vector_GetTrustLevel( IVectorView_GattPresentationFormat *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_presentation_formats_vector_GetAt( IVectorView_GattPresentationFormat *iface, UINT32 index, IGattPresentationFormat **value )
{
    TRACE( "(%p, %u, %p)\n", iface, index, value );
    /* Empty vector, always out of bounds */
    return E_BOUNDS;
}

static HRESULT WINAPI gatt_presentation_formats_vector_get_Size( IVectorView_GattPresentationFormat *iface, UINT32 *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static HRESULT WINAPI gatt_presentation_formats_vector_IndexOf( IVectorView_GattPresentationFormat *iface, IGattPresentationFormat *element,
                                                    UINT32 *index, BOOLEAN *found )
{
    TRACE( "(%p, %p, %p, %p)\n", iface, element, index, found );
    if (!index || !found) return E_POINTER;
    *found = FALSE;
    *index = 0;
    return S_OK;
}

static HRESULT WINAPI gatt_presentation_formats_vector_GetMany( IVectorView_GattPresentationFormat *iface, UINT32 start_index,
                                                    UINT32 items_size, IGattPresentationFormat **items, UINT32 *value )
{
    TRACE( "(%p, %u, %u, %p, %p)\n", iface, start_index, items_size, items, value );
    if (!value) return E_POINTER;
    *value = 0;
    return S_OK;
}

static const struct IVectorView_GattPresentationFormatVtbl gatt_presentation_formats_vector_vtbl =
{
    gatt_presentation_formats_vector_QueryInterface,
    gatt_presentation_formats_vector_AddRef,
    gatt_presentation_formats_vector_Release,
    gatt_presentation_formats_vector_GetIids,
    gatt_presentation_formats_vector_GetRuntimeClassName,
    gatt_presentation_formats_vector_GetTrustLevel,
    gatt_presentation_formats_vector_GetAt,
    gatt_presentation_formats_vector_get_Size,
    gatt_presentation_formats_vector_IndexOf,
    gatt_presentation_formats_vector_GetMany
};

static HRESULT WINAPI gatt_char_get_PresentationFormats( IGattCharacteristic *iface, IVectorView_GattPresentationFormat **value )
{
    struct gatt_presentation_formats_vector *impl;

    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;

    if (!(impl = calloc( 1, sizeof( *impl ) )))
    {
        return E_OUTOFMEMORY;
    }

    impl->IVectorView_GattPresentationFormat_iface.lpVtbl = &gatt_presentation_formats_vector_vtbl;
    impl->IIterable_GattPresentationFormat_iface.lpVtbl = &gatt_presentation_formats_iterable_vtbl;
    impl->ref = 1;

    *value = &impl->IVectorView_GattPresentationFormat_iface;
    TRACE( "Returning empty PresentationFormats vector %p\n", *value );
    return S_OK;
}

static HRESULT gatt_char_read_value_impl( IGattCharacteristic *iface, IAsyncOperation_GattReadResult **value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    struct winebth_le_device_read_characteristic_params *device_params = NULL;
    struct winebth_radio_read_characteristic_params *radio_params = NULL;
    GattCommunicationStatus status;
    IGattReadResult *result;
    IBuffer *buffer = NULL;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    HRESULT hr;
    BOOL ioctl_success;
    ULONG data_size;
    UCHAR *data_ptr;

    TRACE( "(%p, %p)\n", iface, value );

    if (!value) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        status = GattCommunicationStatus_Unreachable;
        hr = gatt_read_result_create( status, NULL, &result );
        if (FAILED( hr )) return hr;
        hr = async_gatt_read_op_create( result, value );
        IGattReadResult_Release( result );
        return hr;
    }

    if (impl->is_radio_handle)
    {
        buffer_size = offsetof( struct winebth_radio_read_characteristic_params, data[512] );
        radio_params = malloc( buffer_size );
        if (!radio_params) return E_OUTOFMEMORY;

        memset( radio_params, 0, buffer_size );
        radio_params->address = impl->device_address;
        radio_params->service = impl->service_info;
        radio_params->characteristic = impl->char_info;
        radio_params->data_size = 0;

        ioctl_success = DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_RADIO_READ_CHARACTERISTIC,
                                          radio_params, buffer_size, radio_params, buffer_size, &bytes_returned, NULL );
        data_size = radio_params->data_size;
        data_ptr = radio_params->data;
    }
    else
    {
        buffer_size = offsetof( struct winebth_le_device_read_characteristic_params, data[512] );
        device_params = malloc( buffer_size );
        if (!device_params) return E_OUTOFMEMORY;

        memset( device_params, 0, buffer_size );
        device_params->service = impl->service_info;
        device_params->characteristic = impl->char_info;
        device_params->data_size = 0;

        ioctl_success = DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_READ_CHARACTERISTIC,
                                          device_params, buffer_size, device_params, buffer_size, &bytes_returned, NULL );
        data_size = device_params->data_size;
        data_ptr = device_params->data;
    }

    if (ioctl_success)
    {
        status = GattCommunicationStatus_Success;
        if (data_size > 0)
        {
            hr = gatt_buffer_create( data_ptr, data_size, &buffer );
            if (FAILED( hr ))
            {
                free( device_params );
                free( radio_params );
                return hr;
            }
        }
    }
    else
    {
        status = GattCommunicationStatus_Unreachable;
    }

    free( device_params );
    free( radio_params );

    hr = gatt_read_result_create( status, buffer, &result );
    if (buffer) IBuffer_Release( buffer );
    if (FAILED( hr )) return hr;

    hr = async_gatt_read_op_create( result, value );
    IGattReadResult_Release( result );
    return hr;
}

static HRESULT WINAPI gatt_char_ReadValueAsync( IGattCharacteristic *iface, IAsyncOperation_GattReadResult **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    return gatt_char_read_value_impl( iface, value );
}

static HRESULT WINAPI gatt_char_ReadValueWithCacheModeAsync( IGattCharacteristic *iface, BluetoothCacheMode mode,
                                                              IAsyncOperation_GattReadResult **value )
{
    TRACE( "(%p, %d, %p)\n", iface, mode, value );
    return gatt_char_read_value_impl( iface, value );
}

static HRESULT gatt_char_write_value_impl( IGattCharacteristic *iface, IBuffer *value, GattWriteOption opt,
                                            IAsyncOperation_GattCommunicationStatus **async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    struct winebth_le_device_write_characteristic_params *device_params = NULL;
    struct winebth_radio_write_characteristic_params *radio_params = NULL;
    IBufferByteAccess *byte_access = NULL;
    GattCommunicationStatus status;
    BOOL ioctl_success;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    UINT32 data_len;
    BYTE *data_ptr;
    HRESULT hr;

    TRACE( "(%p, %p, %d, %p)\n", iface, value, opt, async );

    if (!async) return E_POINTER;
    if (impl->device_handle == INVALID_HANDLE_VALUE)
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Unreachable, async );
    if (!value)
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Success, async );

    hr = IBuffer_QueryInterface( value, &IID_IBufferByteAccess, (void **)&byte_access );
    if (FAILED( hr ))
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Unreachable, async );

    hr = IBuffer_get_Length( value, &data_len );
    if (FAILED( hr ))
    {
        IBufferByteAccess_Release( byte_access );
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Unreachable, async );
    }

    hr = IBufferByteAccess_Buffer( byte_access, &data_ptr );
    if (FAILED( hr ))
    {
        IBufferByteAccess_Release( byte_access );
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Unreachable, async );
    }

    IBufferByteAccess_Release( byte_access );

    if (impl->is_radio_handle)
    {
        buffer_size = offsetof( struct winebth_radio_write_characteristic_params, data[data_len] );
        radio_params = malloc( buffer_size );
        if (!radio_params) return E_OUTOFMEMORY;

        memset( radio_params, 0, buffer_size );
        radio_params->address = impl->device_address;
        radio_params->service = impl->service_info;
        radio_params->characteristic = impl->char_info;
        radio_params->write_type = (opt == GattWriteOption_WriteWithoutResponse) ? 1 : 0;
        radio_params->data_size = data_len;
        if (data_len > 0)
            memcpy( radio_params->data, data_ptr, data_len );

        ioctl_success = DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_RADIO_WRITE_CHARACTERISTIC,
                                          radio_params, buffer_size, NULL, 0, &bytes_returned, NULL );
        free( radio_params );
    }
    else
    {
        buffer_size = offsetof( struct winebth_le_device_write_characteristic_params, data[data_len] );
        device_params = malloc( buffer_size );
        if (!device_params) return E_OUTOFMEMORY;

        memset( device_params, 0, buffer_size );
        device_params->service = impl->service_info;
        device_params->characteristic = impl->char_info;
        device_params->write_type = (opt == GattWriteOption_WriteWithoutResponse) ? 1 : 0;
        device_params->data_size = data_len;
        if (data_len > 0)
            memcpy( device_params->data, data_ptr, data_len );

        ioctl_success = DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_WRITE_CHARACTERISTIC,
                                          device_params, buffer_size, NULL, 0, &bytes_returned, NULL );
        free( device_params );
    }

    status = ioctl_success ? GattCommunicationStatus_Success : GattCommunicationStatus_Unreachable;
    return async_gatt_comm_status_op_create( status, async );
}

static HRESULT WINAPI gatt_char_WriteValueAsync( IGattCharacteristic *iface, IBuffer *value,
                                                  IAsyncOperation_GattCommunicationStatus **async )
{
    TRACE( "(%p, %p, %p)\n", iface, value, async );
    return gatt_char_write_value_impl( iface, value, GattWriteOption_WriteWithResponse, async );
}

static HRESULT WINAPI gatt_char_WriteValueWithOptionAsync( IGattCharacteristic *iface, IBuffer *value,
                                                            GattWriteOption opt,
                                                            IAsyncOperation_GattCommunicationStatus **async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );

    TRACE( "(%p, %p, %d, %p)\n", iface, value, opt, async );
    return gatt_char_write_value_impl( iface, value, opt, async );
}

static HRESULT WINAPI gatt_char_ReadClientCharacteristicConfigurationDescriptorAsync( IGattCharacteristic *iface,
                                                                                        IAsyncOperation_GattReadClientCharacteristicConfigurationDescriptorResult **async )
{
    FIXME( "(%p, %p): stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_char_WriteClientCharacteristicConfigurationDescriptorAsync( IGattCharacteristic *iface,
                                                                                         GattClientCharacteristicConfigurationDescriptorValue value,
                                                                                         IAsyncOperation_GattCommunicationStatus **async )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    struct winebth_le_device_set_notify_params device_params;
    struct winebth_radio_set_notify_params radio_params;
    GattCommunicationStatus status;
    DWORD bytes_returned;
    BOOL ioctl_success;
    BOOL enable;

    TRACE( "(%p, %d, %p)\n", iface, value, async );

    if (!async) return E_POINTER;
    if (impl->device_handle == INVALID_HANDLE_VALUE)
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Unreachable, async );

    enable = (value == GattClientCharacteristicConfigurationDescriptorValue_Notify ||
              value == GattClientCharacteristicConfigurationDescriptorValue_Indicate);

    if (impl->is_radio_handle)
    {
        memset( &radio_params, 0, sizeof( radio_params ) );
        radio_params.address = impl->device_address;
        radio_params.service = impl->service_info;
        radio_params.characteristic = impl->char_info;
        radio_params.enable = enable;

        ioctl_success = DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_RADIO_SET_NOTIFY,
                                          &radio_params, sizeof( radio_params ), NULL, 0, &bytes_returned, NULL );
    }
    else
    {
        memset( &device_params, 0, sizeof( device_params ) );
        device_params.service = impl->service_info;
        device_params.characteristic = impl->char_info;
        device_params.enable = enable;

        ioctl_success = DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_SET_NOTIFY,
                                          &device_params, sizeof( device_params ), NULL, 0, &bytes_returned, NULL );
    }

    status = ioctl_success ? GattCommunicationStatus_Success : GattCommunicationStatus_Unreachable;
    return async_gatt_comm_status_op_create( status, async );
}

static HRESULT gatt_value_changed_event_args_create( IBuffer *value, IGattValueChangedEventArgs **out );

static HRESULT WINAPI gatt_value_changed_args_get_Timestamp( IGattValueChangedEventArgs *iface, DateTime *value );

static DWORD WINAPI gatt_char_notification_thread( void *arg )
{
    struct gatt_characteristic *impl = arg;
    unsigned char buffer[256];
    unsigned int data_size;
    struct winebth_le_device_read_notification_params *device_params = NULL;
    struct winebth_radio_read_notification_params *radio_params = NULL;
    void *params;
    ULONG params_size;
    ULONG bytes_returned;
    HRESULT hr;
    HANDLE device_handle;
    BOOL is_radio_handle;
    UINT64 device_address;
    BTH_LE_GATT_SERVICE service_info;
    BTH_LE_GATT_CHARACTERISTIC char_info;
    DWORD ioctl_code;

    CoInitializeEx( NULL, COINIT_MULTITHREADED );

    device_handle = impl->device_handle;
    is_radio_handle = impl->is_radio_handle;
    device_address = impl->device_address;
    service_info = impl->service_info;
    char_info = impl->char_info;

    if (is_radio_handle)
    {
        params_size = sizeof(*radio_params) + sizeof(buffer);
        radio_params = malloc( params_size );
        if (!radio_params)
        {
            impl->IGattCharacteristic_iface.lpVtbl->Release( &impl->IGattCharacteristic_iface );
            return 1;
        }
        radio_params->address = device_address;
        radio_params->service = service_info;
        radio_params->characteristic = char_info;
        params = radio_params;
        ioctl_code = IOCTL_WINEBTH_RADIO_READ_NOTIFICATION;
    }
    else
    {
        params_size = sizeof(*device_params) + sizeof(buffer);
        device_params = malloc( params_size );
        if (!device_params)
        {
            impl->IGattCharacteristic_iface.lpVtbl->Release( &impl->IGattCharacteristic_iface );
            return 1;
        }
        device_params->service = service_info;
        device_params->characteristic = char_info;
        params = device_params;
        ioctl_code = IOCTL_WINEBTH_LE_DEVICE_READ_NOTIFICATION;
    }

    while (1)
    {
        BOOL should_stop;
        ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs *handler;

        EnterCriticalSection( &impl->handler_cs );
        should_stop = impl->notification_thread_stop;
        handler = impl->value_changed_handler;
        if (handler) ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_AddRef( handler );
        LeaveCriticalSection( &impl->handler_cs );

        if (should_stop) break;

        bytes_returned = 0;

        if (DeviceIoControl( device_handle, ioctl_code,
                            params, params_size,
                            params, params_size,
                            &bytes_returned, NULL ))
        {
            ULONG header_size;
            UCHAR *notification_data;

            if (is_radio_handle)
            {
                header_size = offsetof( struct winebth_radio_read_notification_params, data );
                notification_data = radio_params->data;
            }
            else
            {
                header_size = offsetof( struct winebth_le_device_read_notification_params, data );
                notification_data = device_params->data;
            }

            if (bytes_returned <= header_size)
            {
                Sleep( 50 );
                continue;
            }

            if (bytes_returned > header_size)
            {
                data_size = bytes_returned - header_size;

                if (data_size > 0 && handler)
                {
                    struct gatt_buffer *gatt_buf;
                    IGattValueChangedEventArgs *event_args;
                    IBuffer *ibuf;
                    SIZE_T alloc_size = offsetof( struct gatt_buffer, data[data_size] );

                    gatt_buf = calloc( 1, alloc_size );
                    if (gatt_buf)
                    {
                        gatt_buf->IBuffer_iface.lpVtbl = &gatt_buffer_vtbl;
                        gatt_buf->IBufferByteAccess_iface.lpVtbl = &gatt_buffer_byte_access_vtbl;
                        gatt_buf->ref = 1;
                        gatt_buf->capacity = data_size;
                        gatt_buf->length = data_size;
                        memcpy( gatt_buf->data, notification_data, data_size );
                        
                        hr = gatt_value_changed_event_args_create( &gatt_buf->IBuffer_iface, &event_args );

                        if (SUCCEEDED(hr))
                        {
                            EnterCriticalSection( &impl->handler_cs );
                            handler = impl->value_changed_handler;
                            if (handler)
                            {
                                ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_AddRef( handler );
                                ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Invoke( handler, &impl->IGattCharacteristic_iface, event_args );
                                ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( handler );
                            }
                            LeaveCriticalSection( &impl->handler_cs );
                            IGattValueChangedEventArgs_Release( event_args );
                        }
                        IBuffer_Release( &gatt_buf->IBuffer_iface );
                    }
                }
            }
        }
        else
        {
            DWORD err = GetLastError();

            if (err == ERROR_NO_DATA || err == 0xC00000BB)
            {
                
                if (handler) ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( handler );
                handler = NULL;
                Sleep( 50 );
                EnterCriticalSection( &impl->handler_cs );
                should_stop = impl->notification_thread_stop;
                LeaveCriticalSection( &impl->handler_cs );
                if (should_stop) break;
                continue;
            }
            else
            {
                if (handler) ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( handler );
                handler = NULL;
                Sleep( 100 );
                EnterCriticalSection( &impl->handler_cs );
                should_stop = impl->notification_thread_stop;
                LeaveCriticalSection( &impl->handler_cs );
                if (should_stop) break;
                continue;
            }
        }
        
        if (handler) ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( handler );
    }

    free( params );
    impl->IGattCharacteristic_iface.lpVtbl->Release( &impl->IGattCharacteristic_iface );
    CoUninitialize();
    return 0;
}

static HRESULT WINAPI gatt_char_add_ValueChanged( IGattCharacteristic *iface, ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs *handler,
                                                   EventRegistrationToken *token )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    DWORD thread_id;

    TRACE( "gatt_char_add_ValueChanged: iface=%p handler=%p token=%p\n", iface, handler, token );
    if (!token) return E_POINTER;
    if (!handler) return E_INVALIDARG;

    EnterCriticalSection( &impl->handler_cs );
    if (impl->value_changed_handler) ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( impl->value_changed_handler );
    impl->value_changed_handler = handler;
    ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_AddRef( handler );
    token->value = ++impl->next_token;
    impl->value_changed_token = *token;
    LeaveCriticalSection( &impl->handler_cs );

    if (!impl->notification_thread)
    {
        IUnknown_AddRef( iface );
        impl->notification_thread_stop = FALSE;
        impl->notification_thread = CreateThread( NULL, 0, gatt_char_notification_thread, impl, 0, &thread_id );
        if (!impl->notification_thread)
        {
            IUnknown_Release( iface );
            return HRESULT_FROM_WIN32( GetLastError() );
        }
    }

    return S_OK;
}

static HRESULT WINAPI gatt_char_remove_ValueChanged( IGattCharacteristic *iface, EventRegistrationToken token )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    HANDLE thread_to_wait = NULL;

    TRACE( "(%p, %I64d)\n", iface, token.value );

    EnterCriticalSection( &impl->handler_cs );
    if (impl->value_changed_handler && impl->value_changed_token.value == token.value)
    {
        ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( impl->value_changed_handler );
        impl->value_changed_handler = NULL;
        impl->value_changed_token.value = 0;
        if (impl->notification_thread)
        {
            impl->notification_thread_stop = TRUE;
            CancelIoEx( impl->device_handle, NULL );
            thread_to_wait = impl->notification_thread;
            impl->notification_thread = NULL;
        }
    }
    LeaveCriticalSection( &impl->handler_cs );

    /* Wait outside critical section to avoid deadlock */
    if (thread_to_wait)
    {
        WaitForSingleObject( thread_to_wait, INFINITE );
        CloseHandle( thread_to_wait );
    }
    return S_OK;
}


static const IGattCharacteristicVtbl gatt_characteristic_vtbl =
{
    gatt_char_QueryInterface,
    gatt_char_AddRef,
    gatt_char_Release,
    gatt_char_GetIids,
    gatt_char_GetRuntimeClassName,
    gatt_char_GetTrustLevel,
    gatt_char_GetDescriptors,
    gatt_char_get_CharacteristicProperties,
    gatt_char_get_ProtectionLevel,
    gatt_char_put_ProtectionLevel,
    gatt_char_get_UserDescription,
    gatt_char_get_Uuid,
    gatt_char_get_AttributeHandle,
    gatt_char_get_PresentationFormats,
    gatt_char_ReadValueAsync,
    gatt_char_ReadValueWithCacheModeAsync,
    gatt_char_WriteValueAsync,
    gatt_char_WriteValueWithOptionAsync,
    gatt_char_ReadClientCharacteristicConfigurationDescriptorAsync,
    gatt_char_WriteClientCharacteristicConfigurationDescriptorAsync,
    gatt_char_add_ValueChanged,
    gatt_char_remove_ValueChanged
};

static HRESULT gatt_characteristic_create( HANDLE device_handle, BOOL is_radio_handle, UINT64 device_address,
                                           const BTH_LE_GATT_SERVICE *service,
                                           const BTH_LE_GATT_CHARACTERISTIC *char_info, IGattCharacteristic **out )
{
    struct gatt_characteristic *impl;
    HANDLE dup_handle = INVALID_HANDLE_VALUE;

    if (device_handle != INVALID_HANDLE_VALUE)
    {
        if (!DuplicateHandle( GetCurrentProcess(), device_handle,
                              GetCurrentProcess(), &dup_handle,
                              0, FALSE, DUPLICATE_SAME_ACCESS ))
        {
            WARN( "Failed to duplicate device handle: %lu\n", GetLastError() );
            return HRESULT_FROM_WIN32( GetLastError() );
        }
    }

    if (!(impl = calloc( 1, sizeof( *impl ) )))
    {
        if (dup_handle != INVALID_HANDLE_VALUE) CloseHandle( dup_handle );
        return E_OUTOFMEMORY;
    }
    impl->IGattCharacteristic_iface.lpVtbl = &gatt_characteristic_vtbl;
    impl->ref = 1;
    impl->device_handle = dup_handle;
    impl->is_radio_handle = is_radio_handle;
    impl->device_address = device_address;
    impl->service_info = *service;
    impl->char_info = *char_info;
    InitializeCriticalSection( &impl->handler_cs );
    *out = &impl->IGattCharacteristic_iface;
    return S_OK;
}


/* IIterator_GattCharacteristic implementation */
struct gatt_chars_iterator
{
    IIterator_GattCharacteristic IIterator_GattCharacteristic_iface;
    LONG ref;
    IVectorView_GattCharacteristic *view;
    UINT32 index;
    UINT32 size;
};

static inline struct gatt_chars_iterator *impl_from_IIterator_GattCharacteristic( IIterator_GattCharacteristic *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_chars_iterator, IIterator_GattCharacteristic_iface );
}

static HRESULT WINAPI gatt_chars_iterator_QueryInterface( IIterator_GattCharacteristic *iface, REFIID iid, void **out )
{
    struct gatt_chars_iterator *impl = impl_from_IIterator_GattCharacteristic( iface );
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IIterator_GattCharacteristic ))
    {
        IIterator_GattCharacteristic_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_chars_iterator_AddRef( IIterator_GattCharacteristic *iface )
{
    struct gatt_chars_iterator *impl = impl_from_IIterator_GattCharacteristic( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_chars_iterator_Release( IIterator_GattCharacteristic *iface )
{
    struct gatt_chars_iterator *impl = impl_from_IIterator_GattCharacteristic( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        IVectorView_GattCharacteristic_Release( impl->view );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_chars_iterator_GetIids( IIterator_GattCharacteristic *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_chars_iterator_GetRuntimeClassName( IIterator_GattCharacteristic *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_chars_iterator_GetTrustLevel( IIterator_GattCharacteristic *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_chars_iterator_get_Current( IIterator_GattCharacteristic *iface, IGattCharacteristic **value )
{
    struct gatt_chars_iterator *impl = impl_from_IIterator_GattCharacteristic( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return IVectorView_GattCharacteristic_GetAt( impl->view, impl->index, value );
}

static HRESULT WINAPI gatt_chars_iterator_get_HasCurrent( IIterator_GattCharacteristic *iface, boolean *value )
{
    struct gatt_chars_iterator *impl = impl_from_IIterator_GattCharacteristic( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->index < impl->size;
    return S_OK;
}

static HRESULT WINAPI gatt_chars_iterator_MoveNext( IIterator_GattCharacteristic *iface, boolean *value )
{
    struct gatt_chars_iterator *impl = impl_from_IIterator_GattCharacteristic( iface );
    TRACE( "(%p, %p) index=%u size=%u\n", iface, value, impl->index, impl->size );
    if (!value) return E_POINTER;
    if (impl->index < impl->size) impl->index++;
    return gatt_chars_iterator_get_HasCurrent( iface, value );
}

static HRESULT WINAPI gatt_chars_iterator_GetMany( IIterator_GattCharacteristic *iface, UINT32 items_size,
                                                  IGattCharacteristic **items, UINT32 *count )
{
    struct gatt_chars_iterator *impl = impl_from_IIterator_GattCharacteristic( iface );
    TRACE( "(%p, %u, %p, %p)\n", iface, items_size, items, count );
    return IVectorView_GattCharacteristic_GetMany( impl->view, impl->index, items_size, items, count );
}

static const IIterator_GattCharacteristicVtbl gatt_chars_iterator_vtbl =
{
    gatt_chars_iterator_QueryInterface,
    gatt_chars_iterator_AddRef,
    gatt_chars_iterator_Release,
    gatt_chars_iterator_GetIids,
    gatt_chars_iterator_GetRuntimeClassName,
    gatt_chars_iterator_GetTrustLevel,
    gatt_chars_iterator_get_Current,
    gatt_chars_iterator_get_HasCurrent,
    gatt_chars_iterator_MoveNext,
    gatt_chars_iterator_GetMany,
};

/* IVectorView_GattCharacteristic implementation */
struct gatt_characteristics_vector
{
    IVectorView_GattCharacteristic IVectorView_GattCharacteristic_iface;
    IIterable_GattCharacteristic IIterable_GattCharacteristic_iface;
    LONG ref;
    ULONG count;
    BTH_LE_GATT_CHARACTERISTIC *characteristics;
    IGattCharacteristic **cached_characteristics; /* Cached objects for GetAt */
    BTH_LE_GATT_SERVICE service_info;
    HANDLE device_handle;
    BOOL is_radio_handle;
    UINT64 device_address;
};


static inline struct gatt_characteristics_vector *impl_from_IVectorView_GattCharacteristic( IVectorView_GattCharacteristic *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_characteristics_vector, IVectorView_GattCharacteristic_iface );
}

static inline struct gatt_characteristics_vector *impl_from_IIterable_GattCharacteristic( IIterable_GattCharacteristic *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_characteristics_vector, IIterable_GattCharacteristic_iface );
}

static HRESULT WINAPI gatt_chars_iterable_QueryInterface( IIterable_GattCharacteristic *iface, REFIID iid, void **out )
{
    struct gatt_characteristics_vector *impl = impl_from_IIterable_GattCharacteristic( iface );
    return IInspectable_QueryInterface( (IInspectable *)&impl->IVectorView_GattCharacteristic_iface, iid, out );
}

static ULONG WINAPI gatt_chars_iterable_AddRef( IIterable_GattCharacteristic *iface )
{
    struct gatt_characteristics_vector *impl = impl_from_IIterable_GattCharacteristic( iface );
    return IVectorView_GattCharacteristic_AddRef( &impl->IVectorView_GattCharacteristic_iface );
}

static ULONG WINAPI gatt_chars_iterable_Release( IIterable_GattCharacteristic *iface )
{
    struct gatt_characteristics_vector *impl = impl_from_IIterable_GattCharacteristic( iface );
    return IVectorView_GattCharacteristic_Release( &impl->IVectorView_GattCharacteristic_iface );
}

static HRESULT WINAPI gatt_chars_iterable_GetIids( IIterable_GattCharacteristic *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_chars_iterable_GetRuntimeClassName( IIterable_GattCharacteristic *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_chars_iterable_GetTrustLevel( IIterable_GattCharacteristic *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_chars_iterable_First( IIterable_GattCharacteristic *iface, IIterator_GattCharacteristic **value )
{
    struct gatt_characteristics_vector *impl = impl_from_IIterable_GattCharacteristic( iface );
    struct gatt_chars_iterator *iter;
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    if (!(iter = calloc( 1, sizeof( *iter ) ))) return E_OUTOFMEMORY;
    iter->IIterator_GattCharacteristic_iface.lpVtbl = &gatt_chars_iterator_vtbl;
    iter->ref = 1;
    IVectorView_GattCharacteristic_AddRef( (iter->view = &impl->IVectorView_GattCharacteristic_iface) );
    iter->size = impl->count;
    iter->index = 0;
    *value = &iter->IIterator_GattCharacteristic_iface;
    return S_OK;
}

static const IIterable_GattCharacteristicVtbl gatt_chars_iterable_vtbl =
{
    gatt_chars_iterable_QueryInterface,
    gatt_chars_iterable_AddRef,
    gatt_chars_iterable_Release,
    gatt_chars_iterable_GetIids,
    gatt_chars_iterable_GetRuntimeClassName,
    gatt_chars_iterable_GetTrustLevel,
    gatt_chars_iterable_First,
};

static HRESULT WINAPI gatt_chars_vector_QueryInterface( IVectorView_GattCharacteristic *iface, REFIID iid, void **out )
{
    struct gatt_characteristics_vector *impl = impl_from_IVectorView_GattCharacteristic( iface );
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IVectorView_GattCharacteristic ))
    {
        IUnknown_AddRef( (IUnknown *)iface );
        *out = iface;
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IIterable_GattCharacteristic ))
    {
        IIterable_GattCharacteristic_AddRef( (*out = &impl->IIterable_GattCharacteristic_iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_chars_vector_AddRef( IVectorView_GattCharacteristic *iface )
{
    struct gatt_characteristics_vector *impl = impl_from_IVectorView_GattCharacteristic( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_chars_vector_Release( IVectorView_GattCharacteristic *iface )
{
    struct gatt_characteristics_vector *impl = impl_from_IVectorView_GattCharacteristic( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        ULONG i;
        /* Release all cached characteristics */
        if (impl->cached_characteristics)
        {
            for (i = 0; i < impl->count; i++)
            {
                if (impl->cached_characteristics[i])
                    IUnknown_Release( (IUnknown *)impl->cached_characteristics[i] );
            }
            free( impl->cached_characteristics );
        }
        if (impl->device_handle != INVALID_HANDLE_VALUE)
            CloseHandle( impl->device_handle );
        free( impl->characteristics );
        free( impl );
    }
    return ref;
}


static HRESULT WINAPI gatt_chars_vector_GetIids( IVectorView_GattCharacteristic *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_chars_vector_GetRuntimeClassName( IVectorView_GattCharacteristic *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_chars_vector_GetTrustLevel( IVectorView_GattCharacteristic *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_chars_vector_GetAt( IVectorView_GattCharacteristic *iface, UINT32 index, IGattCharacteristic **value )
{
    struct gatt_characteristics_vector *impl = impl_from_IVectorView_GattCharacteristic( iface );
    HRESULT hr;
    TRACE( "(%p, %u, %p)\n", iface, index, value );
    if (!value) return E_POINTER;
    if (index >= impl->count)
        return E_BOUNDS;

    /* Return cached characteristic with AddRef, matching WinRT semantics */
    if (impl->cached_characteristics && impl->cached_characteristics[index])
    {
        *value = impl->cached_characteristics[index];
        IUnknown_AddRef( (IUnknown *)*value );
        return S_OK;
    }

    /* Create and cache the characteristic on first access */
    hr = gatt_characteristic_create( impl->device_handle, impl->is_radio_handle, impl->device_address,
                                      &impl->service_info, &impl->characteristics[index], value );
    if (SUCCEEDED(hr) && impl->cached_characteristics)
    {
        impl->cached_characteristics[index] = *value;
        IUnknown_AddRef( (IUnknown *)*value ); /* Cache holds one ref, caller gets one ref */
    }
    return hr;
}


static HRESULT WINAPI gatt_chars_vector_get_Size( IVectorView_GattCharacteristic *iface, UINT32 *value )
{
    struct gatt_characteristics_vector *impl = impl_from_IVectorView_GattCharacteristic( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI gatt_chars_vector_IndexOf( IVectorView_GattCharacteristic *iface, IGattCharacteristic *element,
                                                  UINT32 *index, BOOLEAN *found )
{
    FIXME( "(%p, %p, %p, %p): stub!\n", iface, element, index, found );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_chars_vector_GetMany( IVectorView_GattCharacteristic *iface, UINT32 start_index,
                                                  UINT32 items_size, IGattCharacteristic **items, UINT32 *count )
{
    struct gatt_characteristics_vector *impl = impl_from_IVectorView_GattCharacteristic( iface );
    UINT32 i;
    HRESULT hr;
    UINT32 available;
    UINT32 to_copy;

    TRACE( "(%p, %u, %u, %p, %p)\n", iface, start_index, items_size, items, count );
    if (!items) return E_POINTER;
    if (!count) return E_POINTER;

    if (start_index >= impl->count)
    {
        *count = 0;
        return S_OK;
    }

    available = impl->count - start_index;
    to_copy = (items_size < available) ? items_size : available;

    for (i = 0; i < to_copy; i++)
    {
        hr = gatt_characteristic_create( impl->device_handle, impl->is_radio_handle, impl->device_address,
                                          &impl->service_info, &impl->characteristics[start_index + i], &items[i] );
        if (FAILED( hr ))
        {
            while (i > 0)
            {
                struct gatt_characteristic *char_impl = impl_from_IGattCharacteristic( items[i - 1] );
                char_impl->IGattCharacteristic_iface.lpVtbl->Release( items[i - 1] );
                i--;
            }
            *count = 0;
            return hr;
        }
    }

    *count = to_copy;
    return S_OK;
}

static const IVectorView_GattCharacteristicVtbl gatt_characteristics_vector_vtbl =
{
    gatt_chars_vector_QueryInterface,
    gatt_chars_vector_AddRef,
    gatt_chars_vector_Release,
    gatt_chars_vector_GetIids,
    gatt_chars_vector_GetRuntimeClassName,
    gatt_chars_vector_GetTrustLevel,
    gatt_chars_vector_GetAt,
    gatt_chars_vector_get_Size,
    gatt_chars_vector_IndexOf,
    gatt_chars_vector_GetMany
};

static int compare_characteristics( const void *a, const void *b )
{
    const BTH_LE_GATT_CHARACTERISTIC *char_a = a;
    const BTH_LE_GATT_CHARACTERISTIC *char_b = b;
    GUID guid_a, guid_b;

    /* Sort by UUID to ensure Firmware (2003) comes before Notification (2102) */
    bth_le_uuid_to_guid( &char_a->CharacteristicUuid, &guid_a );
    bth_le_uuid_to_guid( &char_b->CharacteristicUuid, &guid_b );

    if (guid_a.Data1 != guid_b.Data1)
        return (guid_a.Data1 > guid_b.Data1) ? 1 : -1;
    if (guid_a.Data2 != guid_b.Data2)
        return (guid_a.Data2 > guid_b.Data2) ? 1 : -1;
    if (guid_a.Data3 != guid_b.Data3)
        return (guid_a.Data3 > guid_b.Data3) ? 1 : -1;

    return memcmp( guid_a.Data4, guid_b.Data4, sizeof(guid_a.Data4) );
}

struct gatt_cached_chars_vector
{
    IVectorView_GattCharacteristic IVectorView_GattCharacteristic_iface;
    LONG ref;
    UINT32 count;
    IGattCharacteristic **chars;
};

static inline struct gatt_cached_chars_vector *impl_from_cached_IVectorView( IVectorView_GattCharacteristic *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_cached_chars_vector, IVectorView_GattCharacteristic_iface );
}

static HRESULT WINAPI gatt_cached_vector_QueryInterface( IVectorView_GattCharacteristic *iface, REFIID iid, void **out )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IVectorView_GattCharacteristic ))
    {
        IUnknown_AddRef( iface );
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_cached_vector_AddRef( IVectorView_GattCharacteristic *iface )
{
    struct gatt_cached_chars_vector *impl = impl_from_cached_IVectorView( iface );
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI gatt_cached_vector_Release( IVectorView_GattCharacteristic *iface )
{
    struct gatt_cached_chars_vector *impl = impl_from_cached_IVectorView( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref)
    {
        UINT32 i;
        for (i = 0; i < impl->count; i++)
            if (impl->chars[i]) IUnknown_Release( (IUnknown *)impl->chars[i] );
        free( impl->chars );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_cached_vector_GetIids( IVectorView_GattCharacteristic *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_cached_vector_GetRuntimeClassName( IVectorView_GattCharacteristic *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_cached_vector_GetTrustLevel( IVectorView_GattCharacteristic *iface, TrustLevel *trust_level )
{
    FIXME( "(%p, %p): stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_cached_vector_GetAt( IVectorView_GattCharacteristic *iface, UINT32 index, IGattCharacteristic **value )
{
    struct gatt_cached_chars_vector *impl = impl_from_cached_IVectorView( iface );
    if (index >= impl->count) return E_BOUNDS;
    *value = impl->chars[index];
    if (*value) IUnknown_AddRef( (IUnknown *)*value );
    return S_OK;
}

static HRESULT WINAPI gatt_cached_vector_get_Size( IVectorView_GattCharacteristic *iface, UINT32 *value )
{
    struct gatt_cached_chars_vector *impl = impl_from_cached_IVectorView( iface );
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI gatt_cached_vector_IndexOf( IVectorView_GattCharacteristic *iface, IGattCharacteristic *element,
                                                   UINT32 *index, BOOLEAN *found )
{
    FIXME( "(%p, %p, %p, %p): stub!\n", iface, element, index, found );
    *found = FALSE;
    return S_OK;
}

static HRESULT WINAPI gatt_cached_vector_GetMany( IVectorView_GattCharacteristic *iface, UINT32 start_index,
                                                   UINT32 items_size, IGattCharacteristic **items, UINT32 *count )
{
    FIXME( "(%p, %u, %u, %p, %p): stub!\n", iface, start_index, items_size, items, count );
    *count = 0;
    return E_NOTIMPL;
}

static const IVectorView_GattCharacteristicVtbl gatt_cached_chars_vector_vtbl =
{
    gatt_cached_vector_QueryInterface,
    gatt_cached_vector_AddRef,
    gatt_cached_vector_Release,
    gatt_cached_vector_GetIids,
    gatt_cached_vector_GetRuntimeClassName,
    gatt_cached_vector_GetTrustLevel,
    gatt_cached_vector_GetAt,
    gatt_cached_vector_get_Size,
    gatt_cached_vector_IndexOf,
    gatt_cached_vector_GetMany
};

static HRESULT gatt_cached_chars_vector_create( IGattCharacteristic **chars, UINT32 count,
                                                 IVectorView_GattCharacteristic **out )
{
    struct gatt_cached_chars_vector *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    impl->IVectorView_GattCharacteristic_iface.lpVtbl = &gatt_cached_chars_vector_vtbl;
    impl->ref = 1;
    impl->count = count;
    impl->chars = chars;
    *out = &impl->IVectorView_GattCharacteristic_iface;
    return S_OK;
}

static HRESULT gatt_characteristics_vector_create( const BTH_LE_GATT_CHARACTERISTIC *characteristics, ULONG count,
                                                   const BTH_LE_GATT_SERVICE *service, HANDLE device_handle,
                                                   BOOL is_radio_handle, UINT64 device_address,
                                                   const GUID *filter_uuid,
                                                   IVectorView_GattCharacteristic **out )
{
    struct gatt_characteristics_vector *impl;
    HANDLE dup_handle = INVALID_HANDLE_VALUE;
    ULONG i;
    ULONG filtered_count = 0;

    /* Duplicate the device handle since we close it in Release */
    if (device_handle != INVALID_HANDLE_VALUE)
    {
        if (!DuplicateHandle( GetCurrentProcess(), device_handle,
                              GetCurrentProcess(), &dup_handle,
                              0, FALSE, DUPLICATE_SAME_ACCESS ))
        {
            WARN( "Failed to duplicate device handle: %lu\n", GetLastError() );
            return HRESULT_FROM_WIN32( GetLastError() );
        }
    }

    /* First pass: count matches */
    for (i = 0; i < count; i++)
    {
            if (filter_uuid)
            {
                GUID char_uuid;
                bth_le_uuid_to_guid( &characteristics[i].CharacteristicUuid, &char_uuid );
                if (!IsEqualGUID( &char_uuid, filter_uuid )) continue;
            }
        filtered_count++;
    }

    if (!(impl = calloc( 1, sizeof( *impl ) )))
    {
        if (dup_handle != INVALID_HANDLE_VALUE) CloseHandle( dup_handle );
        return E_OUTOFMEMORY;
    }
    impl->IVectorView_GattCharacteristic_iface.lpVtbl = &gatt_characteristics_vector_vtbl;
    impl->IIterable_GattCharacteristic_iface.lpVtbl = &gatt_chars_iterable_vtbl;
    impl->ref = 1;
    impl->count = filtered_count;
    impl->device_handle = dup_handle;
    impl->is_radio_handle = is_radio_handle;
    impl->device_address = device_address;
    impl->service_info = *service;

    TRACE( "gatt_characteristics_vector_create: input_count=%lu filtered_count=%lu filter=%s \n",
         count, filtered_count, debugstr_guid(filter_uuid) );

    if (filtered_count > 0)
    {
        ULONG current = 0;
        impl->characteristics = malloc( filtered_count * sizeof( BTH_LE_GATT_CHARACTERISTIC ) );
        if (!impl->characteristics)
        {
            free( impl );
            return E_OUTOFMEMORY;
        }
        
        for (i = 0; i < count; i++)
        {
            if (filter_uuid && !le_uuid_equal( &characteristics[i].CharacteristicUuid, filter_uuid )) continue;
            impl->characteristics[current] = characteristics[i];
            current++;
        }
        
        if (!filter_uuid && filtered_count > 1)
            qsort( impl->characteristics, filtered_count, sizeof(BTH_LE_GATT_CHARACTERISTIC), compare_characteristics );

        impl->cached_characteristics = calloc( filtered_count, sizeof(IGattCharacteristic*) );
    }
    *out = &impl->IVectorView_GattCharacteristic_iface;
    return S_OK;
}

/* IGattCharacteristicsResult implementation */
struct gatt_characteristics_result
{
    IGattCharacteristicsResult IGattCharacteristicsResult_iface;
    LONG ref;
    GattCommunicationStatus status;
    IVectorView_GattCharacteristic *chars_vector;
};

static inline struct gatt_characteristics_result *impl_from_IGattCharacteristicsResult( IGattCharacteristicsResult *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_characteristics_result, IGattCharacteristicsResult_iface );
}

static HRESULT WINAPI gatt_chars_result_QueryInterface( IGattCharacteristicsResult *iface, REFIID iid, void **out )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGattCharacteristicsResult ))
    {
        IGattCharacteristicsResult_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_chars_result_AddRef( IGattCharacteristicsResult *iface )
{
    struct gatt_characteristics_result *impl = impl_from_IGattCharacteristicsResult( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_chars_result_Release( IGattCharacteristicsResult *iface )
{
    struct gatt_characteristics_result *impl = impl_from_IGattCharacteristicsResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->chars_vector) IVectorView_GattCharacteristic_Release( impl->chars_vector );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_chars_result_GetIids( IGattCharacteristicsResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_chars_result_GetRuntimeClassName( IGattCharacteristicsResult *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattCharacteristicsResult";
    TRACE( "(%p, %p)\n", iface, class_name );
    if (!class_name) return E_POINTER;
    return WindowsCreateString( name, ARRAY_SIZE( name ) - 1, class_name );
}

static HRESULT WINAPI gatt_chars_result_GetTrustLevel( IGattCharacteristicsResult *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_chars_result_get_Status( IGattCharacteristicsResult *iface, GattCommunicationStatus *value )
{
    struct gatt_characteristics_result *impl = impl_from_IGattCharacteristicsResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI gatt_chars_result_get_ProtocolError( IGattCharacteristicsResult *iface, IReference_BYTE **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI gatt_chars_result_get_Characteristics( IGattCharacteristicsResult *iface, IVectorView_GattCharacteristic **value )
{
    struct gatt_characteristics_result *impl = impl_from_IGattCharacteristicsResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->chars_vector)
    {
        IVectorView_GattCharacteristic_AddRef( impl->chars_vector );
        *value = impl->chars_vector;
    }
    else
    {
        *value = NULL;
    }
    return S_OK;
}

static const IGattCharacteristicsResultVtbl gatt_characteristics_result_vtbl =
{
    gatt_chars_result_QueryInterface,
    gatt_chars_result_AddRef,
    gatt_chars_result_Release,
    gatt_chars_result_GetIids,
    gatt_chars_result_GetRuntimeClassName,
    gatt_chars_result_GetTrustLevel,
    gatt_chars_result_get_Status,
    gatt_chars_result_get_ProtocolError,
    gatt_chars_result_get_Characteristics
};

static HRESULT gatt_characteristics_result_create( GattCommunicationStatus status, IVectorView_GattCharacteristic *chars_vector,
                                                   IGattCharacteristicsResult **out )
{
    struct gatt_characteristics_result *impl;

    TRACE( "gatt_characteristics_result_create: status=%d chars_vector=%p \n", status, chars_vector );
    if (!chars_vector)
        TRACE( "gatt_characteristics_result_create: WARNING - creating result with NULL chars_vector! \n" );

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattCharacteristicsResult_iface.lpVtbl = &gatt_characteristics_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    impl->chars_vector = chars_vector;
    if (chars_vector) IVectorView_GattCharacteristic_AddRef( chars_vector );
    *out = &impl->IGattCharacteristicsResult_iface;
    return S_OK;
}

/* IAsyncOperation_GattCharacteristicsResult implementation */
struct async_gatt_chars_op
{
    IAsyncOperation_GattCharacteristicsResult IAsyncOperation_GattCharacteristicsResult_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IAsyncOperationCompletedHandler_GattCharacteristicsResult *handler;
    IGattCharacteristicsResult *result;
    AsyncStatus status;
};

static inline struct async_gatt_chars_op *impl_from_IAsyncOperation_GattCharacteristicsResult( IAsyncOperation_GattCharacteristicsResult *iface )
{
    return CONTAINING_RECORD( iface, struct async_gatt_chars_op, IAsyncOperation_GattCharacteristicsResult_iface );
}

static HRESULT WINAPI async_gatt_chars_op_QueryInterface( IAsyncOperation_GattCharacteristicsResult *iface, REFIID iid, void **out )
{
    struct async_gatt_chars_op *impl = impl_from_IAsyncOperation_GattCharacteristicsResult( iface );
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_GattCharacteristicsResult ))
    {
        IAsyncOperation_GattCharacteristicsResult_AddRef( (*out = &impl->IAsyncOperation_GattCharacteristicsResult_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IAsyncInfo_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI async_gatt_chars_op_AddRef( IAsyncOperation_GattCharacteristicsResult *iface )
{
    struct async_gatt_chars_op *impl = impl_from_IAsyncOperation_GattCharacteristicsResult( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_gatt_chars_op_Release( IAsyncOperation_GattCharacteristicsResult *iface )
{
    struct async_gatt_chars_op *impl = impl_from_IAsyncOperation_GattCharacteristicsResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->handler) IAsyncOperationCompletedHandler_GattCharacteristicsResult_Release( impl->handler );
        if (impl->result) IGattCharacteristicsResult_Release( impl->result );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_gatt_chars_op_GetIids( IAsyncOperation_GattCharacteristicsResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_chars_op_GetRuntimeClassName( IAsyncOperation_GattCharacteristicsResult *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_chars_op_GetTrustLevel( IAsyncOperation_GattCharacteristicsResult *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI async_gatt_chars_op_put_Completed( IAsyncOperation_GattCharacteristicsResult *iface,
                                                          IAsyncOperationCompletedHandler_GattCharacteristicsResult *handler )
{
    struct async_gatt_chars_op *impl = impl_from_IAsyncOperation_GattCharacteristicsResult( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (impl->handler) IAsyncOperationCompletedHandler_GattCharacteristicsResult_Release( impl->handler );
    impl->handler = handler;
    if (handler) IAsyncOperationCompletedHandler_GattCharacteristicsResult_AddRef( handler );
    if (impl->status == Completed && handler)
        IAsyncOperationCompletedHandler_GattCharacteristicsResult_Invoke( handler, iface, impl->status );
    return S_OK;
}

static HRESULT WINAPI async_gatt_chars_op_get_Completed( IAsyncOperation_GattCharacteristicsResult *iface,
                                                          IAsyncOperationCompletedHandler_GattCharacteristicsResult **handler )
{
    struct async_gatt_chars_op *impl = impl_from_IAsyncOperation_GattCharacteristicsResult( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;
    *handler = impl->handler;
    if (*handler) IAsyncOperationCompletedHandler_GattCharacteristicsResult_AddRef( *handler );
    return S_OK;
}

static HRESULT WINAPI async_gatt_chars_op_GetResults( IAsyncOperation_GattCharacteristicsResult *iface, IGattCharacteristicsResult **result )
{
    struct async_gatt_chars_op *impl = impl_from_IAsyncOperation_GattCharacteristicsResult( iface );
    TRACE( "(%p, %p)\n", iface, result );
    if (!result) return E_POINTER;
    *result = impl->result;
    if (*result) IGattCharacteristicsResult_AddRef( *result );
    return S_OK;
}

static const IAsyncOperation_GattCharacteristicsResultVtbl async_gatt_chars_op_vtbl =
{
    async_gatt_chars_op_QueryInterface,
    async_gatt_chars_op_AddRef,
    async_gatt_chars_op_Release,
    async_gatt_chars_op_GetIids,
    async_gatt_chars_op_GetRuntimeClassName,
    async_gatt_chars_op_GetTrustLevel,
    async_gatt_chars_op_put_Completed,
    async_gatt_chars_op_get_Completed,
    async_gatt_chars_op_GetResults
};

static inline struct async_gatt_chars_op *impl_from_chars_IAsyncInfo( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct async_gatt_chars_op, IAsyncInfo_iface );
}

static HRESULT WINAPI async_gatt_chars_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    struct async_gatt_chars_op *impl = impl_from_chars_IAsyncInfo( iface );
    return async_gatt_chars_op_QueryInterface( &impl->IAsyncOperation_GattCharacteristicsResult_iface, iid, out );
}

static ULONG WINAPI async_gatt_chars_info_AddRef( IAsyncInfo *iface )
{
    struct async_gatt_chars_op *impl = impl_from_chars_IAsyncInfo( iface );
    return async_gatt_chars_op_AddRef( &impl->IAsyncOperation_GattCharacteristicsResult_iface );
}

static ULONG WINAPI async_gatt_chars_info_Release( IAsyncInfo *iface )
{
    struct async_gatt_chars_op *impl = impl_from_chars_IAsyncInfo( iface );
    return async_gatt_chars_op_Release( &impl->IAsyncOperation_GattCharacteristicsResult_iface );
}

static HRESULT WINAPI async_gatt_chars_info_GetIids( IAsyncInfo *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_chars_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_chars_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_chars_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    FIXME( "(%p, %p): stub!\n", iface, id );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_chars_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_gatt_chars_op *impl = impl_from_chars_IAsyncInfo( iface );
    TRACE( "(%p, %p)\n", iface, status );
    if (!status) return E_POINTER;
    *status = impl->status;
    return S_OK;
}

static HRESULT WINAPI async_gatt_chars_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    FIXME( "(%p, %p): stub!\n", iface, error_code );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_chars_info_Cancel( IAsyncInfo *iface )
{
    FIXME( "(%p): stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_chars_info_Close( IAsyncInfo *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IAsyncInfoVtbl async_gatt_chars_info_vtbl =
{
    async_gatt_chars_info_QueryInterface,
    async_gatt_chars_info_AddRef,
    async_gatt_chars_info_Release,
    async_gatt_chars_info_GetIids,
    async_gatt_chars_info_GetRuntimeClassName,
    async_gatt_chars_info_GetTrustLevel,
    async_gatt_chars_info_get_Id,
    async_gatt_chars_info_get_Status,
    async_gatt_chars_info_get_ErrorCode,
    async_gatt_chars_info_Cancel,
    async_gatt_chars_info_Close
};

static HRESULT async_gatt_chars_op_create( IGattCharacteristicsResult *result, IAsyncOperation_GattCharacteristicsResult **out )
{
    struct async_gatt_chars_op *impl;
    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_GattCharacteristicsResult_iface.lpVtbl = &async_gatt_chars_op_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_gatt_chars_info_vtbl;
    impl->ref = 1;
    impl->result = result;
    impl->status = Completed;
    if (result) IGattCharacteristicsResult_AddRef( result );
    *out = &impl->IAsyncOperation_GattCharacteristicsResult_iface;
    return S_OK;
}

/* IGattReadResult implementation */
struct gatt_read_result
{
    IGattReadResult IGattReadResult_iface;
    LONG ref;
    GattCommunicationStatus status;
    IBuffer *value;
};

static inline struct gatt_read_result *impl_from_IGattReadResult( IGattReadResult *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_read_result, IGattReadResult_iface );
}

static HRESULT WINAPI gatt_read_result_QueryInterface( IGattReadResult *iface, REFIID iid, void **out )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGattReadResult ))
    {
        IGattReadResult_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_read_result_AddRef( IGattReadResult *iface )
{
    struct gatt_read_result *impl = impl_from_IGattReadResult( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_read_result_Release( IGattReadResult *iface )
{
    struct gatt_read_result *impl = impl_from_IGattReadResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->value) IBuffer_Release( impl->value );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_read_result_GetIids( IGattReadResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_read_result_GetRuntimeClassName( IGattReadResult *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattReadResult";
    TRACE( "(%p, %p)\n", iface, class_name );
    if (!class_name) return E_POINTER;
    return WindowsCreateString( name, ARRAY_SIZE( name ) - 1, class_name );
}

static HRESULT WINAPI gatt_read_result_GetTrustLevel( IGattReadResult *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_read_result_get_Status( IGattReadResult *iface, GattCommunicationStatus *value )
{
    struct gatt_read_result *impl = impl_from_IGattReadResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI gatt_read_result_get_Value( IGattReadResult *iface, IBuffer **value )
{
    struct gatt_read_result *impl = impl_from_IGattReadResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->value)
    {
        IBuffer_AddRef( impl->value );
        *value = impl->value;
    }
    else
    {
        *value = NULL;
    }
    return S_OK;
}

static const IGattReadResultVtbl gatt_read_result_vtbl =
{
    gatt_read_result_QueryInterface,
    gatt_read_result_AddRef,
    gatt_read_result_Release,
    gatt_read_result_GetIids,
    gatt_read_result_GetRuntimeClassName,
    gatt_read_result_GetTrustLevel,
    gatt_read_result_get_Status,
    gatt_read_result_get_Value
};

static HRESULT gatt_read_result_create( GattCommunicationStatus status, IBuffer *value, IGattReadResult **out )
{
    struct gatt_read_result *impl;
    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattReadResult_iface.lpVtbl = &gatt_read_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    impl->value = value;
    if (value) IBuffer_AddRef( value );
    *out = &impl->IGattReadResult_iface;
    return S_OK;
}

/* IGattValueChangedEventArgs implementation */
struct gatt_value_changed_event_args
{
    IGattValueChangedEventArgs IGattValueChangedEventArgs_iface;
    LONG ref;
    IBuffer *value;
    DateTime timestamp;
};

static inline struct gatt_value_changed_event_args *impl_from_IGattValueChangedEventArgs( IGattValueChangedEventArgs *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_value_changed_event_args, IGattValueChangedEventArgs_iface );
}

static HRESULT WINAPI gatt_value_changed_args_QueryInterface( IGattValueChangedEventArgs *iface, REFIID iid, void **out )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGattValueChangedEventArgs ))
    {
        IGattValueChangedEventArgs_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_value_changed_args_AddRef( IGattValueChangedEventArgs *iface )
{
    struct gatt_value_changed_event_args *impl = impl_from_IGattValueChangedEventArgs( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_value_changed_args_Release( IGattValueChangedEventArgs *iface )
{
    struct gatt_value_changed_event_args *impl = impl_from_IGattValueChangedEventArgs( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->value) IBuffer_Release( impl->value );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_value_changed_args_GetIids( IGattValueChangedEventArgs *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_value_changed_args_GetRuntimeClassName( IGattValueChangedEventArgs *iface, HSTRING *class_name )
{
    static const WCHAR name[] = L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattValueChangedEventArgs";
    TRACE( "(%p, %p)\n", iface, class_name );
    if (!class_name) return E_POINTER;
    return WindowsCreateString( name, ARRAY_SIZE( name ) - 1, class_name );
}

static HRESULT WINAPI gatt_value_changed_args_GetTrustLevel( IGattValueChangedEventArgs *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_value_changed_args_get_CharacteristicValue( IGattValueChangedEventArgs *iface, IBuffer **value )
{
    struct gatt_value_changed_event_args *impl = impl_from_IGattValueChangedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->value)
    {
        IBuffer_AddRef( impl->value );
        *value = impl->value;
    }
    else
        *value = NULL;
    return S_OK;
}

static HRESULT WINAPI gatt_value_changed_args_get_Timestamp( IGattValueChangedEventArgs *iface, DateTime *value )
{
    struct gatt_value_changed_event_args *impl = impl_from_IGattValueChangedEventArgs( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->timestamp;
    return S_OK;
}

static const IGattValueChangedEventArgsVtbl gatt_value_changed_event_args_vtbl =
{
    gatt_value_changed_args_QueryInterface,
    gatt_value_changed_args_AddRef,
    gatt_value_changed_args_Release,
    gatt_value_changed_args_GetIids,
    gatt_value_changed_args_GetRuntimeClassName,
    gatt_value_changed_args_GetTrustLevel,
    gatt_value_changed_args_get_CharacteristicValue,
    gatt_value_changed_args_get_Timestamp
};

static HRESULT gatt_value_changed_event_args_create( IBuffer *value, IGattValueChangedEventArgs **out )
{
    struct gatt_value_changed_event_args *impl;
    FILETIME ft;
    static ULONGLONG last_ts = 0;
    ULONGLONG ts;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattValueChangedEventArgs_iface.lpVtbl = &gatt_value_changed_event_args_vtbl;
    impl->ref = 1;
    impl->value = value;
    if (value) IBuffer_AddRef( value );

    GetSystemTimePreciseAsFileTime( &ft );
    ts = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    
    /* Ensure monotonic and non-zero deltas for app logic - thread safe */
    EnterCriticalSection( &timestamp_cs );
    if (ts <= last_ts) ts = last_ts + 1;
    last_ts = ts;
    LeaveCriticalSection( &timestamp_cs );
    
    impl->timestamp.UniversalTime = ts;

    *out = &impl->IGattValueChangedEventArgs_iface;
    return S_OK;
}

/* IAsyncOperation_GattReadResult implementation */
struct async_gatt_read_op
{
    IAsyncOperation_GattReadResult IAsyncOperation_GattReadResult_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IAsyncOperationCompletedHandler_GattReadResult *handler;
    IGattReadResult *result;
    AsyncStatus status;
};

static inline struct async_gatt_read_op *impl_from_IAsyncOperation_GattReadResult( IAsyncOperation_GattReadResult *iface )
{
    return CONTAINING_RECORD( iface, struct async_gatt_read_op, IAsyncOperation_GattReadResult_iface );
}

static HRESULT WINAPI async_gatt_read_op_QueryInterface( IAsyncOperation_GattReadResult *iface, REFIID iid, void **out )
{
    struct async_gatt_read_op *impl = impl_from_IAsyncOperation_GattReadResult( iface );
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_GattReadResult ))
    {
        IAsyncOperation_GattReadResult_AddRef( (*out = &impl->IAsyncOperation_GattReadResult_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IAsyncInfo_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI async_gatt_read_op_AddRef( IAsyncOperation_GattReadResult *iface )
{
    struct async_gatt_read_op *impl = impl_from_IAsyncOperation_GattReadResult( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_gatt_read_op_Release( IAsyncOperation_GattReadResult *iface )
{
    struct async_gatt_read_op *impl = impl_from_IAsyncOperation_GattReadResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->handler) IAsyncOperationCompletedHandler_GattReadResult_Release( impl->handler );
        if (impl->result) IGattReadResult_Release( impl->result );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_gatt_read_op_GetIids( IAsyncOperation_GattReadResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_read_op_GetRuntimeClassName( IAsyncOperation_GattReadResult *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_read_op_GetTrustLevel( IAsyncOperation_GattReadResult *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI async_gatt_read_op_put_Completed( IAsyncOperation_GattReadResult *iface,
                                                         IAsyncOperationCompletedHandler_GattReadResult *handler )
{
    struct async_gatt_read_op *impl = impl_from_IAsyncOperation_GattReadResult( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (impl->handler) IAsyncOperationCompletedHandler_GattReadResult_Release( impl->handler );
    impl->handler = handler;
    if (handler) IAsyncOperationCompletedHandler_GattReadResult_AddRef( handler );
    if (impl->status == Completed && handler)
        IAsyncOperationCompletedHandler_GattReadResult_Invoke( handler, iface, impl->status );
    return S_OK;
}

static HRESULT WINAPI async_gatt_read_op_get_Completed( IAsyncOperation_GattReadResult *iface,
                                                         IAsyncOperationCompletedHandler_GattReadResult **handler )
{
    struct async_gatt_read_op *impl = impl_from_IAsyncOperation_GattReadResult( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;
    *handler = impl->handler;
    if (*handler) IAsyncOperationCompletedHandler_GattReadResult_AddRef( *handler );
    return S_OK;
}

static HRESULT WINAPI async_gatt_read_op_GetResults( IAsyncOperation_GattReadResult *iface, IGattReadResult **result )
{
    struct async_gatt_read_op *impl = impl_from_IAsyncOperation_GattReadResult( iface );
    TRACE( "(%p, %p)\n", iface, result );
    if (!result) return E_POINTER;
    *result = impl->result;
    if (*result) IGattReadResult_AddRef( *result );
    return S_OK;
}

static const IAsyncOperation_GattReadResultVtbl async_gatt_read_op_vtbl =
{
    async_gatt_read_op_QueryInterface,
    async_gatt_read_op_AddRef,
    async_gatt_read_op_Release,
    async_gatt_read_op_GetIids,
    async_gatt_read_op_GetRuntimeClassName,
    async_gatt_read_op_GetTrustLevel,
    async_gatt_read_op_put_Completed,
    async_gatt_read_op_get_Completed,
    async_gatt_read_op_GetResults
};

static inline struct async_gatt_read_op *impl_from_read_IAsyncInfo( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct async_gatt_read_op, IAsyncInfo_iface );
}

static HRESULT WINAPI async_gatt_read_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    struct async_gatt_read_op *impl = impl_from_read_IAsyncInfo( iface );
    return async_gatt_read_op_QueryInterface( &impl->IAsyncOperation_GattReadResult_iface, iid, out );
}

static ULONG WINAPI async_gatt_read_info_AddRef( IAsyncInfo *iface )
{
    struct async_gatt_read_op *impl = impl_from_read_IAsyncInfo( iface );
    return async_gatt_read_op_AddRef( &impl->IAsyncOperation_GattReadResult_iface );
}

static ULONG WINAPI async_gatt_read_info_Release( IAsyncInfo *iface )
{
    struct async_gatt_read_op *impl = impl_from_read_IAsyncInfo( iface );
    return async_gatt_read_op_Release( &impl->IAsyncOperation_GattReadResult_iface );
}

static HRESULT WINAPI async_gatt_read_info_GetIids( IAsyncInfo *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_read_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_read_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_read_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    FIXME( "(%p, %p): stub!\n", iface, id );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_read_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_gatt_read_op *impl = impl_from_read_IAsyncInfo( iface );
    TRACE( "(%p, %p)\n", iface, status );
    if (!status) return E_POINTER;
    *status = impl->status;
    return S_OK;
}

static HRESULT WINAPI async_gatt_read_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    FIXME( "(%p, %p): stub!\n", iface, error_code );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_read_info_Cancel( IAsyncInfo *iface )
{
    FIXME( "(%p): stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_read_info_Close( IAsyncInfo *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IAsyncInfoVtbl async_gatt_read_info_vtbl =
{
    async_gatt_read_info_QueryInterface,
    async_gatt_read_info_AddRef,
    async_gatt_read_info_Release,
    async_gatt_read_info_GetIids,
    async_gatt_read_info_GetRuntimeClassName,
    async_gatt_read_info_GetTrustLevel,
    async_gatt_read_info_get_Id,
    async_gatt_read_info_get_Status,
    async_gatt_read_info_get_ErrorCode,
    async_gatt_read_info_Cancel,
    async_gatt_read_info_Close
};

static HRESULT async_gatt_read_op_create( IGattReadResult *result, IAsyncOperation_GattReadResult **out )
{
    struct async_gatt_read_op *impl;
    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_GattReadResult_iface.lpVtbl = &async_gatt_read_op_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_gatt_read_info_vtbl;
    impl->ref = 1;
    impl->result = result;
    impl->status = Completed;
    if (result) IGattReadResult_AddRef( result );
    *out = &impl->IAsyncOperation_GattReadResult_iface;
    return S_OK;
}

/* IAsyncOperation_GattCommunicationStatus implementation */
struct async_gatt_comm_status_op
{
    IAsyncOperation_GattCommunicationStatus IAsyncOperation_GattCommunicationStatus_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IAsyncOperationCompletedHandler_GattCommunicationStatus *handler;
    GattCommunicationStatus result;
    AsyncStatus status;
};

static inline struct async_gatt_comm_status_op *impl_from_IAsyncOperation_GattCommunicationStatus( IAsyncOperation_GattCommunicationStatus *iface )
{
    return CONTAINING_RECORD( iface, struct async_gatt_comm_status_op, IAsyncOperation_GattCommunicationStatus_iface );
}

static HRESULT WINAPI async_gatt_comm_status_op_QueryInterface( IAsyncOperation_GattCommunicationStatus *iface, REFIID iid, void **out )
{
    struct async_gatt_comm_status_op *impl = impl_from_IAsyncOperation_GattCommunicationStatus( iface );
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_GattCommunicationStatus ))
    {
        IAsyncOperation_GattCommunicationStatus_AddRef( (*out = &impl->IAsyncOperation_GattCommunicationStatus_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IAsyncInfo_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI async_gatt_comm_status_op_AddRef( IAsyncOperation_GattCommunicationStatus *iface )
{
    struct async_gatt_comm_status_op *impl = impl_from_IAsyncOperation_GattCommunicationStatus( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_gatt_comm_status_op_Release( IAsyncOperation_GattCommunicationStatus *iface )
{
    struct async_gatt_comm_status_op *impl = impl_from_IAsyncOperation_GattCommunicationStatus( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->handler) IAsyncOperationCompletedHandler_GattCommunicationStatus_Release( impl->handler );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_gatt_comm_status_op_GetIids( IAsyncOperation_GattCommunicationStatus *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_comm_status_op_GetRuntimeClassName( IAsyncOperation_GattCommunicationStatus *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_comm_status_op_GetTrustLevel( IAsyncOperation_GattCommunicationStatus *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI async_gatt_comm_status_op_put_Completed( IAsyncOperation_GattCommunicationStatus *iface,
                                                                IAsyncOperationCompletedHandler_GattCommunicationStatus *handler )
{
    struct async_gatt_comm_status_op *impl = impl_from_IAsyncOperation_GattCommunicationStatus( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (impl->handler) IAsyncOperationCompletedHandler_GattCommunicationStatus_Release( impl->handler );
    impl->handler = handler;
    if (handler) IAsyncOperationCompletedHandler_GattCommunicationStatus_AddRef( handler );
    if (impl->status == Completed && handler)
        IAsyncOperationCompletedHandler_GattCommunicationStatus_Invoke( handler, iface, impl->status );
    return S_OK;
}

static HRESULT WINAPI async_gatt_comm_status_op_get_Completed( IAsyncOperation_GattCommunicationStatus *iface,
                                                                IAsyncOperationCompletedHandler_GattCommunicationStatus **handler )
{
    struct async_gatt_comm_status_op *impl = impl_from_IAsyncOperation_GattCommunicationStatus( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;
    *handler = impl->handler;
    if (*handler) IAsyncOperationCompletedHandler_GattCommunicationStatus_AddRef( *handler );
    return S_OK;
}

static HRESULT WINAPI async_gatt_comm_status_op_GetResults( IAsyncOperation_GattCommunicationStatus *iface, GattCommunicationStatus *result )
{
    struct async_gatt_comm_status_op *impl = impl_from_IAsyncOperation_GattCommunicationStatus( iface );
    TRACE( "(%p, %p)\n", iface, result );
    if (!result) return E_POINTER;
    *result = impl->result;
    return S_OK;
}

static const IAsyncOperation_GattCommunicationStatusVtbl async_gatt_comm_status_op_vtbl =
{
    async_gatt_comm_status_op_QueryInterface,
    async_gatt_comm_status_op_AddRef,
    async_gatt_comm_status_op_Release,
    async_gatt_comm_status_op_GetIids,
    async_gatt_comm_status_op_GetRuntimeClassName,
    async_gatt_comm_status_op_GetTrustLevel,
    async_gatt_comm_status_op_put_Completed,
    async_gatt_comm_status_op_get_Completed,
    async_gatt_comm_status_op_GetResults
};

static inline struct async_gatt_comm_status_op *impl_from_comm_status_IAsyncInfo( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct async_gatt_comm_status_op, IAsyncInfo_iface );
}

static HRESULT WINAPI async_gatt_comm_status_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    struct async_gatt_comm_status_op *impl = impl_from_comm_status_IAsyncInfo( iface );
    return async_gatt_comm_status_op_QueryInterface( &impl->IAsyncOperation_GattCommunicationStatus_iface, iid, out );
}

static ULONG WINAPI async_gatt_comm_status_info_AddRef( IAsyncInfo *iface )
{
    struct async_gatt_comm_status_op *impl = impl_from_comm_status_IAsyncInfo( iface );
    return async_gatt_comm_status_op_AddRef( &impl->IAsyncOperation_GattCommunicationStatus_iface );
}

static ULONG WINAPI async_gatt_comm_status_info_Release( IAsyncInfo *iface )
{
    struct async_gatt_comm_status_op *impl = impl_from_comm_status_IAsyncInfo( iface );
    return async_gatt_comm_status_op_Release( &impl->IAsyncOperation_GattCommunicationStatus_iface );
}

static HRESULT WINAPI async_gatt_comm_status_info_GetIids( IAsyncInfo *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_comm_status_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_comm_status_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_comm_status_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    FIXME( "(%p, %p): stub!\n", iface, id );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_comm_status_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_gatt_comm_status_op *impl = impl_from_comm_status_IAsyncInfo( iface );
    TRACE( "(%p, %p)\n", iface, status );
    if (!status) return E_POINTER;
    *status = impl->status;
    return S_OK;
}

static HRESULT WINAPI async_gatt_comm_status_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    FIXME( "(%p, %p): stub!\n", iface, error_code );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_comm_status_info_Cancel( IAsyncInfo *iface )
{
    FIXME( "(%p): stub!\n", iface );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_gatt_comm_status_info_Close( IAsyncInfo *iface )
{
    TRACE( "(%p)\n", iface );
    return S_OK;
}

static const IAsyncInfoVtbl async_gatt_comm_status_info_vtbl =
{
    async_gatt_comm_status_info_QueryInterface,
    async_gatt_comm_status_info_AddRef,
    async_gatt_comm_status_info_Release,
    async_gatt_comm_status_info_GetIids,
    async_gatt_comm_status_info_GetRuntimeClassName,
    async_gatt_comm_status_info_GetTrustLevel,
    async_gatt_comm_status_info_get_Id,
    async_gatt_comm_status_info_get_Status,
    async_gatt_comm_status_info_get_ErrorCode,
    async_gatt_comm_status_info_Cancel,
    async_gatt_comm_status_info_Close
};

static HRESULT async_gatt_comm_status_op_create( GattCommunicationStatus result, IAsyncOperation_GattCommunicationStatus **out )
{
    struct async_gatt_comm_status_op *impl;
    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_GattCommunicationStatus_iface.lpVtbl = &async_gatt_comm_status_op_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_gatt_comm_status_info_vtbl;
    impl->ref = 1;
    impl->result = result;
    impl->status = Completed;
    *out = &impl->IAsyncOperation_GattCommunicationStatus_iface;
    return S_OK;
}

/* IGattDeviceService3 implementation */
static inline struct gatt_device_service *impl_from_IGattDeviceService3( IGattDeviceService3 *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_device_service, IGattDeviceService3_iface );
}

static HRESULT WINAPI gatt_service3_QueryInterface( IGattDeviceService3 *iface, REFIID iid, void **out )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService3( iface );
    return gatt_service_QueryInterface( &impl->IGattDeviceService_iface, iid, out );
}

static ULONG WINAPI gatt_service3_AddRef( IGattDeviceService3 *iface )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService3( iface );
    return gatt_service_AddRef( &impl->IGattDeviceService_iface );
}

static ULONG WINAPI gatt_service3_Release( IGattDeviceService3 *iface )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService3( iface );
    return gatt_service_Release( &impl->IGattDeviceService_iface );
}

static HRESULT WINAPI gatt_service3_GetIids( IGattDeviceService3 *iface, ULONG *iid_count, IID **iids )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService3( iface );
    return gatt_service_GetIids( &impl->IGattDeviceService_iface, iid_count, iids );
}

static HRESULT WINAPI gatt_service3_GetRuntimeClassName( IGattDeviceService3 *iface, HSTRING *class_name )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService3( iface );
    return gatt_service_GetRuntimeClassName( &impl->IGattDeviceService_iface, class_name );
}

static HRESULT WINAPI gatt_service3_GetTrustLevel( IGattDeviceService3 *iface, TrustLevel *level )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService3( iface );
    return gatt_service_GetTrustLevel( &impl->IGattDeviceService_iface, level );
}

static HRESULT WINAPI gatt_service3_get_DeviceAccessInformation( IGattDeviceService3 *iface, IDeviceAccessInformation **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return device_access_info_create( value );
}


static HRESULT WINAPI gatt_service3_get_Session( IGattDeviceService3 *iface, IGattSession **value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service3_get_SharingMode( IGattDeviceService3 *iface, GattSharingMode *value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    if (!value) return E_POINTER;
    *value = GattSharingMode_SharedReadAndWrite;
    return S_OK;
}

static HRESULT WINAPI gatt_service3_RequestAccessAsync( IGattDeviceService3 *iface,
                                                         IAsyncOperation_DeviceAccessStatus **async )
{
    TRACE( "(%p, %p)\n", iface, async );
    return async_device_access_op_create( DeviceAccessStatus_Allowed, async );
}

static HRESULT WINAPI gatt_service3_OpenAsync( IGattDeviceService3 *iface, GattSharingMode mode,
                                                IAsyncOperation_GattOpenStatus **async )
{
    FIXME( "(%p, %d, %p): stub!\n", iface, mode, async );
    if (!async) return E_POINTER;
    *async = NULL;
    return E_NOTIMPL;
}

static HRESULT get_characteristics_async_helper( IGattDeviceService3 *iface,
                                                 BluetoothCacheMode mode,
                                                 const GUID *filter_uuid,
                                                 IAsyncOperation_GattCharacteristicsResult **operation )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService3( iface );
    struct winebth_le_device_get_gatt_characteristics_params *params = NULL;
    struct winebth_radio_get_le_device_gatt_characteristics_params *radio_params = NULL;
    IVectorView_GattCharacteristic *chars_vector = NULL;
    IGattCharacteristicsResult *result;
    GattCommunicationStatus status;
    HANDLE radio_handle = INVALID_HANDLE_VALUE;
    BOOL use_radio_fallback = FALSE;
    BOOL owns_radio_handle = FALSE;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    HRESULT hr;
    int retry;

    TRACE( "(%p, %d, %s, %p)\n", iface, mode, debugstr_guid(filter_uuid), operation );
    if (!operation) return E_POINTER;

    if (impl->cached_chars && impl->cached_chars_count > 0)
    {
        UINT32 i, match_count = 0;
        IGattCharacteristic **matches = NULL;
        IGattCharacteristicsResult *cached_result;

        EnterCriticalSection( &impl->cache_cs );

        if (filter_uuid)
        {
            for (i = 0; i < impl->cached_chars_count; i++)
            {
                if (impl->cached_chars[i])
                {
                    GUID char_uuid;
                    impl->cached_chars[i]->lpVtbl->get_Uuid( impl->cached_chars[i], &char_uuid );
                    if (IsEqualGUID( &char_uuid, filter_uuid ))
                        match_count++;
                }
            }
            if (match_count > 0)
            {
                UINT32 idx = 0;
                matches = malloc( match_count * sizeof(IGattCharacteristic*) );
                if (matches)
                {
                    for (i = 0; i < impl->cached_chars_count; i++)
                    {
                        if (impl->cached_chars[i])
                        {
                            GUID char_uuid;
                            impl->cached_chars[i]->lpVtbl->get_Uuid( impl->cached_chars[i], &char_uuid );
                            if (IsEqualGUID( &char_uuid, filter_uuid ))
                            {
                                matches[idx++] = impl->cached_chars[i];
                                IUnknown_AddRef( (IUnknown *)impl->cached_chars[i] );
                            }
                        }
                    }
                }
            }
        }
        else
        {
            match_count = impl->cached_chars_count;
            matches = malloc( match_count * sizeof(IGattCharacteristic*) );
            if (matches)
            {
                for (i = 0; i < impl->cached_chars_count; i++)
                {
                    matches[i] = impl->cached_chars[i];
                    if (matches[i])
                        IUnknown_AddRef( (IUnknown *)matches[i] );
                }
            }
        }
        LeaveCriticalSection( &impl->cache_cs );

        if (matches && match_count > 0)
        {
            IVectorView_GattCharacteristic *cached_vector;
            hr = gatt_cached_chars_vector_create( matches, match_count, &cached_vector );
            if (SUCCEEDED(hr))
            {
                hr = gatt_characteristics_result_create( GattCommunicationStatus_Success, cached_vector, &cached_result );
                IVectorView_GattCharacteristic_Release( cached_vector );
                if (SUCCEEDED(hr))
                {
                    hr = async_gatt_chars_op_create( cached_result, operation );
                    IGattCharacteristicsResult_Release( cached_result );
                    return hr;
                }
            }
            for (i = 0; i < match_count; i++)
                if (matches[i]) IUnknown_Release( (IUnknown *)matches[i] );
            free( matches );
        }
        else if (matches)
        {
            free( matches );
        }
    }

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        radio_handle = open_first_radio();
        if (radio_handle == INVALID_HANDLE_VALUE)
        {
            status = GattCommunicationStatus_Unreachable;
            hr = gatt_characteristics_result_create( status, NULL, &result );
            if (FAILED( hr )) return hr;
            hr = async_gatt_chars_op_create( result, operation );
            IGattCharacteristicsResult_Release( result );
            return hr;
        }
        use_radio_fallback = TRUE;
        owns_radio_handle = TRUE;
    }
    else if (impl->is_radio_handle)
    {
        radio_handle = impl->device_handle;
        use_radio_fallback = TRUE;
        owns_radio_handle = FALSE;
    }

    if (use_radio_fallback)
    {
        buffer_size = offsetof( struct winebth_radio_get_le_device_gatt_characteristics_params, characteristics[32] );
        radio_params = malloc( buffer_size );
        if (!radio_params)
        {
            if (owns_radio_handle) CloseHandle( radio_handle );
            return E_OUTOFMEMORY;
        }
    }
    else
    {
        buffer_size = offsetof( struct winebth_le_device_get_gatt_characteristics_params, characteristics[32] );
        params = malloc( buffer_size );
        if (!params) return E_OUTOFMEMORY;
    }

    status = GattCommunicationStatus_Unreachable;
    for (retry = 0; retry < 10; retry++)
    {
        if (use_radio_fallback)
        {
            memset( radio_params, 0, buffer_size );
            radio_params->address = impl->device_address;
            radio_params->service = impl->service_info;
            radio_params->count = 0;
        }
        else
        {
            memset( params, 0, buffer_size );
            params->service = impl->service_info;
            params->count = 0;
        }

        if (use_radio_fallback
            ? DeviceIoControl( radio_handle, IOCTL_WINEBTH_RADIO_GET_LE_DEVICE_GATT_CHARACTERISTICS,
                               radio_params, buffer_size, radio_params, buffer_size, &bytes_returned, NULL )
            : DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_CHARACTERISTICS,
                               params, sizeof(*params), params, buffer_size, &bytes_returned, NULL ))
        {
            ULONG char_count = use_radio_fallback ? radio_params->count : params->count;
            BTH_LE_GATT_CHARACTERISTIC *characteristics = use_radio_fallback ? radio_params->characteristics : params->characteristics;
            HANDLE handle_for_chars = use_radio_fallback ? radio_handle : impl->device_handle;

            status = GattCommunicationStatus_Success;
            if (char_count > 0)
            {
                hr = gatt_characteristics_vector_create( characteristics, char_count,
                                                          &impl->service_info, handle_for_chars,
                                                          use_radio_fallback, impl->device_address,
                                                          filter_uuid,
                                                          &chars_vector );
                if (FAILED( hr ))
                {
                    free( params );
                    free( radio_params );
                    if (owns_radio_handle) CloseHandle( radio_handle );
                    return hr;
                }

                /* Only cache if we did a FULL query (no filter) */
                if (!filter_uuid)
                {
                    EnterCriticalSection( &impl->cache_cs );
                    if (!impl->cached_chars)
                    {
                        UINT32 i_idx;
                        UINT32 vec_size = 0;
                        IVectorView_GattCharacteristic_get_Size( chars_vector, &vec_size );
                        
                        impl->cached_chars = calloc( vec_size, sizeof(IGattCharacteristic*) );
                        if (impl->cached_chars)
                        {
                            for (i_idx = 0; i_idx < vec_size; i_idx++)
                            {
                                IGattCharacteristic *chr = NULL;
                                if (SUCCEEDED( IVectorView_GattCharacteristic_GetAt( chars_vector, i_idx, &chr ) ))
                                {
                                    impl->cached_chars[i_idx] = chr;
                                    impl->cached_chars_count = i_idx + 1;
                                }
                            }
                        }
                    }
                    LeaveCriticalSection( &impl->cache_cs );
                }

                break;
            }
            Sleep( 50 );
        }
        else
        {
            Sleep( 50 );
        }
    }

cleanup:
    free( params );
    free( radio_params );
    if (owns_radio_handle) CloseHandle( radio_handle );

    hr = gatt_characteristics_result_create( status, chars_vector, &result );
    if (chars_vector)
        IVectorView_GattCharacteristic_Release( chars_vector );
    if (FAILED( hr )) return hr;

    hr = async_gatt_chars_op_create( result, operation );
    IGattCharacteristicsResult_Release( result );
    return hr;
}

static HRESULT WINAPI gatt_service3_GetCharacteristicsWithCacheModeAsync( IGattDeviceService3 *iface,
                                                                           BluetoothCacheMode mode,
                                                                           IAsyncOperation_GattCharacteristicsResult **operation )
{
    return get_characteristics_async_helper( iface, mode, NULL, operation );
}

static HRESULT WINAPI gatt_service3_GetCharacteristicsAsync( IGattDeviceService3 *iface,
                                                              IAsyncOperation_GattCharacteristicsResult **async )
{
    TRACE( "(%p, %p)\n", iface, async );
    return gatt_service3_GetCharacteristicsWithCacheModeAsync( iface, BluetoothCacheMode_Cached, async );
}

static HRESULT WINAPI gatt_service3_GetCharacteristicsForUuidAsync( IGattDeviceService3 *iface, GUID uuid,
                                                                     IAsyncOperation_GattCharacteristicsResult **async )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( &uuid ), async );
    return get_characteristics_async_helper( iface, BluetoothCacheMode_Cached, &uuid, async );
}

static HRESULT WINAPI gatt_service3_GetCharacteristicsForUuidWithCacheModeAsync( IGattDeviceService3 *iface,
                                                                                  GUID uuid, BluetoothCacheMode mode,
                                                                                  IAsyncOperation_GattCharacteristicsResult **async )
{
    TRACE( "(%p, %s, %d, %p)\n", iface, debugstr_guid( &uuid ), mode, async );
    return get_characteristics_async_helper( iface, mode, &uuid, async );
}

static HRESULT WINAPI gatt_service3_GetIncludedServicesAsync( IGattDeviceService3 *iface,
                                                               IAsyncOperation_GattDeviceServicesResult **async )
{
    FIXME( "(%p, %p): stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service3_GetIncludedServicesWithCacheModeAsync( IGattDeviceService3 *iface,
                                                                            BluetoothCacheMode mode,
                                                                            IAsyncOperation_GattDeviceServicesResult **async )
{
    FIXME( "(%p, %d, %p): stub!\n", iface, mode, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service3_GetIncludedServicesForUuidAsync( IGattDeviceService3 *iface, GUID uuid,
                                                                      IAsyncOperation_GattDeviceServicesResult **async )
{
    FIXME( "(%p, %s, %p): stub!\n", iface, debugstr_guid( &uuid ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service3_GetIncludedServicesForUuidWithCacheModeAsync( IGattDeviceService3 *iface,
                                                                                   GUID uuid, BluetoothCacheMode mode,
                                                                                   IAsyncOperation_GattDeviceServicesResult **async )
{
    FIXME( "(%p, %s, %d, %p): stub!\n", iface, debugstr_guid( &uuid ), mode, async );
    return E_NOTIMPL;
}

static const IGattDeviceService3Vtbl gatt_device_service3_vtbl =
{
    gatt_service3_QueryInterface,
    gatt_service3_AddRef,
    gatt_service3_Release,
    gatt_service3_GetIids,
    gatt_service3_GetRuntimeClassName,
    gatt_service3_GetTrustLevel,
    gatt_service3_get_DeviceAccessInformation,
    gatt_service3_get_Session,
    gatt_service3_get_SharingMode,
    gatt_service3_RequestAccessAsync,
    gatt_service3_OpenAsync,
    gatt_service3_GetCharacteristicsAsync,
    gatt_service3_GetCharacteristicsWithCacheModeAsync,
    gatt_service3_GetCharacteristicsForUuidAsync,
    gatt_service3_GetCharacteristicsForUuidWithCacheModeAsync,
    gatt_service3_GetIncludedServicesAsync,
    gatt_service3_GetIncludedServicesWithCacheModeAsync,
    gatt_service3_GetIncludedServicesForUuidAsync,
    gatt_service3_GetIncludedServicesForUuidWithCacheModeAsync
};

static HRESULT gatt_device_service_create( HANDLE device_handle, BOOL is_radio_handle,
                                            const BTH_LE_GATT_SERVICE *service_info,
                                            const WCHAR *device_id, UINT64 device_address,
                                            IGattDeviceService **out )
{
    struct gatt_device_service *impl;
    HANDLE dup_handle = INVALID_HANDLE_VALUE;

    if (device_handle != INVALID_HANDLE_VALUE)
    {
        if (!DuplicateHandle( GetCurrentProcess(), device_handle,
                              GetCurrentProcess(), &dup_handle,
                              0, FALSE, DUPLICATE_SAME_ACCESS ))
        {
            WARN( "Failed to duplicate device handle: %lu\n", GetLastError() );
            return HRESULT_FROM_WIN32( GetLastError() );
        }
    }

    if (!(impl = calloc( 1, sizeof( *impl ) )))
    {
        if (dup_handle != INVALID_HANDLE_VALUE) CloseHandle( dup_handle );
        return E_OUTOFMEMORY;
    }
    impl->IGattDeviceService_iface.lpVtbl = &gatt_device_service_vtbl;
    impl->IGattDeviceService3_iface.lpVtbl = &gatt_device_service3_vtbl;
    impl->IClosable_iface.lpVtbl = &service_closable_vtbl;
    impl->ref = 1;
    impl->device_handle = dup_handle;
    impl->is_radio_handle = is_radio_handle;
    impl->service_info = *service_info;
    impl->device_address = device_address;
    impl->cached_chars = NULL;
    impl->cached_chars_count = 0;
    InitializeCriticalSection( &impl->cache_cs );
    if (device_id)
    {
        impl->device_id = wcsdup( device_id );
        if (!impl->device_id)
        {
            free( impl );
            return E_OUTOFMEMORY;
        }
    }
    *out = &impl->IGattDeviceService_iface;
    return S_OK;
}

/* IIterator_GattDeviceService implementation */
struct gatt_services_iterator
{
    IIterator_GattDeviceService IIterator_GattDeviceService_iface;
    LONG ref;
    IVectorView_GattDeviceService *view;
    UINT32 index;
    UINT32 size;
};

static inline struct gatt_services_iterator *impl_from_IIterator_GattDeviceService( IIterator_GattDeviceService *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_services_iterator, IIterator_GattDeviceService_iface );
}

static HRESULT WINAPI gatt_services_iterator_QueryInterface( IIterator_GattDeviceService *iface, REFIID iid, void **out )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );
    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IIterator_GattDeviceService ))
    {
        IIterator_GattDeviceService_AddRef( (*out = iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_services_iterator_AddRef( IIterator_GattDeviceService *iface )
{
    struct gatt_services_iterator *impl = impl_from_IIterator_GattDeviceService( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_services_iterator_Release( IIterator_GattDeviceService *iface )
{
    struct gatt_services_iterator *impl = impl_from_IIterator_GattDeviceService( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        IVectorView_GattDeviceService_Release( impl->view );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_services_iterator_GetIids( IIterator_GattDeviceService *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_services_iterator_GetRuntimeClassName( IIterator_GattDeviceService *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_services_iterator_GetTrustLevel( IIterator_GattDeviceService *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_services_iterator_get_Current( IIterator_GattDeviceService *iface, IGattDeviceService **value )
{
    struct gatt_services_iterator *impl = impl_from_IIterator_GattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    return IVectorView_GattDeviceService_GetAt( impl->view, impl->index, value );
}

static HRESULT WINAPI gatt_services_iterator_get_HasCurrent( IIterator_GattDeviceService *iface, boolean *value )
{
    struct gatt_services_iterator *impl = impl_from_IIterator_GattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->index < impl->size;
    return S_OK;
}

static HRESULT WINAPI gatt_services_iterator_MoveNext( IIterator_GattDeviceService *iface, boolean *value )
{
    struct gatt_services_iterator *impl = impl_from_IIterator_GattDeviceService( iface );
    HRESULT hr;
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->index < impl->size) impl->index++;
    hr = gatt_services_iterator_get_HasCurrent( iface, value );
    return hr;
}

static HRESULT WINAPI gatt_services_iterator_GetMany( IIterator_GattDeviceService *iface, UINT32 items_size,
                                                       IGattDeviceService **items, UINT32 *count )
{
    struct gatt_services_iterator *impl = impl_from_IIterator_GattDeviceService( iface );
    TRACE( "(%p, %u, %p, %p)\n", iface, items_size, items, count );
    return IVectorView_GattDeviceService_GetMany( impl->view, impl->index, items_size, items, count );
}

static const IIterator_GattDeviceServiceVtbl gatt_services_iterator_vtbl =
{
    gatt_services_iterator_QueryInterface,
    gatt_services_iterator_AddRef,
    gatt_services_iterator_Release,
    gatt_services_iterator_GetIids,
    gatt_services_iterator_GetRuntimeClassName,
    gatt_services_iterator_GetTrustLevel,
    gatt_services_iterator_get_Current,
    gatt_services_iterator_get_HasCurrent,
    gatt_services_iterator_MoveNext,
    gatt_services_iterator_GetMany,
};

struct gatt_services_vector
{
    IVectorView_GattDeviceService IVectorView_GattDeviceService_iface;
    IIterable_GattDeviceService IIterable_GattDeviceService_iface;
    LONG ref;
    ULONG count;
    BTH_LE_GATT_SERVICE *services;
    HANDLE device_handle;
    BOOL is_radio_handle;
    WCHAR *device_id;
    UINT64 device_address;
};

static inline struct gatt_services_vector *impl_from_IVectorView_GattDeviceService( IVectorView_GattDeviceService *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_services_vector, IVectorView_GattDeviceService_iface );
}

static inline struct gatt_services_vector *impl_from_IIterable_GattDeviceService( IIterable_GattDeviceService *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_services_vector, IIterable_GattDeviceService_iface );
}

static HRESULT WINAPI gatt_services_iterable_QueryInterface( IIterable_GattDeviceService *iface, REFIID iid, void **out )
{
    struct gatt_services_vector *impl = impl_from_IIterable_GattDeviceService( iface );
    return IInspectable_QueryInterface( (IInspectable *)&impl->IVectorView_GattDeviceService_iface, iid, out );
}

static ULONG WINAPI gatt_services_iterable_AddRef( IIterable_GattDeviceService *iface )
{
    struct gatt_services_vector *impl = impl_from_IIterable_GattDeviceService( iface );
    return IVectorView_GattDeviceService_AddRef( &impl->IVectorView_GattDeviceService_iface );
}

static ULONG WINAPI gatt_services_iterable_Release( IIterable_GattDeviceService *iface )
{
    struct gatt_services_vector *impl = impl_from_IIterable_GattDeviceService( iface );
    return IVectorView_GattDeviceService_Release( &impl->IVectorView_GattDeviceService_iface );
}

static HRESULT WINAPI gatt_services_iterable_GetIids( IIterable_GattDeviceService *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_services_iterable_GetRuntimeClassName( IIterable_GattDeviceService *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_services_iterable_GetTrustLevel( IIterable_GattDeviceService *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_services_iterable_First( IIterable_GattDeviceService *iface, IIterator_GattDeviceService **value )
{
    struct gatt_services_vector *impl = impl_from_IIterable_GattDeviceService( iface );
    struct gatt_services_iterator *iter;
    TRACE( "(%p, %p)\n", iface, value );

    if (!value) return E_POINTER;
    if (!(iter = calloc( 1, sizeof( *iter ) ))) return E_OUTOFMEMORY;
    iter->IIterator_GattDeviceService_iface.lpVtbl = &gatt_services_iterator_vtbl;
    iter->ref = 1;
    IVectorView_GattDeviceService_AddRef( (iter->view = &impl->IVectorView_GattDeviceService_iface) );
    iter->size = impl->count;
    iter->index = 0;
    *value = &iter->IIterator_GattDeviceService_iface;

    return S_OK;
}

static const IIterable_GattDeviceServiceVtbl gatt_services_iterable_vtbl =
{
    gatt_services_iterable_QueryInterface,
    gatt_services_iterable_AddRef,
    gatt_services_iterable_Release,
    gatt_services_iterable_GetIids,
    gatt_services_iterable_GetRuntimeClassName,
    gatt_services_iterable_GetTrustLevel,
    gatt_services_iterable_First,
};

static HRESULT WINAPI gatt_services_vector_QueryInterface( IVectorView_GattDeviceService *iface, REFIID iid, void **out )
{
    struct gatt_services_vector *impl = impl_from_IVectorView_GattDeviceService( iface );
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IVectorView_GattDeviceService ))
    {
        IVectorView_GattDeviceService_AddRef( (*out = &impl->IVectorView_GattDeviceService_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IIterable_GattDeviceService ))
    {
        IIterable_GattDeviceService_AddRef( (*out = &impl->IIterable_GattDeviceService_iface) );
        return S_OK;
    }
    *out = NULL;
    FIXME( "%s not implemented\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_services_vector_AddRef( IVectorView_GattDeviceService *iface )
{
    struct gatt_services_vector *impl = impl_from_IVectorView_GattDeviceService( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_services_vector_Release( IVectorView_GattDeviceService *iface )
{
    struct gatt_services_vector *impl = impl_from_IVectorView_GattDeviceService( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->device_handle != INVALID_HANDLE_VALUE)
            CloseHandle( impl->device_handle );
        free( impl->services );
        free( impl->device_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_services_vector_GetIids( IVectorView_GattDeviceService *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_services_vector_GetRuntimeClassName( IVectorView_GattDeviceService *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_services_vector_GetTrustLevel( IVectorView_GattDeviceService *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_services_vector_GetAt( IVectorView_GattDeviceService *iface, UINT32 index, IGattDeviceService **value )
{
    struct gatt_services_vector *impl = impl_from_IVectorView_GattDeviceService( iface );
    HRESULT hr;
    TRACE( "(%p, %u, %p)\n", iface, index, value );
    if (!value) return E_POINTER;
    if (index >= impl->count)
        return E_BOUNDS;
    hr = gatt_device_service_create( impl->device_handle, impl->is_radio_handle, &impl->services[index], impl->device_id, impl->device_address, value );
    return hr;
}

static HRESULT WINAPI gatt_services_vector_get_Size( IVectorView_GattDeviceService *iface, UINT32 *value )
{
    struct gatt_services_vector *impl = impl_from_IVectorView_GattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->count;
    return S_OK;
}

static HRESULT WINAPI gatt_services_vector_IndexOf( IVectorView_GattDeviceService *iface, IGattDeviceService *element,
                                                    UINT32 *index, BOOLEAN *found )
{
    FIXME( "(%p, %p, %p, %p): stub!\n", iface, element, index, found );
    if (!index || !found) return E_POINTER;
    *found = FALSE;
    return S_OK;
}

static HRESULT WINAPI gatt_services_vector_GetMany( IVectorView_GattDeviceService *iface, UINT32 start_index,
                                                    UINT32 items_size, IGattDeviceService **items, UINT32 *value )
{
    struct gatt_services_vector *impl = impl_from_IVectorView_GattDeviceService( iface );
    UINT32 i, available, to_copy;
    HRESULT hr;

    TRACE( "(%p, %u, %u, %p, %p)\n", iface, start_index, items_size, items, value );
    if (!value) return E_POINTER;
    if (!items) return E_POINTER;

    if (start_index >= impl->count)
    {
        *value = 0;
        return S_OK;
    }

    available = impl->count - start_index;
    to_copy = (items_size < available) ? items_size : available;

    for (i = 0; i < to_copy; i++)
    {
        hr = gatt_device_service_create( impl->device_handle, impl->is_radio_handle, &impl->services[start_index + i], impl->device_id, impl->device_address, &items[i] );
        if (FAILED( hr ))
        {
            while (i > 0)
            {
                struct gatt_device_service *svc_impl = impl_from_IGattDeviceService( items[i - 1] );
                svc_impl->IGattDeviceService_iface.lpVtbl->Release( items[i - 1] );
                i--;
            }
            *value = 0;
            return hr;
        }
    }

    *value = to_copy;
    return S_OK;
}

static const struct IVectorView_GattDeviceServiceVtbl gatt_services_vector_vtbl =
{
    gatt_services_vector_QueryInterface,
    gatt_services_vector_AddRef,
    gatt_services_vector_Release,
    gatt_services_vector_GetIids,
    gatt_services_vector_GetRuntimeClassName,
    gatt_services_vector_GetTrustLevel,
    gatt_services_vector_GetAt,
    gatt_services_vector_get_Size,
    gatt_services_vector_IndexOf,
    gatt_services_vector_GetMany
};

static HRESULT gatt_services_vector_create( BTH_LE_GATT_SERVICE *services, ULONG count,
                                             HANDLE device_handle, BOOL is_radio_handle,
                                             const WCHAR *device_id, UINT64 device_address,
                                             IVectorView_GattDeviceService **out )
{
    struct gatt_services_vector *impl;
    HANDLE dup_handle = INVALID_HANDLE_VALUE;

    TRACE( "gatt_services_vector_create: count=%lu is_radio=%d device_handle=%p \n", count, is_radio_handle, device_handle );

    if (device_handle != INVALID_HANDLE_VALUE)
    {
        if (!DuplicateHandle( GetCurrentProcess(), device_handle,
                              GetCurrentProcess(), &dup_handle,
                              0, FALSE, DUPLICATE_SAME_ACCESS ))
        {
            WARN( "Failed to duplicate device handle: %lu\n", GetLastError() );
            return HRESULT_FROM_WIN32( GetLastError() );
        }
    }

    if (!(impl = calloc( 1, sizeof( *impl ) )))
    {
        if (dup_handle != INVALID_HANDLE_VALUE) CloseHandle( dup_handle );
        return E_OUTOFMEMORY;
    }
    impl->IVectorView_GattDeviceService_iface.lpVtbl = &gatt_services_vector_vtbl;
    impl->IIterable_GattDeviceService_iface.lpVtbl = &gatt_services_iterable_vtbl;
    impl->ref = 1;
    impl->count = count;
    impl->device_handle = dup_handle;
    impl->is_radio_handle = is_radio_handle;
    impl->device_address = device_address;
    if (device_id)
    {
        impl->device_id = wcsdup( device_id );
        if (!impl->device_id)
        {
            free( impl );
            return E_OUTOFMEMORY;
        }
    }
    if (count > 0)
    {
        impl->services = malloc( count * sizeof( BTH_LE_GATT_SERVICE ) );
        if (!impl->services)
        {
            free( impl->device_id );
            free( impl );
            return E_OUTOFMEMORY;
        }
        memcpy( impl->services, services, count * sizeof( BTH_LE_GATT_SERVICE ) );
    }
    *out = &impl->IVectorView_GattDeviceService_iface;
    return S_OK;
}

struct gatt_services_result
{
    IGattDeviceServicesResult IGattDeviceServicesResult_iface;
    LONG ref;
    GattCommunicationStatus status;
    IVectorView_GattDeviceService *services_vector;
    BOOL services_accessed;  /* Track if get_Services was called */
};

static inline struct gatt_services_result *impl_from_IGattDeviceServicesResult( IGattDeviceServicesResult *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_services_result, IGattDeviceServicesResult_iface );
}

static HRESULT WINAPI gatt_result_QueryInterface( IGattDeviceServicesResult *iface, REFIID iid, void **out )
{
    struct gatt_services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGattDeviceServicesResult ))
    {
        IGattDeviceServicesResult_AddRef( (*out = &impl->IGattDeviceServicesResult_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_result_AddRef( IGattDeviceServicesResult *iface )
{
    struct gatt_services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_result_Release( IGattDeviceServicesResult *iface )
{
    struct gatt_services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->services_vector)
            IVectorView_GattDeviceService_Release( impl->services_vector );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_result_GetIids( IGattDeviceServicesResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): semi-stub!\n", iface, iid_count, iids );

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = 1;
    *iids = CoTaskMemAlloc( sizeof(IID) );
    if (!*iids) return E_OUTOFMEMORY;

    (*iids)[0] = IID_IGattDeviceServicesResult;
    return S_OK;
}

static HRESULT WINAPI gatt_result_GetRuntimeClassName( IGattDeviceServicesResult *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI gatt_result_GetTrustLevel( IGattDeviceServicesResult *iface, TrustLevel *level )
{
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_result_get_Status( IGattDeviceServicesResult *iface, GattCommunicationStatus *value )
{
    struct gatt_services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->status;
    return S_OK;
}

static HRESULT WINAPI gatt_result_get_ProtocolError( IGattDeviceServicesResult *iface, IReference_BYTE **value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return S_OK;
}

static HRESULT WINAPI gatt_result_get_Services( IGattDeviceServicesResult *iface, IVectorView_GattDeviceService **value )
{
    struct gatt_services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    TRACE( "(%p, %p)\n", iface, value );
    impl->services_accessed = TRUE;

    if (!value) return E_POINTER;
    if (impl->services_vector)
    {
        IVectorView_GattDeviceService_AddRef( impl->services_vector );
        *value = impl->services_vector;
    }
    else
    {
        *value = NULL;
    }
    return S_OK;
}

static const IGattDeviceServicesResultVtbl gatt_result_vtbl =
{
    gatt_result_QueryInterface,
    gatt_result_AddRef,
    gatt_result_Release,
    gatt_result_GetIids,
    gatt_result_GetRuntimeClassName,
    gatt_result_GetTrustLevel,
    gatt_result_get_Status,
    gatt_result_get_ProtocolError,
    gatt_result_get_Services
};

static HRESULT gatt_services_result_create( GattCommunicationStatus status, IVectorView_GattDeviceService *services_vector,
                                           IGattDeviceServicesResult **out )
{
    struct gatt_services_result *impl;
    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattDeviceServicesResult_iface.lpVtbl = &gatt_result_vtbl;
    impl->ref = 1;
    impl->status = status;
    impl->services_vector = services_vector;
    if (services_vector)
        IVectorView_GattDeviceService_AddRef( services_vector );
    *out = &impl->IGattDeviceServicesResult_iface;
    return S_OK;
}

struct async_gatt_services_op
{
    IAsyncOperation_GattDeviceServicesResult IAsyncOperation_GattDeviceServicesResult_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IAsyncOperationCompletedHandler_GattDeviceServicesResult *handler;
    IGattDeviceServicesResult *result;
    AsyncStatus status;
};

static inline struct async_gatt_services_op *impl_from_IAsyncOperation_GattDeviceServicesResult( IAsyncOperation_GattDeviceServicesResult *iface )
{
    return CONTAINING_RECORD( iface, struct async_gatt_services_op, IAsyncOperation_GattDeviceServicesResult_iface );
}

static HRESULT WINAPI async_gatt_op_QueryInterface( IAsyncOperation_GattDeviceServicesResult *iface, REFIID iid, void **out )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncOperation_GattDeviceServicesResult( iface );
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_GattDeviceServicesResult ))
    {
        IAsyncOperation_GattDeviceServicesResult_AddRef( (*out = &impl->IAsyncOperation_GattDeviceServicesResult_iface) );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IAsyncInfo_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI async_gatt_op_AddRef( IAsyncOperation_GattDeviceServicesResult *iface )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncOperation_GattDeviceServicesResult( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_gatt_op_Release( IAsyncOperation_GattDeviceServicesResult *iface )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncOperation_GattDeviceServicesResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->handler) IAsyncOperationCompletedHandler_GattDeviceServicesResult_Release( impl->handler );
        if (impl->result) IGattDeviceServicesResult_Release( impl->result );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_gatt_op_GetIids( IAsyncOperation_GattDeviceServicesResult *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): semi-stub!\n", iface, iid_count, iids );

    if (!iid_count || !iids) return E_POINTER;

    *iid_count = 1;
    *iids = CoTaskMemAlloc( sizeof(IID) );
    if (!*iids) return E_OUTOFMEMORY;

    (*iids)[0] = IID_IAsyncOperation_GattDeviceServicesResult;
    return S_OK;
}

static HRESULT WINAPI async_gatt_op_GetRuntimeClassName( IAsyncOperation_GattDeviceServicesResult *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Foundation.IAsyncOperation`1<Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult>";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI async_gatt_op_GetTrustLevel( IAsyncOperation_GattDeviceServicesResult *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI async_gatt_op_put_Completed( IAsyncOperation_GattDeviceServicesResult *iface,
                                                    IAsyncOperationCompletedHandler_GattDeviceServicesResult *handler )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncOperation_GattDeviceServicesResult( iface );
    AsyncStatus current_status;

    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;

    impl->handler = handler;
    IAsyncOperationCompletedHandler_GattDeviceServicesResult_AddRef( handler );

    current_status = impl->status;
    if (current_status != Started)
        IAsyncOperationCompletedHandler_GattDeviceServicesResult_Invoke( handler, iface, current_status );

    return S_OK;
}

static HRESULT WINAPI async_gatt_op_get_Completed( IAsyncOperation_GattDeviceServicesResult *iface,
                                                    IAsyncOperationCompletedHandler_GattDeviceServicesResult **handler )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncOperation_GattDeviceServicesResult( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;
    *handler = impl->handler;
    if (*handler) IAsyncOperationCompletedHandler_GattDeviceServicesResult_AddRef( *handler );
    return S_OK;
}

static HRESULT WINAPI async_gatt_op_GetResults( IAsyncOperation_GattDeviceServicesResult *iface, IGattDeviceServicesResult **results )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncOperation_GattDeviceServicesResult( iface );
    TRACE( "(%p, %p)\n", iface, results );

    if (!results) return E_POINTER;
    *results = impl->result;
    if (*results)
        IGattDeviceServicesResult_AddRef( *results );

    return S_OK;
}

static const IAsyncOperation_GattDeviceServicesResultVtbl async_gatt_op_vtbl =
{
    async_gatt_op_QueryInterface,
    async_gatt_op_AddRef,
    async_gatt_op_Release,
    async_gatt_op_GetIids,
    async_gatt_op_GetRuntimeClassName,
    async_gatt_op_GetTrustLevel,
    async_gatt_op_put_Completed,
    async_gatt_op_get_Completed,
    async_gatt_op_GetResults
};

static inline struct async_gatt_services_op *impl_from_IAsyncInfo_gatt( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct async_gatt_services_op, IAsyncInfo_iface );
}

static HRESULT WINAPI async_gatt_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncInfo_gatt( iface );
    return IAsyncOperation_GattDeviceServicesResult_QueryInterface( &impl->IAsyncOperation_GattDeviceServicesResult_iface, iid, out );
}

static ULONG WINAPI async_gatt_info_AddRef( IAsyncInfo *iface )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncInfo_gatt( iface );
    return IAsyncOperation_GattDeviceServicesResult_AddRef( &impl->IAsyncOperation_GattDeviceServicesResult_iface );
}

static ULONG WINAPI async_gatt_info_Release( IAsyncInfo *iface )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncInfo_gatt( iface );
    return IAsyncOperation_GattDeviceServicesResult_Release( &impl->IAsyncOperation_GattDeviceServicesResult_iface );
}

static HRESULT WINAPI async_gatt_info_GetIids( IAsyncInfo *iface, ULONG *iid_count, IID **iids )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncInfo_gatt( iface );
    return IAsyncOperation_GattDeviceServicesResult_GetIids( &impl->IAsyncOperation_GattDeviceServicesResult_iface, iid_count, iids );
}

static HRESULT WINAPI async_gatt_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *class_name )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncInfo_gatt( iface );
    return IAsyncOperation_GattDeviceServicesResult_GetRuntimeClassName( &impl->IAsyncOperation_GattDeviceServicesResult_iface, class_name );
}

static HRESULT WINAPI async_gatt_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncInfo_gatt( iface );
    return IAsyncOperation_GattDeviceServicesResult_GetTrustLevel( &impl->IAsyncOperation_GattDeviceServicesResult_iface, level );
}

static HRESULT WINAPI async_gatt_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI async_gatt_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_gatt_services_op *impl = impl_from_IAsyncInfo_gatt( iface );
    *status = impl->status;
    return S_OK;
}

static HRESULT WINAPI async_gatt_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    *error_code = S_OK;
    return S_OK;
}

static HRESULT WINAPI async_gatt_info_Cancel( IAsyncInfo *iface )
{
    return S_OK;
}

static HRESULT WINAPI async_gatt_info_Close( IAsyncInfo *iface )
{
    return S_OK;
}

static const IAsyncInfoVtbl async_gatt_info_vtbl =
{
    async_gatt_info_QueryInterface,
    async_gatt_info_AddRef,
    async_gatt_info_Release,
    async_gatt_info_GetIids,
    async_gatt_info_GetRuntimeClassName,
    async_gatt_info_GetTrustLevel,
    async_gatt_info_get_Id,
    async_gatt_info_get_Status,
    async_gatt_info_get_ErrorCode,
    async_gatt_info_Cancel,
    async_gatt_info_Close
};

static HRESULT async_gatt_services_op_create( IGattDeviceServicesResult *result, IAsyncOperation_GattDeviceServicesResult **out )
{
    struct async_gatt_services_op *impl;
    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_GattDeviceServicesResult_iface.lpVtbl = &async_gatt_op_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_gatt_info_vtbl;
    impl->ref = 1;
    impl->result = result;
    impl->status = Started;
    if (result) IGattDeviceServicesResult_AddRef( result );
    *out = &impl->IAsyncOperation_GattDeviceServicesResult_iface;
    return S_OK;
}

static inline struct bluetooth_le_device *impl_from_IBluetoothLEDevice3( IBluetoothLEDevice3 *iface )
{
    return CONTAINING_RECORD( iface, struct bluetooth_le_device, IBluetoothLEDevice3_iface );
}

static HRESULT WINAPI le_device3_QueryInterface( IBluetoothLEDevice3 *iface, REFIID iid, void **out )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice3( iface );
    return le_device_QueryInterface( &impl->IBluetoothLEDevice_iface, iid, out );
}

static ULONG WINAPI le_device3_AddRef( IBluetoothLEDevice3 *iface )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice3( iface );
    return le_device_AddRef( &impl->IBluetoothLEDevice_iface );
}

static ULONG WINAPI le_device3_Release( IBluetoothLEDevice3 *iface )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice3( iface );
    return le_device_Release( &impl->IBluetoothLEDevice_iface );
}

static HRESULT WINAPI le_device3_GetIids( IBluetoothLEDevice3 *iface, ULONG *iid_count, IID **iids )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice3( iface );
    return le_device_GetIids( &impl->IBluetoothLEDevice_iface, iid_count, iids );
}

static HRESULT WINAPI le_device3_GetRuntimeClassName( IBluetoothLEDevice3 *iface, HSTRING *class_name )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice3( iface );
    return le_device_GetRuntimeClassName( &impl->IBluetoothLEDevice_iface, class_name );
}

static HRESULT WINAPI le_device3_GetTrustLevel( IBluetoothLEDevice3 *iface, TrustLevel *level )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice3( iface );
    return le_device_GetTrustLevel( &impl->IBluetoothLEDevice_iface, level );
}

static HRESULT WINAPI le_device3_get_DeviceAccessInformation( IBluetoothLEDevice3 *iface, IInspectable **value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return device_access_info_create( (IDeviceAccessInformation **)value );
}




struct async_device_access_op
{
    IAsyncOperation_DeviceAccessStatus IAsyncOperation_DeviceAccessStatus_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IAsyncOperationCompletedHandler_DeviceAccessStatus *handler;
    __x_ABI_CWindows_CDevices_CEnumeration_CDeviceAccessStatus status;
};

static inline struct async_device_access_op *impl_from_IAsyncOperation_DeviceAccessStatus( IAsyncOperation_DeviceAccessStatus *iface )
{
    return CONTAINING_RECORD( iface, struct async_device_access_op, IAsyncOperation_DeviceAccessStatus_iface );
}

static HRESULT WINAPI async_device_access_op_QueryInterface( IAsyncOperation_DeviceAccessStatus *iface, REFIID iid, void **out )
{
    struct async_device_access_op *impl = impl_from_IAsyncOperation_DeviceAccessStatus( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_DeviceAccessStatus ))
    {
        IAsyncOperation_DeviceAccessStatus_AddRef( (*out = &impl->IAsyncOperation_DeviceAccessStatus_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IAsyncInfo_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI async_device_access_op_AddRef( IAsyncOperation_DeviceAccessStatus *iface )
{
    struct async_device_access_op *impl = impl_from_IAsyncOperation_DeviceAccessStatus( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_device_access_op_Release( IAsyncOperation_DeviceAccessStatus *iface )
{
    struct async_device_access_op *impl = impl_from_IAsyncOperation_DeviceAccessStatus( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );

    if (!ref)
    {
        if (impl->handler) IAsyncOperationCompletedHandler_DeviceAccessStatus_Release( impl->handler );
        free( impl );
    }

    return ref;
}

static HRESULT WINAPI async_device_access_op_GetIids( IAsyncOperation_DeviceAccessStatus *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_device_access_op_GetRuntimeClassName( IAsyncOperation_DeviceAccessStatus *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_device_access_op_GetTrustLevel( IAsyncOperation_DeviceAccessStatus *iface, TrustLevel *level )
{
    FIXME( "(%p, %p): stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI async_device_access_op_put_Completed( IAsyncOperation_DeviceAccessStatus *iface, IAsyncOperationCompletedHandler_DeviceAccessStatus *handler )
{
    struct async_device_access_op *impl = impl_from_IAsyncOperation_DeviceAccessStatus( iface );

    TRACE( "(%p, %p) status=%d\n", iface, handler, impl->status );

    if (!handler) return E_POINTER;
    if (impl->handler) return E_ILLEGAL_DELEGATE_ASSIGNMENT;

    IAsyncOperationCompletedHandler_DeviceAccessStatus_AddRef( handler );
    impl->handler = handler;

    /* Invoke immediately as it's always COMPLETED */
    IAsyncOperationCompletedHandler_DeviceAccessStatus_Invoke( handler, iface, Completed );

    return S_OK;
}

static HRESULT WINAPI async_device_access_op_get_Completed( IAsyncOperation_DeviceAccessStatus *iface, IAsyncOperationCompletedHandler_DeviceAccessStatus **handler )
{
    struct async_device_access_op *impl = impl_from_IAsyncOperation_DeviceAccessStatus( iface );

    TRACE( "(%p, %p)\n", iface, handler );

    if (!handler) return E_POINTER;
    *handler = impl->handler;
    if (*handler) ((IUnknown *)*handler)->lpVtbl->AddRef( (IUnknown *)*handler );
    return S_OK;
}

static HRESULT WINAPI async_device_access_op_GetResults( IAsyncOperation_DeviceAccessStatus *iface, __x_ABI_CWindows_CDevices_CEnumeration_CDeviceAccessStatus *results )
{
    struct async_device_access_op *impl = impl_from_IAsyncOperation_DeviceAccessStatus( iface );

    TRACE( "(%p, %p)\n", iface, results );
    
    if (!results) return E_POINTER;
    *results = impl->status;
    return S_OK;
}

static const IAsyncOperation_DeviceAccessStatusVtbl async_device_access_op_vtbl =
{
    async_device_access_op_QueryInterface,
    async_device_access_op_AddRef,
    async_device_access_op_Release,
    async_device_access_op_GetIids,
    async_device_access_op_GetRuntimeClassName,
    async_device_access_op_GetTrustLevel,
    async_device_access_op_put_Completed,
    async_device_access_op_get_Completed,
    async_device_access_op_GetResults
};

static HRESULT async_device_access_op_create( DeviceAccessStatus status, IAsyncOperation_DeviceAccessStatus **operation )
{
    struct async_device_access_op *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_DeviceAccessStatus_iface.lpVtbl = &async_device_access_op_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_device_access_info_vtbl;
    impl->ref = 1;
    impl->status = status;

    *operation = &impl->IAsyncOperation_DeviceAccessStatus_iface;
    return S_OK;
}

static inline struct async_device_access_op *impl_from_IAsyncInfo_device_access( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct async_device_access_op, IAsyncInfo_iface );
}

static HRESULT WINAPI async_device_access_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    struct async_device_access_op *impl = impl_from_IAsyncInfo_device_access( iface );
    return IAsyncOperation_DeviceAccessStatus_QueryInterface( &impl->IAsyncOperation_DeviceAccessStatus_iface, iid, out );
}

static ULONG WINAPI async_device_access_info_AddRef( IAsyncInfo *iface )
{
    struct async_device_access_op *impl = impl_from_IAsyncInfo_device_access( iface );
    return IAsyncOperation_DeviceAccessStatus_AddRef( &impl->IAsyncOperation_DeviceAccessStatus_iface );
}

static ULONG WINAPI async_device_access_info_Release( IAsyncInfo *iface )
{
    struct async_device_access_op *impl = impl_from_IAsyncInfo_device_access( iface );
    return IAsyncOperation_DeviceAccessStatus_Release( &impl->IAsyncOperation_DeviceAccessStatus_iface );
}

static HRESULT WINAPI async_device_access_info_GetIids( IAsyncInfo *iface, ULONG *iid_count, IID **iids )
{
    struct async_device_access_op *impl = impl_from_IAsyncInfo_device_access( iface );
    return IAsyncOperation_DeviceAccessStatus_GetIids( &impl->IAsyncOperation_DeviceAccessStatus_iface, iid_count, iids );
}

static HRESULT WINAPI async_device_access_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *class_name )
{
    struct async_device_access_op *impl = impl_from_IAsyncInfo_device_access( iface );
    return IAsyncOperation_DeviceAccessStatus_GetRuntimeClassName( &impl->IAsyncOperation_DeviceAccessStatus_iface, class_name );
}

static HRESULT WINAPI async_device_access_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    struct async_device_access_op *impl = impl_from_IAsyncInfo_device_access( iface );
    return IAsyncOperation_DeviceAccessStatus_GetTrustLevel( &impl->IAsyncOperation_DeviceAccessStatus_iface, level );
}

static HRESULT WINAPI async_device_access_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = 1; /* Dummy ID */
    return S_OK;
}

static HRESULT WINAPI async_device_access_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    *status = Completed;
    return S_OK;
}

static HRESULT WINAPI async_device_access_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    *error_code = S_OK;
    return S_OK;
}

static HRESULT WINAPI async_device_access_info_Cancel( IAsyncInfo *iface )
{
    return S_OK;
}

static HRESULT WINAPI async_device_access_info_Close( IAsyncInfo *iface )
{
    return S_OK;
}

static const IAsyncInfoVtbl async_device_access_info_vtbl =
{
    async_device_access_info_QueryInterface,
    async_device_access_info_AddRef,
    async_device_access_info_Release,
    async_device_access_info_GetIids,
    async_device_access_info_GetRuntimeClassName,
    async_device_access_info_GetTrustLevel,
    async_device_access_info_get_Id,
    async_device_access_info_get_Status,
    async_device_access_info_get_ErrorCode,
    async_device_access_info_Cancel,
    async_device_access_info_Close
};

static HRESULT WINAPI le_device3_RequestAccessAsync( IBluetoothLEDevice3 *iface, IAsyncOperation_DeviceAccessStatus **operation )
{
    TRACE( "(%p, %p)\n", iface, operation );
    return async_device_access_op_create( DeviceAccessStatus_Allowed, operation );
}

struct gatt_services_async_ctx
{
    IBluetoothLEDevice3 *iface;
    IAsyncOperation_GattDeviceServicesResult *op;
    BluetoothCacheMode mode;
    BOOL has_filter_uuid;
    GUID filter_uuid;
};

static DWORD WINAPI gatt_services_thread( void *arg )
{
    struct gatt_services_async_ctx *ctx = arg;
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice3( ctx->iface );
    struct async_gatt_services_op *op_impl = impl_from_IAsyncOperation_GattDeviceServicesResult( ctx->op );
    struct winebth_le_device_get_gatt_services_params *params = NULL;
    struct winebth_radio_get_le_device_gatt_services_params *radio_params = NULL;
    IVectorView_GattDeviceService *services_vector = NULL;
    IGattDeviceServicesResult *result;
    GattCommunicationStatus status = GattCommunicationStatus_Unreachable;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    HRESULT hr;
    int retries = 0;
    const int max_retries = 50;
    HANDLE radio_handle = INVALID_HANDLE_VALUE;
    BOOL use_radio_fallback = FALSE;

    CoInitializeEx( NULL, COINIT_MULTITHREADED );
    TRACE( "gatt_services_thread: started device=%p handle=%p address=%I64x\n", impl, impl->device_handle, impl->address );

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        TRACE( "gatt_services_thread: device handle invalid, trying radio fallback\n" );
        radio_handle = open_first_radio();
        if (radio_handle == INVALID_HANDLE_VALUE)
        {
            TRACE( "gatt_services_thread: failed to open radio for fallback\n" );
            goto done;
        }
        use_radio_fallback = TRUE;
    }

    if (use_radio_fallback)
    {
        buffer_size = offsetof( struct winebth_radio_get_le_device_gatt_services_params, services[16] );
        radio_params = malloc( buffer_size );
        if (!radio_params) goto done;
    }
    else
    {
        buffer_size = offsetof( struct winebth_le_device_get_gatt_services_params, services[16] );
        params = malloc( buffer_size );
        if (!params) goto done;
    }

retry:
    if (use_radio_fallback)
    {
        memset( radio_params, 0, buffer_size );
        radio_params->address = impl->address;
        radio_params->count = 0;
    }
    else
    {
        memset( params, 0, buffer_size );
        params->count = 0;
    }

    TRACE( "gatt_services_thread: calling IOCTL (use_radio_fallback=%d)\n", use_radio_fallback );

    if (use_radio_fallback
        ? DeviceIoControl( radio_handle, IOCTL_WINEBTH_RADIO_GET_LE_DEVICE_GATT_SERVICES,
                           radio_params, buffer_size, radio_params, buffer_size, &bytes_returned, NULL )
        : DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES, NULL, 0,
                           params, buffer_size, &bytes_returned, NULL ))
    {
        ULONG svc_count = use_radio_fallback ? radio_params->count : params->count;
        BTH_LE_GATT_SERVICE *services = use_radio_fallback ? radio_params->services : params->services;
        HANDLE handle_for_services = use_radio_fallback ? radio_handle : impl->device_handle;

        TRACE( "gatt_services_thread: IOCTL OK count=%lu mode=%d retry=%d filter=%d\n", svc_count, ctx->mode, retries, ctx->has_filter_uuid );
        status = GattCommunicationStatus_Success;

        if (ctx->mode == BluetoothCacheMode_Uncached && svc_count == 0 && retries < max_retries)
        {
             TRACE( "Uncached mode and 0 services, waiting... (retry %d/%d)\n", retries+1, max_retries );
             Sleep( 100 );
             retries++;
             goto retry;
        }

        if (svc_count > 0)
        {
            if (ctx->has_filter_uuid)
            {
                BTH_LE_GATT_SERVICE filtered_services[16];
                ULONG filtered_count = 0;
                ULONG i;
                TRACE( "gatt_services_thread: filtering for UUID %s\n", debugstr_guid( &ctx->filter_uuid ) );
                for (i = 0; i < svc_count && filtered_count < 16; i++)
                {
                    GUID svc_guid;
                    if (services[i].ServiceUuid.IsShortUuid)
                    {
                        memset( &svc_guid, 0, sizeof(svc_guid) );
                        svc_guid.Data1 = services[i].ServiceUuid.Value.ShortUuid;
                        svc_guid.Data4[4] = 0x00; svc_guid.Data4[5] = 0x80;
                        svc_guid.Data4[6] = 0x5f; svc_guid.Data4[7] = 0x9b;
                    }
                    else
                    {
                        svc_guid = services[i].ServiceUuid.Value.LongUuid;
                    }
                    TRACE( "gatt_services_thread: service[%lu] UUID=%s\n", i, debugstr_guid( &svc_guid ) );
                    if (IsEqualGUID( &svc_guid, &ctx->filter_uuid ))
                    {
                        TRACE( "gatt_services_thread: matched service at index %lu\n", i );
                        filtered_services[filtered_count++] = services[i];
                    }
                }
                TRACE( "gatt_services_thread: filtered %lu -> %lu services\n", svc_count, filtered_count );
                if (filtered_count > 0)
                {
                    hr = gatt_services_vector_create( filtered_services, filtered_count, handle_for_services,
                                                       use_radio_fallback, WindowsGetStringRawBuffer( impl->id, NULL ), impl->address, &services_vector );
                }
                else
                {
                    hr = S_OK;
                }
            }
            else
            {
                hr = gatt_services_vector_create( services, svc_count, handle_for_services,
                                                   use_radio_fallback, WindowsGetStringRawBuffer( impl->id, NULL ), impl->address, &services_vector );
            }
            if (FAILED( hr ))
            {
                TRACE( "Failed to create services vector: hr=0x%lx\n", hr );
                status = GattCommunicationStatus_Unreachable;
            }
        }
    }
    else
    {
        DWORD err = GetLastError();
        TRACE( "IOCTL failed: error=%lu\n", err );
        status = GattCommunicationStatus_Unreachable;
    }

done:
    TRACE( "gatt_services_thread: done, gatt_status=%d\n", status );
    if (params) free( params );
    if (radio_params) free( radio_params );
    if (radio_handle != INVALID_HANDLE_VALUE) CloseHandle( radio_handle );

    hr = gatt_services_result_create( status, services_vector, &result );
    if (services_vector) IVectorView_GattDeviceService_Release( services_vector );

    if (SUCCEEDED(hr))
    {
        op_impl->result = result;
        op_impl->status = Completed;
        TRACE( "gatt_services_thread: async_status=Completed\n" );
    }
    else
    {
        op_impl->status = Error;
        TRACE( "gatt_services_thread: async_status=Error hr=0x%lx\n", hr );
    }

    TRACE( "gatt_services_thread: handler=%p result=%p status=%d\n", op_impl->handler, op_impl->result, op_impl->status );
    if (op_impl->handler)
    {
        HRESULT invoke_hr;
        TRACE( "gatt_services_thread: ABOUT TO INVOKE HANDLER handler=%p op=%p status=%d \n",
             op_impl->handler, ctx->op, op_impl->status );
        invoke_hr = IAsyncOperationCompletedHandler_GattDeviceServicesResult_Invoke( op_impl->handler, ctx->op, op_impl->status );
        TRACE( "gatt_services_thread: HANDLER INVOKE RETURNED hr=0x%lx \n", invoke_hr );
        TRACE( "gatt_services_thread: handler returned hr=0x%lx\n", invoke_hr );
    }
    else
    {
        TRACE( "gatt_services_thread: NO HANDLER TO INVOKE \n" );
    }

    TRACE( "gatt_services_thread: done final_status=%d\n", op_impl->status );
    IBluetoothLEDevice3_Release( ctx->iface );
    IAsyncOperation_GattDeviceServicesResult_Release( ctx->op );
    free( ctx );
    CoUninitialize();
    return 0;
}

static HRESULT WINAPI le_device3_GetGattServicesWithCacheModeAsync( IBluetoothLEDevice3 *iface,
                                                                    BluetoothCacheMode cacheMode,
                                                                    IAsyncOperation_GattDeviceServicesResult **operation )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice3( iface );
    struct winebth_le_device_get_gatt_services_params *params = NULL;
    struct winebth_radio_get_le_device_gatt_services_params *radio_params = NULL;
    IVectorView_GattDeviceService *services_vector = NULL;
    IGattDeviceServicesResult *result = NULL;
    GattCommunicationStatus status = GattCommunicationStatus_Unreachable;
    struct async_gatt_services_op *op_impl;
    HANDLE radio_handle = INVALID_HANDLE_VALUE;
    BOOL use_radio_fallback = FALSE;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    HRESULT hr;
    ULONG service_count = 0;

    TRACE( "(%p, %d, %p)\n", iface, cacheMode, operation );
    if (!operation) return E_POINTER;

    /* Do service discovery SYNCHRONOUSLY before creating async operation.
     * Windows GetGattServicesAsync returns an operation that's ALREADY completed with cached services. */

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        radio_handle = open_first_radio();
        if (radio_handle == INVALID_HANDLE_VALUE)
            goto create_failed_operation;
        use_radio_fallback = TRUE;
    }

    if (use_radio_fallback)
    {
        buffer_size = offsetof( struct winebth_radio_get_le_device_gatt_services_params, services[16] );
        radio_params = malloc( buffer_size );
        if (!radio_params)
        {
            CloseHandle( radio_handle );
            return E_OUTOFMEMORY;
        }
        memset( radio_params, 0, buffer_size );
        radio_params->address = impl->address;
        radio_params->count = 0;

        if (DeviceIoControl( radio_handle, IOCTL_WINEBTH_RADIO_GET_LE_DEVICE_GATT_SERVICES,
                             radio_params, buffer_size, radio_params, buffer_size, &bytes_returned, NULL ))
        {
            status = GattCommunicationStatus_Success;
            service_count = radio_params->count;

            if (radio_params->count > 0)
            {
                hr = gatt_services_vector_create( radio_params->services, radio_params->count, radio_handle,
                                                   TRUE, WindowsGetStringRawBuffer( impl->id, NULL ), impl->address, &services_vector );
                if (FAILED( hr ))
                {
                    TRACE( "Failed to create services vector: hr=0x%lx\n", hr );
                    status = GattCommunicationStatus_Unreachable;
                }
            }
        }
        else
        {
            DWORD err = GetLastError();
            TRACE( "IOCTL failed: error=%lu\n", err );
        }

        free( radio_params );
        CloseHandle( radio_handle );
    }
    else
    {
        buffer_size = offsetof( struct winebth_le_device_get_gatt_services_params, services[16] );
        params = malloc( buffer_size );
        if (!params) return E_OUTOFMEMORY;

        memset( params, 0, buffer_size );
        params->count = 0;

        if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES, NULL, 0,
                             params, buffer_size, &bytes_returned, NULL ))
        {
            status = GattCommunicationStatus_Success;
            service_count = params->count;

            if (params->count > 0)
            {
                hr = gatt_services_vector_create( params->services, params->count, impl->device_handle,
                                                   FALSE, WindowsGetStringRawBuffer( impl->id, NULL ), impl->address, &services_vector );
                if (FAILED( hr ))
                {
                    TRACE( "Failed to create services vector: hr=0x%lx\n", hr );
                    status = GattCommunicationStatus_Unreachable;
                }
            }
        }
        else
        {
            DWORD err = GetLastError();
            TRACE( "IOCTL failed: error=%lu\n", err );
        }

        free( params );
    }

create_failed_operation:

    /* Create result with discovered services */
    hr = gatt_services_result_create( status, services_vector, &result );
    if (services_vector) IVectorView_GattDeviceService_Release( services_vector );
    if (FAILED( hr ))
    {
        TRACE( "Failed to create result: hr=0x%lx\n", hr );
        return hr;
    }

    /* Create async operation that's ALREADY completed */
    hr = async_gatt_services_op_create( result, operation );
    IGattDeviceServicesResult_Release( result );  /* op took its own ref */
    if (FAILED( hr ))
    {
        TRACE( "Failed to create async operation: hr=0x%lx\n", hr );
        return hr;
    }

    /* Mark as Completed so await_ready returns true immediately */
    op_impl = impl_from_IAsyncOperation_GattDeviceServicesResult( *operation );
    op_impl->status = Completed;

    return S_OK;
}

static HRESULT WINAPI le_device3_GetGattServicesForUuidWithCacheModeAsync( IBluetoothLEDevice3 *iface,
                                                                           GUID serviceUuid,
                                                                           BluetoothCacheMode cacheMode,
                                                                           IAsyncOperation_GattDeviceServicesResult **operation );

static HRESULT WINAPI le_device3_GetGattServicesAsync( IBluetoothLEDevice3 *iface,
                                                       IAsyncOperation_GattDeviceServicesResult **operation )
{
    TRACE( "(%p, %p)\n", iface, operation );
    return le_device3_GetGattServicesWithCacheModeAsync( iface, BluetoothCacheMode_Cached, operation );
}

static HRESULT WINAPI le_device3_GetGattServicesForUuidAsync( IBluetoothLEDevice3 *iface,
                                                              GUID serviceUuid,
                                                              IAsyncOperation_GattDeviceServicesResult **operation )
{
    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( &serviceUuid ), operation );
    return le_device3_GetGattServicesForUuidWithCacheModeAsync( iface, serviceUuid, BluetoothCacheMode_Cached, operation );
}

static HRESULT WINAPI le_device3_GetGattServicesForUuidWithCacheModeAsync( IBluetoothLEDevice3 *iface,
                                                                           GUID serviceUuid,
                                                                           BluetoothCacheMode cacheMode,
                                                                           IAsyncOperation_GattDeviceServicesResult **operation )
{
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice3( iface );
    struct winebth_le_device_get_gatt_services_params *params = NULL;
    struct winebth_radio_get_le_device_gatt_services_params *radio_params = NULL;
    IVectorView_GattDeviceService *services_vector = NULL;
    IGattDeviceServicesResult *result = NULL;
    GattCommunicationStatus status = GattCommunicationStatus_Unreachable;
    struct async_gatt_services_op *op_impl;
    HANDLE radio_handle = INVALID_HANDLE_VALUE;
    BOOL use_radio_fallback = FALSE;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    HRESULT hr;
    ULONG service_count = 0;

    TRACE( "(%p, %s, %d, %p)\n", iface, debugstr_guid( &serviceUuid ), cacheMode, operation );
    if (!operation) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        radio_handle = open_first_radio();
        if (radio_handle != INVALID_HANDLE_VALUE) use_radio_fallback = TRUE;
    }

    if (use_radio_fallback)
    {
        buffer_size = offsetof( struct winebth_radio_get_le_device_gatt_services_params, services[16] );
        radio_params = malloc( buffer_size );
        if (radio_params)
        {
            memset( radio_params, 0, buffer_size );
            radio_params->address = impl->address;
            if (DeviceIoControl( radio_handle, IOCTL_WINEBTH_RADIO_GET_LE_DEVICE_GATT_SERVICES,
                                 radio_params, buffer_size, radio_params, buffer_size, &bytes_returned, NULL ))
            {
                BTH_LE_GATT_SERVICE filtered[16];
                ULONG filtered_count = 0, i;
                status = GattCommunicationStatus_Success;
                for (i = 0; i < radio_params->count && filtered_count < 16; i++)
                {
                    GUID svc_guid;
                    bth_le_uuid_to_guid( &radio_params->services[i].ServiceUuid, &svc_guid );
                    if (IsEqualGUID( &svc_guid, &serviceUuid )) filtered[filtered_count++] = radio_params->services[i];
                }
                service_count = filtered_count;
                if (filtered_count > 0)
                    gatt_services_vector_create( filtered, filtered_count, radio_handle, TRUE,
                                                 WindowsGetStringRawBuffer( impl->id, NULL ), impl->address, &services_vector );
            }
            free( radio_params );
        }
        CloseHandle( radio_handle );
    }
    else
    {
        buffer_size = offsetof( struct winebth_le_device_get_gatt_services_params, services[16] );
        params = malloc( buffer_size );
        if (params)
        {
            memset( params, 0, buffer_size );
            if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES, NULL, 0,
                                 params, buffer_size, &bytes_returned, NULL ))
            {
                BTH_LE_GATT_SERVICE filtered[16];
                ULONG filtered_count = 0, i;
                status = GattCommunicationStatus_Success;
                for (i = 0; i < params->count && filtered_count < 16; i++)
                {
                    GUID svc_guid;
                    bth_le_uuid_to_guid( &params->services[i].ServiceUuid, &svc_guid );
                    if (IsEqualGUID( &svc_guid, &serviceUuid )) filtered[filtered_count++] = params->services[i];
                }
                service_count = filtered_count;
                if (filtered_count > 0)
                    gatt_services_vector_create( filtered, filtered_count, impl->device_handle, FALSE,
                                                 WindowsGetStringRawBuffer( impl->id, NULL ), impl->address, &services_vector );
            }
            free( params );
        }
    }

    hr = async_gatt_services_op_create( NULL, operation );
    if (FAILED(hr)) {
        if (services_vector) IVectorView_GattDeviceService_Release( services_vector );
        return hr;
    }

    hr = gatt_services_result_create( status, services_vector, &result );
    if (services_vector) IVectorView_GattDeviceService_Release( services_vector );
    
    op_impl = impl_from_IAsyncOperation_GattDeviceServicesResult( *operation );
    op_impl->result = result;
    op_impl->status = Completed;

    return S_OK;
}

static const IBluetoothLEDevice3Vtbl le_device3_vtbl =
{
    le_device3_QueryInterface,
    le_device3_AddRef,
    le_device3_Release,
    le_device3_GetIids,
    le_device3_GetRuntimeClassName,
    le_device3_GetTrustLevel,
    le_device3_get_DeviceAccessInformation,
    (void*)le_device3_RequestAccessAsync,
    le_device3_GetGattServicesAsync,
    le_device3_GetGattServicesWithCacheModeAsync,
    le_device3_GetGattServicesForUuidAsync,
    le_device3_GetGattServicesForUuidWithCacheModeAsync
};

static UINT64 swap_address_bytes( UINT64 addr )
{
    return ((addr >> 40) & 0xFFULL) |
           (((addr >> 32) & 0xFFULL) << 8) |
           (((addr >> 24) & 0xFFULL) << 16) |
           (((addr >> 16) & 0xFFULL) << 24) |
           (((addr >> 8) & 0xFFULL) << 32) |
           ((addr & 0xFFULL) << 40);
}

static HANDLE open_le_device_interface( UINT64 address )
{
    BYTE buffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + MAX_PATH * sizeof( WCHAR )];
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *iface_detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
    SP_DEVICE_INTERFACE_DATA iface_data;
    HDEVINFO devinfo;
    HANDLE device = INVALID_HANDLE_VALUE;
    DWORD idx = 0;
    WCHAR addr_str[32], addr_str_swapped[32];
    UINT64 addr_48 = address & 0xFFFFFFFFFFFFULL;
    UINT64 addr_swapped = swap_address_bytes( addr_48 );
    int retry;

    swprintf( addr_str, ARRAY_SIZE(addr_str), L"%012I64x", addr_48 );
    swprintf( addr_str_swapped, ARRAY_SIZE(addr_str_swapped), L"%012I64x", addr_swapped );

    for (retry = 0; retry < 5; retry++)
    {
        idx = 0;
        devinfo = SetupDiGetClassDevsW( &btle_device_interface_guid, NULL, NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE );
        if (devinfo == INVALID_HANDLE_VALUE)
        {
            Sleep( 100 );
            continue;
        }

        iface_detail->cbSize = sizeof( *iface_detail );
        iface_data.cbSize = sizeof( iface_data );

        while (SetupDiEnumDeviceInterfaces( devinfo, NULL, &btle_device_interface_guid, idx++, &iface_data ))
        {
            if (!SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, iface_detail, sizeof( buffer ), NULL, NULL ))
                continue;

            {
                /* Try multiple delimiters: & (legacy), _ (alt), # (reference string from IoRegisterDeviceInterface) */
                const WCHAR *delims[] = { L"&", L"_", L"#" };
                const WCHAR *addr_ptr = NULL;
                WCHAR path_addr[32];
                int d;
                for (d = 0; d < 3 && !addr_ptr; d++)
                {
                    const WCHAR *p = iface_detail->DevicePath;
                    while ((p = wcsstr( p, delims[d] )) != NULL)
                    {
                        p++;
                        /* Check if this looks like a 12-char hex address */
                        memset( path_addr, 0, sizeof(path_addr) );
                        lstrcpynW( path_addr, p, 13 );
                        if (wcslen( path_addr ) >= 12)
                        {
                            if (_wcsicmp( path_addr, addr_str ) == 0 || _wcsicmp( path_addr, addr_str_swapped ) == 0)
                            {
                                addr_ptr = p;
                                break;
                            }
                        }
                    }
                }
                if (addr_ptr)
                {
                    memset( path_addr, 0, sizeof(path_addr) );
                    lstrcpynW( path_addr, addr_ptr, 13 );
                    device = CreateFileW( iface_detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
                    if (device != INVALID_HANDLE_VALUE)
                    {
                        SetupDiDestroyDeviceInfoList( devinfo );
                        return device;
                    }
                }
            }
        }
        SetupDiDestroyDeviceInfoList( devinfo );
        Sleep( 100 );
    }

    return device;
}

static HRESULT bluetooth_le_device_create( HSTRING id, UINT64 address, IBluetoothLEDevice **out )
{
    struct bluetooth_le_device *impl;
    WCHAR device_name[256];

    TRACE( "(%s, %I64x, %p)\n", debugstr_hstring(id), address, out );

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothLEDevice_iface.lpVtbl = &le_device_vtbl;
    impl->IBluetoothLEDevice3_iface.lpVtbl = &le_device3_vtbl;
    impl->IClosable_iface.lpVtbl = &closable_vtbl;
    impl->ref = 1;
    impl->address = address;
    impl->device_handle = INVALID_HANDLE_VALUE;
    WindowsDuplicateString( id, &impl->id );
    list_init( &impl->name_changed_handlers );
    list_init( &impl->connection_status_changed_handlers );
    list_init( &impl->gatt_services_changed_handlers );

    device_name[0] = 0;
    if (!get_device_name_from_id( WindowsGetStringRawBuffer( id, NULL ), address, device_name, ARRAY_SIZE( device_name ) ))
        wcscpy( device_name, L"" );
    wcscpy( impl->name, device_name );

    impl->device_handle = open_le_device_interface( address );

    *out = &impl->IBluetoothLEDevice_iface;
    TRACE( "created %p, name=%s, address=%I64x, handle=%p\n", impl, debugstr_w( device_name ), address, impl->device_handle );
    return S_OK;
}

struct async_le_device_op
{
    IAsyncOperation_BluetoothLEDevice IAsyncOperation_BluetoothLEDevice_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IAsyncOperationCompletedHandler_BluetoothLEDevice *handler;
    IBluetoothLEDevice *result;
    AsyncStatus status;
};

static inline struct async_le_device_op *impl_from_IAsyncOperation_BluetoothLEDevice( IAsyncOperation_BluetoothLEDevice *iface )
{
    return CONTAINING_RECORD( iface, struct async_le_device_op, IAsyncOperation_BluetoothLEDevice_iface );
}

static HRESULT WINAPI async_op_QueryInterface( IAsyncOperation_BluetoothLEDevice *iface, REFIID iid, void **out )
{
    struct async_le_device_op *impl = impl_from_IAsyncOperation_BluetoothLEDevice( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_BluetoothLEDevice ))
    {
        IAsyncOperation_BluetoothLEDevice_AddRef( (*out = &impl->IAsyncOperation_BluetoothLEDevice_iface) );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IAsyncInfo_AddRef( (*out = &impl->IAsyncInfo_iface) );
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI async_op_AddRef( IAsyncOperation_BluetoothLEDevice *iface )
{
    struct async_le_device_op *impl = impl_from_IAsyncOperation_BluetoothLEDevice( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_op_Release( IAsyncOperation_BluetoothLEDevice *iface )
{
    struct async_le_device_op *impl = impl_from_IAsyncOperation_BluetoothLEDevice( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->handler) IAsyncOperationCompletedHandler_BluetoothLEDevice_Release( impl->handler );
        if (impl->result) IBluetoothLEDevice_Release( impl->result );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_op_GetIids( IAsyncOperation_BluetoothLEDevice *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IAsyncOperation_BluetoothLEDevice, &IID_IAsyncInfo };
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

static HRESULT WINAPI async_op_GetRuntimeClassName( IAsyncOperation_BluetoothLEDevice *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Foundation.IAsyncOperation`1<Windows.Devices.Bluetooth.BluetoothLEDevice>";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI async_op_GetTrustLevel( IAsyncOperation_BluetoothLEDevice *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI async_op_put_Completed( IAsyncOperation_BluetoothLEDevice *iface,
                                              IAsyncOperationCompletedHandler_BluetoothLEDevice *handler )
{
    struct async_le_device_op *impl = impl_from_IAsyncOperation_BluetoothLEDevice( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;
    impl->handler = handler;
    IAsyncOperationCompletedHandler_BluetoothLEDevice_AddRef( handler );
    /* Complete immediately */
    if (impl->status == Started)
        impl->status = Completed;
    IAsyncOperationCompletedHandler_BluetoothLEDevice_Invoke( handler, iface, impl->status );
    return S_OK;
}

static HRESULT WINAPI async_op_get_Completed( IAsyncOperation_BluetoothLEDevice *iface,
                                              IAsyncOperationCompletedHandler_BluetoothLEDevice **handler )
{
    struct async_le_device_op *impl = impl_from_IAsyncOperation_BluetoothLEDevice( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;
    *handler = impl->handler;
    if (*handler) IAsyncOperationCompletedHandler_BluetoothLEDevice_AddRef( *handler );
    return S_OK;
}

static HRESULT WINAPI async_op_GetResults( IAsyncOperation_BluetoothLEDevice *iface, IBluetoothLEDevice **results )
{
    struct async_le_device_op *impl = impl_from_IAsyncOperation_BluetoothLEDevice( iface );
    TRACE( "(%p, %p)\n", iface, results );
    if (!results) return E_POINTER;
    *results = impl->result;
    if (*results) IBluetoothLEDevice_AddRef( *results );
    return S_OK;
}

static const IAsyncOperation_BluetoothLEDeviceVtbl async_op_vtbl =
{
    async_op_QueryInterface,
    async_op_AddRef,
    async_op_Release,
    async_op_GetIids,
    async_op_GetRuntimeClassName,
    async_op_GetTrustLevel,
    async_op_put_Completed,
    async_op_get_Completed,
    async_op_GetResults
};

static inline struct async_le_device_op *impl_from_IAsyncInfo( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct async_le_device_op, IAsyncInfo_iface );
}

static HRESULT WINAPI async_info_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    struct async_le_device_op *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperation_BluetoothLEDevice_QueryInterface( &impl->IAsyncOperation_BluetoothLEDevice_iface, iid, out );
}

static ULONG WINAPI async_info_AddRef( IAsyncInfo *iface )
{
    struct async_le_device_op *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperation_BluetoothLEDevice_AddRef( &impl->IAsyncOperation_BluetoothLEDevice_iface );
}

static ULONG WINAPI async_info_Release( IAsyncInfo *iface )
{
    struct async_le_device_op *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperation_BluetoothLEDevice_Release( &impl->IAsyncOperation_BluetoothLEDevice_iface );
}

static HRESULT WINAPI async_info_GetIids( IAsyncInfo *iface, ULONG *iid_count, IID **iids )
{
    struct async_le_device_op *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperation_BluetoothLEDevice_GetIids( &impl->IAsyncOperation_BluetoothLEDevice_iface, iid_count, iids );
}

static HRESULT WINAPI async_info_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *class_name )
{
    struct async_le_device_op *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperation_BluetoothLEDevice_GetRuntimeClassName( &impl->IAsyncOperation_BluetoothLEDevice_iface, class_name );
}

static HRESULT WINAPI async_info_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    struct async_le_device_op *impl = impl_from_IAsyncInfo( iface );
    return IAsyncOperation_BluetoothLEDevice_GetTrustLevel( &impl->IAsyncOperation_BluetoothLEDevice_iface, level );
}

static HRESULT WINAPI async_info_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI async_info_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_le_device_op *impl = impl_from_IAsyncInfo( iface );
    *status = impl->status;
    return S_OK;
}

static HRESULT WINAPI async_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    *error_code = S_OK;
    return S_OK;
}

static HRESULT WINAPI async_info_Cancel( IAsyncInfo *iface )
{
    return S_OK;
}

static HRESULT WINAPI async_info_Close( IAsyncInfo *iface )
{
    return S_OK;
}

static const IAsyncInfoVtbl async_info_vtbl =
{
    async_info_QueryInterface,
    async_info_AddRef,
    async_info_Release,
    async_info_GetIids,
    async_info_GetRuntimeClassName,
    async_info_GetTrustLevel,
    async_info_get_Id,
    async_info_get_Status,
    async_info_get_ErrorCode,
    async_info_Cancel,
    async_info_Close
};

static HRESULT async_le_device_op_create( IBluetoothLEDevice *device, IAsyncOperation_BluetoothLEDevice **out )
{
    struct async_le_device_op *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_BluetoothLEDevice_iface.lpVtbl = &async_op_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_info_vtbl;
    impl->ref = 1;
    impl->result = device;
    impl->status = device ? Completed : Error;
    if (device) IBluetoothLEDevice_AddRef( device );

    *out = &impl->IAsyncOperation_BluetoothLEDevice_iface;
    return S_OK;
}

DEFINE_IINSPECTABLE( bluetoothledevice_statics, IBluetoothLEDeviceStatics, struct bluetoothledevice_statics, IActivationFactory_iface );
DEFINE_IINSPECTABLE( bluetoothledevice_statics2, IBluetoothLEDeviceStatics2, struct bluetoothledevice_statics, IActivationFactory_iface );

static HRESULT WINAPI bluetoothledevice_statics_FromIdAsync( IBluetoothLEDeviceStatics *iface, HSTRING id, IAsyncOperation_BluetoothLEDevice **async_op )
{
    HRESULT hr;
    IBluetoothLEDevice *device;
    UINT64 addr = 0;
    const WCHAR *str;
    const WCHAR *addr_ptr;

    TRACE( "(%s, %p)\n", debugstr_hstring( id ), async_op );

    str = WindowsGetStringRawBuffer( id, NULL );
    addr_ptr = wcsstr( str, L"&" );
    if (addr_ptr) swscanf( addr_ptr + 1, L"%I64x", &addr );
    else if ((addr_ptr = wcsstr( str, L"_" ))) swscanf( addr_ptr + 1, L"%I64x", &addr );

    hr = bluetooth_le_device_create( id, addr, &device );
    if (FAILED(hr))
        return hr;

    hr = async_le_device_op_create( device, async_op );
    IBluetoothLEDevice_Release( device );
    return hr;
}

static HRESULT WINAPI bluetoothledevice_statics_FromBluetoothAddressAsync( IBluetoothLEDeviceStatics *iface, UINT64 addr, IAsyncOperation_BluetoothLEDevice **async_op )
{
    HRESULT hr;
    IBluetoothLEDevice *device;
    WCHAR buf[64];
    HSTRING id;

    TRACE( "(%p, %I64x, %p)\n", iface, addr, async_op );
    if (!async_op) return E_POINTER;

    swprintf( buf, ARRAY_SIZE(buf), L"BT:%I64u", addr );
    WindowsCreateString( buf, wcslen(buf), &id );

    hr = bluetooth_le_device_create( id, addr, &device );
    WindowsDeleteString( id );
    if (FAILED(hr)) return hr;

    hr = async_le_device_op_create( device, async_op );
    IBluetoothLEDevice_Release( device );
    return hr;
}

static HRESULT WINAPI bluetoothledevice_statics_GetDeviceSelector( IBluetoothLEDeviceStatics *iface, HSTRING *result )
{
    static const WCHAR selector[] = L"System.Devices.InterfaceClassGuid:=\"{781aee18-7733-4ce4-add0-91f41c67b592}\"";
    TRACE( "(%p, %p)\n", iface, result );
    if (!result) return E_POINTER;
    return WindowsCreateString( selector, ARRAY_SIZE( selector ) - 1, result );
}

static const IBluetoothLEDeviceStaticsVtbl bluetoothledevice_statics_vtbl =
{
    /* IUnknown */
    bluetoothledevice_statics_QueryInterface,
    bluetoothledevice_statics_AddRef,
    bluetoothledevice_statics_Release,
    /* IInspectable */
    bluetoothledevice_statics_GetIids,
    bluetoothledevice_statics_GetRuntimeClassName,
    bluetoothledevice_statics_GetTrustLevel,
    /* IBluetoothLEDeviceStatics */
    bluetoothledevice_statics_FromIdAsync,
    bluetoothledevice_statics_FromBluetoothAddressAsync,
    bluetoothledevice_statics_GetDeviceSelector
};

static HRESULT WINAPI bluetoothledevice_statics2_GetDeviceSelectorFromPairingState( IBluetoothLEDeviceStatics2 *iface,
                                                                                     boolean paired, HSTRING *result )
{
    WCHAR buf[256];
    TRACE( "(%p, %d, %p)\n", iface, paired, result );
    if (!result) return E_POINTER;
    swprintf( buf, ARRAY_SIZE(buf),
              L"System.Devices.InterfaceClassGuid:=\"{781aee18-7733-4ce4-add0-91f41c67b592}\" AND System.Devices.Aep.Bluetooth.IssueInquiry:=System.StructuredQueryType.Boolean#True AND System.Devices.Aep.IsPaired:=%s",
              paired ? L"System.StructuredQueryType.Boolean#True" : L"System.StructuredQueryType.Boolean#False" );
    return WindowsCreateString( buf, wcslen(buf), result );
}

static HRESULT WINAPI bluetoothledevice_statics2_GetDeviceSelectorFromConnectionStatus( IBluetoothLEDeviceStatics2 *iface,
                                                                                         BluetoothConnectionStatus status,
                                                                                         HSTRING *result )
{
    WCHAR buf[256];
    TRACE( "(%p, %d, %p)\n", iface, status, result );
    if (!result) return E_POINTER;
    swprintf( buf, ARRAY_SIZE(buf),
              L"System.Devices.InterfaceClassGuid:=\"{781aee18-7733-4ce4-add0-91f41c67b592}\" AND System.Devices.Aep.IsConnected:=%s",
              status == BluetoothConnectionStatus_Connected ? L"System.StructuredQueryType.Boolean#True" : L"System.StructuredQueryType.Boolean#False" );
    return WindowsCreateString( buf, wcslen(buf), result );
}

static HRESULT WINAPI bluetoothledevice_statics2_GetDeviceSelectorFromDeviceName( IBluetoothLEDeviceStatics2 *iface,
                                                                                   HSTRING name, HSTRING *result )
{
    WCHAR buf[512];
    const WCHAR *name_str;
    TRACE( "(%p, %s, %p)\n", iface, debugstr_hstring(name), result );
    if (!result) return E_POINTER;
    name_str = WindowsGetStringRawBuffer( name, NULL );
    swprintf( buf, ARRAY_SIZE(buf),
              L"System.Devices.InterfaceClassGuid:=\"{781aee18-7733-4ce4-add0-91f41c67b592}\" AND System.ItemNameDisplay:=\"%s\"",
              name_str ? name_str : L"" );
    return WindowsCreateString( buf, wcslen(buf), result );
}

static HRESULT WINAPI bluetoothledevice_statics2_GetDeviceSelectorFromBluetoothAddress( IBluetoothLEDeviceStatics2 *iface,
                                                                                         UINT64 addr, HSTRING *result )
{
    WCHAR buf[256];
    TRACE( "(%p, %I64x, %p)\n", iface, addr, result );
    if (!result) return E_POINTER;
    swprintf( buf, ARRAY_SIZE(buf),
              L"System.Devices.InterfaceClassGuid:=\"{781aee18-7733-4ce4-add0-91f41c67b592}\" AND System.DeviceInterface.Bluetooth.DeviceAddress:=\"%012I64X\"",
              addr );
    return WindowsCreateString( buf, wcslen(buf), result );
}

static HRESULT WINAPI bluetoothledevice_statics2_GetDeviceSelectorFromBluetoothAddressWithBluetoothAddressType(
                                                                                         IBluetoothLEDeviceStatics2 *iface,
                                                                                         UINT64 addr,
                                                                                         BluetoothAddressType type,
                                                                                         HSTRING *result )
{
    TRACE( "(%p, %I64x, %d, %p)\n", iface, addr, type, result );
    return bluetoothledevice_statics2_GetDeviceSelectorFromBluetoothAddress( iface, addr, result );
}

static HRESULT WINAPI bluetoothledevice_statics2_GetDeviceSelectorFromAppearance( IBluetoothLEDeviceStatics2 *iface,
                                                                                   IInspectable *appearance,
                                                                                   HSTRING *result )
{
    FIXME( "(%p, %p, %p) stub!\n", iface, appearance, result );
    return E_NOTIMPL;
}

static HRESULT WINAPI bluetoothledevice_statics2_FromBluetoothAddressWithBluetoothAddressTypeAsync(
                                                                                         IBluetoothLEDeviceStatics2 *iface,
                                                                                         UINT64 addr,
                                                                                         BluetoothAddressType type,
                                                                                         IAsyncOperation_BluetoothLEDevice **async_op )
{
    HRESULT hr;
    IBluetoothLEDevice *device;
    WCHAR buf[64];
    HSTRING id;

    TRACE( "(%p, %I64x, %d, %p)\n", iface, addr, type, async_op );
    if (!async_op) return E_POINTER;

    swprintf( buf, ARRAY_SIZE(buf), L"BT:%I64u", addr );
    WindowsCreateString( buf, wcslen(buf), &id );

    hr = bluetooth_le_device_create( id, addr, &device );
    WindowsDeleteString( id );
    if (FAILED(hr)) return hr;

    hr = async_le_device_op_create( device, async_op );
    IBluetoothLEDevice_Release( device );
    return hr;
}

static const IBluetoothLEDeviceStatics2Vtbl bluetoothledevice_statics2_vtbl =
{
    /* IUnknown */
    bluetoothledevice_statics2_QueryInterface,
    bluetoothledevice_statics2_AddRef,
    bluetoothledevice_statics2_Release,
    /* IInspectable */
    bluetoothledevice_statics2_GetIids,
    bluetoothledevice_statics2_GetRuntimeClassName,
    bluetoothledevice_statics2_GetTrustLevel,
    /* IBluetoothLEDeviceStatics2 */
    bluetoothledevice_statics2_GetDeviceSelectorFromPairingState,
    bluetoothledevice_statics2_GetDeviceSelectorFromConnectionStatus,
    bluetoothledevice_statics2_GetDeviceSelectorFromDeviceName,
    bluetoothledevice_statics2_GetDeviceSelectorFromBluetoothAddress,
    bluetoothledevice_statics2_GetDeviceSelectorFromBluetoothAddressWithBluetoothAddressType,
    bluetoothledevice_statics2_GetDeviceSelectorFromAppearance,
    bluetoothledevice_statics2_FromBluetoothAddressWithBluetoothAddressTypeAsync
};

static struct bluetoothledevice_statics bluetoothledevice_statics =
{
    {&ble_device_factory_vtbl},
    {&bluetoothledevice_statics_vtbl},
    {&bluetoothledevice_statics2_vtbl},
    1
};

IActivationFactory *bluetoothledevice_statics_factory = &bluetoothledevice_statics.IActivationFactory_iface;
