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
    return InterlockedIncrement( &impl->ref );
}

static ULONG WINAPI gatt_buffer_Release( IBuffer *iface )
{
    struct gatt_buffer *impl = impl_from_IBuffer( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    if (!ref) free( impl );
    return ref;
}

static HRESULT WINAPI gatt_buffer_GetIids( IBuffer *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
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
    if (!(impl = malloc( offsetof( struct gatt_buffer, data[size] ) ))) return E_OUTOFMEMORY;
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

    TRACE( "(%p, %s, %p)\n", iface, debugstr_hstring( id ), async_op );
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

    swprintf( buf, ARRAY_SIZE(buf), L"Bluetooth#Bluetooth%012I64x", address );
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

    ERR( "=== ble_device_factory_QueryInterface: iface=%p iid=%s out=%p ===\n", iface, debugstr_guid( iid ), out );
    TRACE( "(%p, %s, %p) %ld\n", iface, debugstr_guid( iid ), out, impl->ref );

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
        ERR( "=== Returning IBluetoothLEDeviceStatics2 interface: %p ===\n", &impl->IBluetoothLEDeviceStatics2_iface );
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
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->id, value );
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
    if (!get_classic_device_name_from_id( WindowsGetStringRawBuffer( id, NULL ), address, device_name, ARRAY_SIZE( device_name ) ))
        wcscpy( device_name, L"" );
    wcscpy( impl->name, device_name );

    *out = &impl->IBluetoothDevice_iface;
    TRACE( "Created BluetoothDevice %p, name=%s\n", impl, debugstr_w( device_name ) );
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
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->id, value );
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
            FIXME( "Found %lu GATT services, but IVectorView_GattDeviceService not implemented\n", params->count );
        }
        *value = NULL;
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

    TRACE( "(%p, %p)\n", iface, value );

    if (!value) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        *value = BluetoothConnectionStatus_Disconnected;
        return S_OK;
    }

    if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_CONNECTION_STATUS,
                         NULL, 0, &connected, sizeof(connected), &bytes_returned, NULL ))
    {
        *value = connected ? BluetoothConnectionStatus_Connected : BluetoothConnectionStatus_Disconnected;
        TRACE( "ConnectionStatus from driver: connected=%d\n", connected );
    }
    else
    {
        WARN( "IOCTL_WINEBTH_LE_DEVICE_GET_CONNECTION_STATUS failed: error=%lu\n", GetLastError() );
        *value = BluetoothConnectionStatus_Disconnected;
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

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( &service_uuid ), service );
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

    /* Trigger connection if not already connected */
    if (impl->device_handle != INVALID_HANDLE_VALUE)
    {
        memset( &params, 0, sizeof( params ) );
        DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES, NULL, 0,
                        &params, sizeof( params ), &bytes_returned, NULL );
        ERR( "=== Triggered BLE connection via IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES ===\n" );
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
#define IGattSession __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSession
#define IGattSessionVtbl __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CIGattSessionVtbl
typedef enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattSharingMode GattSharingMode;
typedef enum __x_ABI_CWindows_CDevices_CBluetooth_CGenericAttributeProfile_CGattOpenStatus GattOpenStatus;
#define IDeviceAccessInformation __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceAccessInformation
#define IDeviceAccessInformationVtbl __x_ABI_CWindows_CDevices_CEnumeration_CIDeviceAccessInformationVtbl

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
    BTH_LE_GATT_SERVICE service_info;
    WCHAR *device_id;
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
        free( impl->device_id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_service_GetIids( IGattDeviceService *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service_GetRuntimeClassName( IGattDeviceService *iface, HSTRING *class_name )
{
    FIXME( "(%p, %p): stub!\n", iface, class_name );
    return E_NOTIMPL;
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
    struct gatt_device_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsCreateString( impl->device_id, wcslen( impl->device_id ), value );
}

static HRESULT WINAPI gatt_service_get_Uuid( IGattDeviceService *iface, GUID *value )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->service_info.ServiceUuid.Value.LongUuid;
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
    BTH_LE_GATT_SERVICE service_info;
    BTH_LE_GATT_CHARACTERISTIC char_info;
    ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs *value_changed_handler;
    EventRegistrationToken value_changed_token;
    LONG next_token;
    HANDLE notification_thread;
    volatile BOOL notification_thread_stop;
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
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_char_AddRef( IGattCharacteristic *iface )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_char_Release( IGattCharacteristic *iface )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->value_changed_handler)
            ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( impl->value_changed_handler );
        if (impl->notification_thread)
        {
            impl->notification_thread_stop = TRUE;
            WaitForSingleObject( impl->notification_thread, INFINITE );
            CloseHandle( impl->notification_thread );
        }
        free( impl );
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
    *value = impl->char_info.CharacteristicUuid.Value.LongUuid;
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

static HRESULT WINAPI gatt_char_get_PresentationFormats( IGattCharacteristic *iface, IVectorView_GattPresentationFormat **value )
{
    FIXME( "(%p, %p): stub!\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT gatt_char_read_value_impl( IGattCharacteristic *iface, IAsyncOperation_GattReadResult **value )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    struct winebth_le_device_read_characteristic_params *params;
    GattCommunicationStatus status;
    IGattReadResult *result;
    IBuffer *buffer = NULL;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    HRESULT hr;

    ERR( "=== gatt_char_read_value_impl called: iface=%p device_handle=%p char_handle=%u ===\n",
         iface, impl->device_handle, impl->char_info.AttributeHandle );

    if (!value) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        ERR( "=== Device handle is invalid ===\n" );
        status = GattCommunicationStatus_Unreachable;
        hr = gatt_read_result_create( status, NULL, &result );
        if (FAILED( hr )) return hr;
        hr = async_gatt_read_op_create( result, value );
        IGattReadResult_Release( result );
        return hr;
    }

    buffer_size = offsetof( struct winebth_le_device_read_characteristic_params, data[512] );
    params = malloc( buffer_size );
    if (!params) return E_OUTOFMEMORY;

    memset( params, 0, buffer_size );
    params->service = impl->service_info;
    params->characteristic = impl->char_info;
    params->data_size = 0;

    ERR( "=== Calling IOCTL_WINEBTH_LE_DEVICE_READ_CHARACTERISTIC for char uuid=%s ===\n",
         debugstr_guid( &impl->char_info.CharacteristicUuid.Value.LongUuid ) );

    if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_READ_CHARACTERISTIC,
                         params, buffer_size, params, buffer_size, &bytes_returned, NULL ))
    {
        ERR( "=== IOCTL succeeded: data_size=%lu bytes_returned=%lu ===\n", params->data_size, bytes_returned );
        status = GattCommunicationStatus_Success;
        if (params->data_size > 0)
        {
            hr = gatt_buffer_create( params->data, params->data_size, &buffer );
            if (FAILED( hr ))
            {
                ERR( "=== Failed to create buffer: hr=0x%lx ===\n", hr );
                free( params );
                return hr;
            }
        }
    }
    else
    {
        DWORD err = GetLastError();
        ERR( "=== IOCTL failed: error=%lu ===\n", err );
        status = GattCommunicationStatus_Unreachable;
    }

    free( params );

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
    struct winebth_le_device_write_characteristic_params *params;
    IBufferByteAccess *byte_access = NULL;
    GattCommunicationStatus status;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    UINT32 data_len;
    BYTE *data_ptr;
    HRESULT hr;

    ERR( "=== gatt_char_write_value_impl called: iface=%p device_handle=%p char_handle=%u opt=%d ===\n",
         iface, impl->device_handle, impl->char_info.AttributeHandle, opt );

    if (!async) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        ERR( "=== Device handle is invalid ===\n" );
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Unreachable, async );
    }

    if (!value)
    {
        ERR( "=== Value buffer is null ===\n" );
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Success, async );
    }

    hr = IBuffer_QueryInterface( value, &IID_IBufferByteAccess, (void **)&byte_access );
    if (FAILED( hr ))
    {
        ERR( "=== Failed to get IBufferByteAccess: hr=0x%lx ===\n", hr );
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Unreachable, async );
    }

    hr = IBuffer_get_Length( value, &data_len );
    if (FAILED( hr ))
    {
        ERR( "=== Failed to get buffer length: hr=0x%lx ===\n", hr );
        IBufferByteAccess_Release( byte_access );
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Unreachable, async );
    }

    hr = IBufferByteAccess_Buffer( byte_access, &data_ptr );
    if (FAILED( hr ))
    {
        ERR( "=== Failed to get buffer data: hr=0x%lx ===\n", hr );
        IBufferByteAccess_Release( byte_access );
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Unreachable, async );
    }

    buffer_size = offsetof( struct winebth_le_device_write_characteristic_params, data[data_len] );
    params = malloc( buffer_size );
    if (!params)
    {
        IBufferByteAccess_Release( byte_access );
        return E_OUTOFMEMORY;
    }

    memset( params, 0, buffer_size );
    params->service = impl->service_info;
    params->characteristic = impl->char_info;
    params->write_type = (opt == GattWriteOption_WriteWithoutResponse) ? 1 : 0;
    params->data_size = data_len;
    if (data_len > 0)
        memcpy( params->data, data_ptr, data_len );

    IBufferByteAccess_Release( byte_access );

    ERR( "=== Calling IOCTL_WINEBTH_LE_DEVICE_WRITE_CHARACTERISTIC for char uuid=%s data_size=%lu write_type=%lu ===\n",
         debugstr_guid( &impl->char_info.CharacteristicUuid.Value.LongUuid ), params->data_size, params->write_type );

    

    if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_WRITE_CHARACTERISTIC,
                         params, buffer_size, NULL, 0, &bytes_returned, NULL ))
    {
        ERR( "=== IOCTL succeeded ===\n" );
        
        status = GattCommunicationStatus_Success;
    }
    else
    {
        DWORD err = GetLastError();
        ERR( "=== IOCTL failed: error=%lu ===\n", err );
        
        status = GattCommunicationStatus_Unreachable;
    }

    free( params );
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
    struct winebth_le_device_set_notify_params params;
    GattCommunicationStatus status;
    DWORD bytes_returned;

    ERR( "=== gatt_char_WriteClientCharacteristicConfigurationDescriptorAsync: iface=%p value=%d char_handle=%u ===\n",
         iface, value, impl->char_info.AttributeHandle );
    

    if (!async) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        ERR( "=== Invalid device handle! ===\n" );
        return async_gatt_comm_status_op_create( GattCommunicationStatus_Unreachable, async );
    }

    memset( &params, 0, sizeof( params ) );
    params.service = impl->service_info;
    params.characteristic = impl->char_info;
    params.enable = (value == GattClientCharacteristicConfigurationDescriptorValue_Notify ||
                     value == GattClientCharacteristicConfigurationDescriptorValue_Indicate);

    ERR( "=== Calling IOCTL_WINEBTH_LE_DEVICE_SET_NOTIFY enable=%d ===\n", params.enable );

    status = GattCommunicationStatus_Unreachable;
    if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_SET_NOTIFY,
                         &params, sizeof( params ), NULL, 0, &bytes_returned, NULL ))
    {
        status = GattCommunicationStatus_Success;
        ERR( "=== IOCTL_WINEBTH_LE_DEVICE_SET_NOTIFY succeeded ===\n" );
    }
    else
    {
        ERR( "=== IOCTL_WINEBTH_LE_DEVICE_SET_NOTIFY failed: %lu ===\n", GetLastError() );
    }

    return async_gatt_comm_status_op_create( status, async );
}

static HRESULT gatt_value_changed_event_args_create( IBuffer *value, IGattValueChangedEventArgs **out );

static DWORD WINAPI gatt_char_notification_thread( void *arg )
{
    struct gatt_characteristic *impl = arg;
    unsigned char buffer[256];
    unsigned int data_size;
    struct winebth_le_device_read_notification_params *params;
    ULONG params_size = sizeof(*params) + sizeof(buffer);
    ULONG bytes_returned;
    HRESULT hr;
    HANDLE device_handle;
    BTH_LE_GATT_SERVICE service_info;
    BTH_LE_GATT_CHARACTERISTIC char_info;

    ERR( "=== gatt_char_notification_thread: STARTED for char_handle=%u ===\n", impl->char_info.AttributeHandle );
    

    impl->IGattCharacteristic_iface.lpVtbl->AddRef( &impl->IGattCharacteristic_iface );

    device_handle = impl->device_handle;
    service_info = impl->service_info;
    char_info = impl->char_info;

    params = malloc( params_size );
    if (!params)
    {
        ERR( "=== gatt_char_notification_thread: malloc failed ===\n" );
        impl->IGattCharacteristic_iface.lpVtbl->Release( &impl->IGattCharacteristic_iface );
        return 1;
    }

    params->service = service_info;
    params->characteristic = char_info;

    while (1)
    {
        BOOL should_stop;
        ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs *handler;
        
        should_stop = impl->notification_thread_stop;
        if (should_stop) break;
        
        handler = impl->value_changed_handler;
        if (handler) ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_AddRef( handler );
        
        bytes_returned = 0;
        if (DeviceIoControl( device_handle, IOCTL_WINEBTH_LE_DEVICE_READ_NOTIFICATION,
                            params, sizeof(*params),
                            params, params_size,
                            &bytes_returned, NULL ))
        {
            ERR( "=== gatt_char_notification_thread: IOCTL succeeded, bytes_returned=%lu ===\n", bytes_returned );
            
            if (bytes_returned > sizeof(*params))
            {
                data_size = bytes_returned - sizeof(*params);
                ERR( "=== gatt_char_notification_thread: data_size=%u ===\n", data_size );
                if (data_size > 0 && data_size <= sizeof(buffer) && handler)
                {
                    struct gatt_buffer *gatt_buf;
                    IGattValueChangedEventArgs *event_args;
                    IBuffer *ibuf;

                    gatt_buf = calloc( 1, sizeof(*gatt_buf) + data_size );
                    if (gatt_buf)
                    {
                        gatt_buf->IBuffer_iface.lpVtbl = &gatt_buffer_vtbl;
                        gatt_buf->IBufferByteAccess_iface.lpVtbl = &gatt_buffer_byte_access_vtbl;
                        gatt_buf->ref = 1;
                        gatt_buf->capacity = data_size;
                        gatt_buf->length = data_size;
                        memcpy( gatt_buf->data, params->data, data_size );
                        ibuf = &gatt_buf->IBuffer_iface;

                        hr = gatt_value_changed_event_args_create( ibuf, &event_args );
                        if (SUCCEEDED(hr) && event_args)
                        {
                            ERR( "=== gatt_char_notification_thread: Invoking handler with %u bytes ===\n", data_size );
                            
                            if (handler)
                            {
                                typedef struct ITypedEventHandlerVtbl {
                                    HRESULT (STDMETHODCALLTYPE *QueryInterface)(void*, REFIID, void**);
                                    ULONG (STDMETHODCALLTYPE *AddRef)(void*);
                                    ULONG (STDMETHODCALLTYPE *Release)(void*);
                                    HRESULT (STDMETHODCALLTYPE *GetIids)(void*, ULONG*, IID**);
                                    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(void*, HSTRING*);
                                    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(void*, void*);
                                    HRESULT (STDMETHODCALLTYPE *Invoke)(void*, void*, void*);
                                } ITypedEventHandlerVtbl;
                                typedef struct { void *lpVtbl; } HandlerObj;
                                HandlerObj *handler_obj = (HandlerObj*)handler;
                                if (handler_obj && handler_obj->lpVtbl)
                                {
                                    ITypedEventHandlerVtbl *handler_vtbl = (ITypedEventHandlerVtbl*)handler_obj->lpVtbl;
                                    if (handler_vtbl->Invoke)
                                    {
                                        ERR( "=== gatt_char_notification_thread: Calling handler->Invoke ===\n" );
                                        handler_vtbl->Invoke( handler, &impl->IGattCharacteristic_iface, event_args );
                                        ERR( "=== gatt_char_notification_thread: Handler->Invoke returned ===\n" );
                                    }
                                    else
                                    {
                                        ERR( "=== gatt_char_notification_thread: handler_vtbl->Invoke is NULL! ===\n" );
                                    }
                                }
                                else
                                {
                                    ERR( "=== gatt_char_notification_thread: handler_obj or lpVtbl is NULL! ===\n" );
                                }
                            }
                            else
                            {
                                ERR( "=== gatt_char_notification_thread: value_changed_handler is NULL! ===\n" );
                            }
                            IGattValueChangedEventArgsVtbl *event_args_vtbl = (IGattValueChangedEventArgsVtbl*)event_args->lpVtbl;
                            event_args_vtbl->Release( event_args );
                        }
                        IBuffer_Release( ibuf );
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
                should_stop = impl->notification_thread_stop;
                if (should_stop) break;
                continue;
            }
            else
            {
                ERR( "=== gatt_char_notification_thread: IOCTL failed, err=%lu ===\n", err );
                
                if (handler) ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( handler );
                handler = NULL;
                Sleep( 100 );
                should_stop = impl->notification_thread_stop;
                if (should_stop) break;
                continue;
            }
        }
        
        if (handler) ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( handler );
    }

    free( params );
    ERR( "=== gatt_char_notification_thread: EXITING ===\n" );
    impl->IGattCharacteristic_iface.lpVtbl->Release( &impl->IGattCharacteristic_iface );
    return 0;
}

static HRESULT WINAPI gatt_char_add_ValueChanged( IGattCharacteristic *iface, ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs *handler,
                                                   EventRegistrationToken *token )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );
    LONG new_token;
    DWORD thread_id;

    ERR( "=== gatt_char_add_ValueChanged: iface=%p handler=%p char_handle=%u ===\n",
         iface, handler, impl->char_info.AttributeHandle );
    

    if (!token) return E_POINTER;
    if (!handler) return E_INVALIDARG;

    if (impl->value_changed_handler)
    {
        ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( impl->value_changed_handler );
        if (impl->notification_thread)
        {
            impl->notification_thread_stop = TRUE;
            WaitForSingleObject( impl->notification_thread, INFINITE );
            CloseHandle( impl->notification_thread );
            impl->notification_thread = NULL;
        }
    }

    impl->value_changed_handler = handler;
    ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_AddRef( handler );

    new_token = InterlockedIncrement( &impl->next_token );
    impl->value_changed_token.value = new_token;
    token->value = new_token;

    impl->notification_thread_stop = FALSE;
    impl->notification_thread = CreateThread( NULL, 0, gatt_char_notification_thread, impl, 0, &thread_id );
    if (!impl->notification_thread)
    {
        ERR( "=== gatt_char_add_ValueChanged: CreateThread FAILED: err=%lu ===\n", GetLastError() );
        
    }
    else
    {
        ERR( "=== gatt_char_add_ValueChanged: notification thread created, handle=%p ===\n", impl->notification_thread );
        
    }

    ERR( "=== gatt_char_add_ValueChanged: registered handler, token=%I64d ===\n", token->value );
    return S_OK;
}

static HRESULT WINAPI gatt_char_remove_ValueChanged( IGattCharacteristic *iface, EventRegistrationToken token )
{
    struct gatt_characteristic *impl = impl_from_IGattCharacteristic( iface );

    ERR( "=== gatt_char_remove_ValueChanged: iface=%p token=%I64d ===\n", iface, token.value );

    if (impl->value_changed_handler && impl->value_changed_token.value == token.value)
    {
        ITypedEventHandler_GattCharacteristic_GattValueChangedEventArgs_Release( impl->value_changed_handler );
        impl->value_changed_handler = NULL;
        impl->value_changed_token.value = 0;
        if (impl->notification_thread)
        {
            impl->notification_thread_stop = TRUE;
            WaitForSingleObject( impl->notification_thread, INFINITE );
            CloseHandle( impl->notification_thread );
            impl->notification_thread = NULL;
        }
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

static HRESULT gatt_characteristic_create( HANDLE device_handle, const BTH_LE_GATT_SERVICE *service,
                                           const BTH_LE_GATT_CHARACTERISTIC *char_info, IGattCharacteristic **out )
{
    struct gatt_characteristic *impl;
    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattCharacteristic_iface.lpVtbl = &gatt_characteristic_vtbl;
    impl->ref = 1;
    impl->device_handle = device_handle;
    impl->service_info = *service;
    impl->char_info = *char_info;
    ERR( "=== Created IGattCharacteristic: uuid=%s handle=%u ===\n",
         debugstr_guid( &char_info->CharacteristicUuid.Value.LongUuid ), char_info->AttributeHandle );
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
    ERR( "=== gatt_chars_iterator_get_Current: iface=%p index=%lu size=%lu ===\n", iface, impl->index, impl->size );
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
    ERR( "=== gatt_chars_iterator_MoveNext: iface=%p index=%lu size=%lu ===\n", iface, impl->index, impl->size );
    TRACE( "(%p, %p)\n", iface, value );
    
    if (!value) return E_POINTER;
    if (impl->index < impl->size) impl->index++;
    HRESULT hr = gatt_chars_iterator_get_HasCurrent( iface, value );
    
    return hr;
}

static HRESULT WINAPI gatt_chars_iterator_GetMany( IIterator_GattCharacteristic *iface, UINT32 items_size,
                                                    IGattCharacteristic **items, UINT32 *count )
{
    struct gatt_chars_iterator *impl = impl_from_IIterator_GattCharacteristic( iface );
    TRACE( "(%p, %lu, %p, %p)\n", iface, items_size, items, count );
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
    BTH_LE_GATT_SERVICE service_info;
    HANDLE device_handle;
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
    ERR( "=== gatt_chars_iterable_First: iface=%p count=%lu ===\n", iface, impl->count );
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
    ERR( "=== gatt_chars_vector_QueryInterface: iid=%s ===\n", debugstr_guid( iid ) );
    FIXME( "gatt_chars_vector_QueryInterface: iid=%s\n", debugstr_guid( iid ) );
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
    ERR( "=== gatt_chars_vector_QueryInterface: FAILED for iid=%s ===\n", debugstr_guid( iid ) );
    FIXME( "gatt_chars_vector_QueryInterface FAILED: iid=%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
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
    ERR( "=== gatt_chars_vector_GetAt: index=%lu, count=%lu ===\n", index, impl->count );
    TRACE( "(%p, %lu, %p)\n", iface, index, value );
    if (!value) return E_POINTER;
    if (index >= impl->count)
    {
        ERR( "=== gatt_chars_vector_GetAt: index %lu >= count %lu, returning E_BOUNDS ===\n", index, impl->count );
        return E_BOUNDS;
    }
    hr = gatt_characteristic_create( impl->device_handle, &impl->service_info, &impl->characteristics[index], value );
    ERR( "=== gatt_chars_vector_GetAt: created characteristic, hr=0x%08lx ===\n", hr );
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

    ERR( "=== gatt_chars_vector_GetMany: start_index=%lu, items_size=%lu, count=%lu ===\n", start_index, items_size, impl->count );
    TRACE( "(%p, %lu, %lu, %p, %p)\n", iface, start_index, items_size, items, count );
    if (!count) return E_POINTER;
    if (!items) return E_POINTER;

    if (start_index >= impl->count)
    {
        ERR( "=== gatt_chars_vector_GetMany: start_index %lu >= count %lu, returning 0 ===\n", start_index, impl->count );
        *count = 0;
        return S_OK;
    }

    UINT32 available = impl->count - start_index;
    UINT32 to_copy = (items_size < available) ? items_size : available;
    ERR( "=== gatt_chars_vector_GetMany: copying %lu items ===\n", to_copy );

    for (i = 0; i < to_copy; i++)
    {
        hr = gatt_characteristic_create( impl->device_handle, &impl->service_info, &impl->characteristics[start_index + i], &items[i] );
        ERR( "=== gatt_chars_vector_GetMany: char[%lu] create hr=0x%08lx ===\n", start_index + i, hr );
        if (FAILED( hr ))
        {
            ERR( "=== gatt_chars_vector_GetMany: FAILED creating char[%lu], cleaning up ===\n", start_index + i );
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

    ERR( "=== gatt_chars_vector_GetMany: SUCCESS, returning %lu items ===\n", to_copy );
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

static HRESULT gatt_characteristics_vector_create( const BTH_LE_GATT_CHARACTERISTIC *characteristics, ULONG count,
                                                   const BTH_LE_GATT_SERVICE *service, HANDLE device_handle,
                                                   IVectorView_GattCharacteristic **out )
{
    struct gatt_characteristics_vector *impl;
    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IVectorView_GattCharacteristic_iface.lpVtbl = &gatt_characteristics_vector_vtbl;
    impl->IIterable_GattCharacteristic_iface.lpVtbl = &gatt_chars_iterable_vtbl;
    impl->ref = 1;
    impl->count = count;
    impl->device_handle = device_handle;
    impl->service_info = *service;
    if (count > 0)
    {
        impl->characteristics = malloc( count * sizeof( BTH_LE_GATT_CHARACTERISTIC ) );
        if (!impl->characteristics)
        {
            free( impl );
            return E_OUTOFMEMORY;
        }
        memcpy( impl->characteristics, characteristics, count * sizeof( BTH_LE_GATT_CHARACTERISTIC ) );
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
    ERR( "=== gatt_chars_result_get_Characteristics: iface=%p chars_vector=%p ===\n", iface, impl->chars_vector );
    if (!value) return E_POINTER;
    if (impl->chars_vector)
    {
        IVectorView_GattCharacteristic_AddRef( impl->chars_vector );
        *value = impl->chars_vector;
    }
    else
        *value = NULL;
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
        *value = NULL;
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

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattValueChangedEventArgs_iface.lpVtbl = &gatt_value_changed_event_args_vtbl;
    impl->ref = 1;
    impl->value = value;
    if (value) IBuffer_AddRef( value );

    GetSystemTimeAsFileTime( &ft );
    impl->timestamp.UniversalTime = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;

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
    FIXME( "(%p, %p): stub!\n", iface, value );
    if (!value) return E_POINTER;
    *value = NULL;
    return E_NOTIMPL;
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
    FIXME( "(%p, %p): stub!\n", iface, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service3_OpenAsync( IGattDeviceService3 *iface, GattSharingMode mode,
                                                IAsyncOperation_GattOpenStatus **async )
{
    FIXME( "(%p, %d, %p): stub!\n", iface, mode, async );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service3_GetCharacteristicsWithCacheModeAsync( IGattDeviceService3 *iface,
                                                                           BluetoothCacheMode mode,
                                                                           IAsyncOperation_GattCharacteristicsResult **operation )
{
    struct gatt_device_service *impl = impl_from_IGattDeviceService3( iface );
    struct winebth_le_device_get_gatt_characteristics_params *params;
    IVectorView_GattCharacteristic *chars_vector = NULL;
    IGattCharacteristicsResult *result;
    GattCommunicationStatus status;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    HRESULT hr;
    int retry;

    ERR( "=== gatt_service3_GetCharacteristicsWithCacheModeAsync called: iface=%p mode=%d device_handle=%p service_handle=%u ===\n",
         iface, mode, impl->device_handle, impl->service_info.AttributeHandle );
    if (!operation) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        ERR( "=== Device handle is invalid ===\n" );
        status = GattCommunicationStatus_Unreachable;
        hr = gatt_characteristics_result_create( status, NULL, &result );
        if (FAILED( hr )) return hr;
        hr = async_gatt_chars_op_create( result, operation );
        IGattCharacteristicsResult_Release( result );
        return hr;
    }

    buffer_size = offsetof( struct winebth_le_device_get_gatt_characteristics_params, characteristics[32] );
    params = malloc( buffer_size );
    if (!params) return E_OUTOFMEMORY;

    ERR( "=== Calling IOCTL_WINEBTH_LE_DEVICE_GET_GATT_CHARACTERISTICS for service uuid=%s ===\n",
         debugstr_guid( &impl->service_info.ServiceUuid.Value.LongUuid ) );
    

    status = GattCommunicationStatus_Unreachable;
    for (retry = 0; retry < 30; retry++)
    {
        memset( params, 0, buffer_size );
        params->service = impl->service_info;
        params->count = 0;

        if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_CHARACTERISTICS,
                             &impl->service_info, sizeof( impl->service_info ),
                             params, buffer_size, &bytes_returned, NULL ))
        {
            ERR( "=== IOCTL succeeded (retry %d): count=%lu bytes_returned=%lu ===\n",
                 retry, params->count, bytes_returned );
            
            status = GattCommunicationStatus_Success;
            if (params->count > 0)
            {
                hr = gatt_characteristics_vector_create( params->characteristics, params->count,
                                                          &impl->service_info, impl->device_handle, &chars_vector );
                if (FAILED( hr ))
                {
                    ERR( "=== Failed to create characteristics vector: hr=0x%lx ===\n", hr );
                    
                    free( params );
                    return hr;
                }
                ERR( "=== Created characteristics vector with %lu characteristics ===\n", params->count );
                
                break;
            }
            else
            {
                ERR( "=== IOCTL returned success but count=0 (retry %d) ===\n", retry );
                
            }
            Sleep( 100 );
        }
        else
        {
            DWORD err = GetLastError();
            ERR( "=== IOCTL failed (retry %d): error=%lu ===\n", retry, err );
            
            Sleep( 100 );
        }
    }

    free( params );

    hr = gatt_characteristics_result_create( status, chars_vector, &result );
    if (chars_vector)
        IVectorView_GattCharacteristic_Release( chars_vector );
    if (FAILED( hr )) return hr;

    hr = async_gatt_chars_op_create( result, operation );
    IGattCharacteristicsResult_Release( result );
    return hr;
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
    FIXME( "(%p, %s, %p): stub!\n", iface, debugstr_guid( &uuid ), async );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_service3_GetCharacteristicsForUuidWithCacheModeAsync( IGattDeviceService3 *iface,
                                                                                  GUID uuid, BluetoothCacheMode mode,
                                                                                  IAsyncOperation_GattCharacteristicsResult **async )
{
    FIXME( "(%p, %s, %d, %p): stub!\n", iface, debugstr_guid( &uuid ), mode, async );
    return E_NOTIMPL;
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

static HRESULT gatt_device_service_create( HANDLE device_handle, const BTH_LE_GATT_SERVICE *service_info,
                                            const WCHAR *device_id, IGattDeviceService **out )
{
    struct gatt_device_service *impl;
    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IGattDeviceService_iface.lpVtbl = &gatt_device_service_vtbl;
    impl->IGattDeviceService3_iface.lpVtbl = &gatt_device_service3_vtbl;
    impl->IClosable_iface.lpVtbl = &service_closable_vtbl;
    impl->ref = 1;
    impl->device_handle = device_handle;
    impl->service_info = *service_info;
    if (device_id)
    {
        impl->device_id = wcsdup( device_id );
        if (!impl->device_id)
        {
            free( impl );
            return E_OUTOFMEMORY;
        }
    }
    ERR( "=== Created IGattDeviceService: uuid=%s handle=%u ===\n",
         debugstr_guid( &service_info->ServiceUuid.Value.LongUuid ), service_info->AttributeHandle );
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
    struct gatt_services_iterator *impl = impl_from_IIterator_GattDeviceService( iface );
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
    ERR( "=== gatt_services_iterator_get_Current: iface=%p index=%lu size=%lu ===\n", iface, impl->index, impl->size );
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
    ERR( "=== gatt_services_iterator_MoveNext: iface=%p index=%lu size=%lu ===\n", iface, impl->index, impl->size );
    TRACE( "(%p, %p)\n", iface, value );
    
    if (!value) return E_POINTER;
    if (impl->index < impl->size) impl->index++;
    HRESULT hr = gatt_services_iterator_get_HasCurrent( iface, value );
    
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
    WCHAR *device_id;
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
    ERR( "=== gatt_services_iterable_First: iface=%p count=%lu ===\n", iface, impl->count );
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
    ERR( "=== gatt_services_vector_QueryInterface: iid=%s ===\n", debugstr_guid( iid ) );
    FIXME( "gatt_services_vector_QueryInterface: iid=%s\n", debugstr_guid( iid ) );
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
    ERR( "=== gatt_services_vector_QueryInterface: FAILED for iid=%s ===\n", debugstr_guid( iid ) );
    FIXME( "gatt_services_vector_QueryInterface FAILED: iid=%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
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
    ERR( "=== gatt_services_vector_GetAt: index=%u, count=%lu ===\n", index, impl->count );
    TRACE( "(%p, %u, %p)\n", iface, index, value );
    if (!value) return E_POINTER;
    if (index >= impl->count)
    {
        ERR( "=== gatt_services_vector_GetAt: index %u >= count %lu, returning E_BOUNDS ===\n", index, impl->count );
        return E_BOUNDS;
    }
    hr = gatt_device_service_create( impl->device_handle, &impl->services[index], impl->device_id, value );
    ERR( "=== gatt_services_vector_GetAt: created service, hr=0x%08lx ===\n", hr );
    return hr;
}

static HRESULT WINAPI gatt_services_vector_get_Size( IVectorView_GattDeviceService *iface, UINT32 *value )
{
    struct gatt_services_vector *impl = impl_from_IVectorView_GattDeviceService( iface );
    ERR( "=== gatt_services_vector_get_Size: count=%lu ===\n", impl->count );
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
    UINT32 i;
    HRESULT hr;

    TRACE( "(%p, %u, %u, %p, %p)\n", iface, start_index, items_size, items, value );
    if (!value) return E_POINTER;
    if (!items) return E_POINTER;

    if (start_index >= impl->count)
    {
        *value = 0;
        return S_OK;
    }

    UINT32 available = impl->count - start_index;
    UINT32 to_copy = (items_size < available) ? items_size : available;

    for (i = 0; i < to_copy; i++)
    {
        hr = gatt_device_service_create( impl->device_handle, &impl->services[start_index + i], impl->device_id, &items[i] );
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
                                             HANDLE device_handle, const WCHAR *device_id,
                                             IVectorView_GattDeviceService **out )
{
    struct gatt_services_vector *impl;
    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IVectorView_GattDeviceService_iface.lpVtbl = &gatt_services_vector_vtbl;
    impl->IIterable_GattDeviceService_iface.lpVtbl = &gatt_services_iterable_vtbl;
    impl->ref = 1;
    impl->count = count;
    impl->device_handle = device_handle;
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
};

static inline struct gatt_services_result *impl_from_IGattDeviceServicesResult( IGattDeviceServicesResult *iface )
{
    return CONTAINING_RECORD( iface, struct gatt_services_result, IGattDeviceServicesResult_iface );
}

static HRESULT WINAPI gatt_result_QueryInterface( IGattDeviceServicesResult *iface, REFIID iid, void **out )
{
    struct gatt_services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    ERR( "=== gatt_result_QueryInterface: iface=%p iid=%s ===\n", iface, debugstr_guid( iid ) );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IGattDeviceServicesResult ))
    {
        IGattDeviceServicesResult_AddRef( (*out = &impl->IGattDeviceServicesResult_iface) );
        ERR( "=== gatt_result_QueryInterface: SUCCESS ===\n" );
        return S_OK;
    }
    *out = NULL;
    ERR( "=== gatt_result_QueryInterface: E_NOINTERFACE for %s ===\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI gatt_result_AddRef( IGattDeviceServicesResult *iface )
{
    struct gatt_services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    ERR( "=== gatt_result_AddRef: iface=%p ref=%lu ===\n", iface, ref );
    return ref;
}

static ULONG WINAPI gatt_result_Release( IGattDeviceServicesResult *iface )
{
    struct gatt_services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    ERR( "=== gatt_result_Release: iface=%p ref=%lu ===\n", iface, ref );
    if (!ref)
    {
        ERR( "=== gatt_result_Release: DESTROYING result object! ===\n" );
        if (impl->services_vector)
            IVectorView_GattDeviceService_Release( impl->services_vector );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI gatt_result_GetIids( IGattDeviceServicesResult *iface, ULONG *iid_count, IID **iids )
{
    ERR( "=== gatt_result_GetIids: iface=%p ===\n", iface );
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI gatt_result_GetRuntimeClassName( IGattDeviceServicesResult *iface, HSTRING *class_name )
{
    ERR( "=== gatt_result_GetRuntimeClassName: iface=%p ===\n", iface );
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattDeviceServicesResult";
    TRACE( "(%p, %p)\n", iface, class_name );
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI gatt_result_GetTrustLevel( IGattDeviceServicesResult *iface, TrustLevel *level )
{
    ERR( "=== gatt_result_GetTrustLevel: iface=%p ===\n", iface );
    TRACE( "(%p, %p)\n", iface, level );
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI gatt_result_get_Status( IGattDeviceServicesResult *iface, GattCommunicationStatus *value )
{
    struct gatt_services_result *impl = impl_from_IGattDeviceServicesResult( iface );
    ERR( "=== gatt_result_get_Status: iface=%p status=%d (1=Success, 2=Unreachable, 3=ProtocolError, 4=AccessDenied) ===\n", iface, impl->status );
    FIXME( "gatt_result_get_Status: iface=%p status=%d\n", iface, impl->status );
    
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
    ERR( "=== gatt_result_get_Services: iface=%p services_vector=%p ===\n", iface, impl->services_vector );
    FIXME( "gatt_result_get_Services: iface=%p services_vector=%p\n", iface, impl->services_vector );
    
    if (!value) return E_POINTER;
    if (impl->services_vector)
    {
        IVectorView_GattDeviceService_AddRef( impl->services_vector );
        *value = impl->services_vector;
        ERR( "=== gatt_result_get_Services: returning services_vector with %lu services ===\n", 
             ((struct gatt_services_vector *)impl_from_IVectorView_GattDeviceService( impl->services_vector ))->count );
    }
    else
    {
        ERR( "=== gatt_result_get_Services: services_vector is NULL! ===\n" );
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
    ERR( "=== async_gatt_op_QueryInterface: iface=%p iid=%s ===\n", iface, debugstr_guid( iid ) );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_GattDeviceServicesResult ))
    {
        IAsyncOperation_GattDeviceServicesResult_AddRef( (*out = &impl->IAsyncOperation_GattDeviceServicesResult_iface) );
        ERR( "=== async_gatt_op_QueryInterface: returning IAsyncOperation_GattDeviceServicesResult ===\n" );
        return S_OK;
    }
    if (IsEqualGUID( iid, &IID_IAsyncInfo ))
    {
        IAsyncInfo_AddRef( (*out = &impl->IAsyncInfo_iface) );
        ERR( "=== async_gatt_op_QueryInterface: returning IAsyncInfo ===\n" );
        return S_OK;
    }
    *out = NULL;
    ERR( "=== async_gatt_op_QueryInterface: E_NOINTERFACE for %s ===\n", debugstr_guid( iid ) );
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
    FIXME( "(%p, %p, %p): stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
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

    ERR( "=== put_Completed: iface=%p handler=%p current_status=%d ===\n", iface, handler, impl->status );
    if (!handler) return E_POINTER;

    impl->handler = handler;
    IAsyncOperationCompletedHandler_GattDeviceServicesResult_AddRef( handler );

    current_status = impl->status;
    if (current_status != Started)
    {
        ERR( "=== put_Completed: Operation already complete (status=%d), invoking handler immediately ===\n", current_status );
        IAsyncOperationCompletedHandler_GattDeviceServicesResult_Invoke( handler, iface, current_status );
    }
    else
    {
        ERR( "=== put_Completed: Operation still running, handler will be called by background thread ===\n" );
    }

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
    ERR( "=== async_gatt_op_GetResults: iface=%p impl->result=%p impl->status=%d ===\n", iface, impl->result, impl->status );
    
    if (!results) return E_POINTER;
    *results = impl->result;
    if (*results) IGattDeviceServicesResult_AddRef( *results );
    ERR( "=== async_gatt_op_GetResults: returning result=%p ===\n", *results );
    
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
    ERR( "=== async_gatt_info_get_Status: iface=%p status=%d ===\n", iface, impl->status );
    *status = impl->status;
    return S_OK;
}

static HRESULT WINAPI async_gatt_info_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    ERR( "=== async_gatt_info_get_ErrorCode: iface=%p returning S_OK ===\n", iface );
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
    FIXME( "(%p, %p) stub!\n", iface, value );
    *value = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI le_device3_RequestAccessAsync( IBluetoothLEDevice3 *iface, IAsyncOperation_IInspectable **operation )
{
    FIXME( "(%p, %p) stub!\n", iface, operation );
    return E_NOTIMPL;
}

struct gatt_services_async_ctx
{
    IBluetoothLEDevice3 *iface;
    IAsyncOperation_GattDeviceServicesResult *op;
    BluetoothCacheMode mode;
};

static DWORD WINAPI gatt_services_thread( void *arg )
{
    struct gatt_services_async_ctx *ctx = arg;
    struct bluetooth_le_device *impl = impl_from_IBluetoothLEDevice3( ctx->iface );
    struct async_gatt_services_op *op_impl = impl_from_IAsyncOperation_GattDeviceServicesResult( ctx->op );
    struct winebth_le_device_get_gatt_services_params *params = NULL;
    IVectorView_GattDeviceService *services_vector = NULL;
    IGattDeviceServicesResult *result;
    GattCommunicationStatus status = GattCommunicationStatus_Unreachable;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    HRESULT hr;
    int retries = 0;
    const int max_retries = 20;

    FIXME( "=== gatt_services_thread: STARTED device=%p handle=%p ===\n", impl, impl->device_handle );

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        FIXME( "=== gatt_services_thread: Device handle INVALID! ===\n" );
        goto done;
    }

    buffer_size = offsetof( struct winebth_le_device_get_gatt_services_params, services[16] );
    params = malloc( buffer_size );
    if (!params) goto done;

retry:
    memset( params, 0, buffer_size );
    params->count = 0;

    FIXME( "=== gatt_services_thread: calling IOCTL... ===\n" );
    if (DeviceIoControl( impl->device_handle, IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES, NULL, 0,
                         params, buffer_size, &bytes_returned, NULL ))
    {
        FIXME( "=== gatt_services_thread: IOCTL OK count=%lu mode=%d retry=%d ===\n", params->count, ctx->mode, retries );
        status = GattCommunicationStatus_Success;

        if (ctx->mode == BluetoothCacheMode_Uncached && params->count == 0 && retries < max_retries)
        {
             TRACE( "Uncached mode and 0 services, waiting... (retry %d/%d)\n", retries+1, max_retries );
             Sleep( 100 );
             retries++;
             goto retry;
        }

        if (params->count > 0)
        {
            hr = gatt_services_vector_create( params->services, params->count, impl->device_handle,
                                               WindowsGetStringRawBuffer( impl->id, NULL ), &services_vector );
            if (FAILED( hr ))
            {
                ERR( "Failed to create services vector: hr=0x%lx\n", hr );
                status = GattCommunicationStatus_Unreachable;
            }
        }
    }
    else
    {
        DWORD err = GetLastError();
        ERR( "IOCTL failed: error=%lu\n", err );
        status = GattCommunicationStatus_Unreachable;
    }

done:
    FIXME( "=== gatt_services_thread: done, gatt_status=%d ===\n", status );
    if (params) free( params );

    hr = gatt_services_result_create( status, services_vector, &result );
    if (services_vector) IVectorView_GattDeviceService_Release( services_vector );

    if (SUCCEEDED(hr))
    {
        op_impl->result = result;
        op_impl->status = Completed;
        FIXME( "=== gatt_services_thread: async_status=Completed ===\n" );
    }
    else
    {
        op_impl->status = Error;
        FIXME( "=== gatt_services_thread: async_status=Error hr=0x%lx ===\n", hr );
    }

    FIXME( "=== gatt_services_thread: handler=%p result=%p status=%d ===\n", op_impl->handler, op_impl->result, op_impl->status );
    if (op_impl->handler)
    {
        HRESULT invoke_hr;
        FIXME( "=== gatt_services_thread: invoking handler ctx->op=%p op_impl->status=%d... ===\n", ctx->op, op_impl->status );
        invoke_hr = IAsyncOperationCompletedHandler_GattDeviceServicesResult_Invoke( op_impl->handler, ctx->op, op_impl->status );
        FIXME( "=== gatt_services_thread: handler returned hr=0x%lx ===\n", invoke_hr );
    }
    else
    {
        FIXME( "=== gatt_services_thread: handler is NULL ===\n" );
    }

    FIXME( "=== gatt_services_thread: DONE final_status=%d ===\n", op_impl->status );
    IBluetoothLEDevice3_Release( ctx->iface );
    IAsyncOperation_GattDeviceServicesResult_Release( ctx->op );
    free( ctx );
    return 0;
}

static HRESULT WINAPI le_device3_GetGattServicesWithCacheModeAsync( IBluetoothLEDevice3 *iface,
                                                                    BluetoothCacheMode cacheMode,
                                                                    IAsyncOperation_GattDeviceServicesResult **operation )
{
    struct gatt_services_async_ctx *ctx;
    HANDLE thread;
    HRESULT hr;

    FIXME( "=== GetGattServicesWithCacheModeAsync: iface=%p, cacheMode=%d ===\n", iface, cacheMode );
    if (!operation) return E_POINTER;

    hr = async_gatt_services_op_create( NULL, operation );
    if (FAILED( hr )) {
        FIXME( "=== GetGattServicesWithCacheModeAsync: async_gatt_services_op_create FAILED: hr=0x%lx ===\n", hr );
        return hr;
    }

    ctx = malloc( sizeof( *ctx ) );
    if (!ctx)
    {
        IAsyncOperation_GattDeviceServicesResult_Release( *operation );
        return E_OUTOFMEMORY;
    }

    ctx->iface = iface;
    IBluetoothLEDevice3_AddRef( iface );
    ctx->op = *operation;
    IAsyncOperation_GattDeviceServicesResult_AddRef( *operation );
    ctx->mode = cacheMode;

    FIXME( "=== GetGattServicesWithCacheModeAsync: creating thread... ===\n" );
    thread = CreateThread( NULL, 0, gatt_services_thread, ctx, 0, NULL );
    if (!thread)
    {
        DWORD err = GetLastError();
        FIXME( "=== GetGattServicesWithCacheModeAsync: CreateThread FAILED: err=%lu ===\n", err );
        IBluetoothLEDevice3_Release( iface );
        IAsyncOperation_GattDeviceServicesResult_Release( *operation );
        free( ctx );
        return HRESULT_FROM_WIN32( err );
    }
    FIXME( "=== GetGattServicesWithCacheModeAsync: thread created, handle=%p ===\n", thread );
    CloseHandle( thread );

    return S_OK;
}

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
    FIXME( "(%p, %s, %p) stub!\n", iface, debugstr_guid( &serviceUuid ), operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI le_device3_GetGattServicesForUuidWithCacheModeAsync( IBluetoothLEDevice3 *iface,
                                                                           GUID serviceUuid,
                                                                           BluetoothCacheMode cacheMode,
                                                                           IAsyncOperation_GattDeviceServicesResult **operation )
{
    FIXME( "(%p, %s, %d, %p) stub!\n", iface, debugstr_guid( &serviceUuid ), cacheMode, operation );
    return E_NOTIMPL;
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
    le_device3_RequestAccessAsync,
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

    swprintf( addr_str, ARRAY_SIZE(addr_str), L"%012I64x", addr_48 );
    swprintf( addr_str_swapped, ARRAY_SIZE(addr_str_swapped), L"%012I64x", addr_swapped );

    ERR( "=== open_le_device_interface: searching for address %I64x (48-bit: %I64x, str: %s, swapped: %s) ===\n", address, addr_48, debugstr_w( addr_str ), debugstr_w( addr_str_swapped ) );

    devinfo = SetupDiGetClassDevsW( &btle_device_interface_guid, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE );
    if (devinfo == INVALID_HANDLE_VALUE)
    {
        ERR( "=== SetupDiGetClassDevsW failed ===\n" );
        return INVALID_HANDLE_VALUE;
    }

    iface_detail->cbSize = sizeof( *iface_detail );
    iface_data.cbSize = sizeof( iface_data );

    while (SetupDiEnumDeviceInterfaces( devinfo, NULL, &btle_device_interface_guid, idx++, &iface_data ))
    {
        if (!SetupDiGetDeviceInterfaceDetailW( devinfo, &iface_data, iface_detail, sizeof( buffer ), NULL, NULL ))
            continue;

        ERR( "=== Found device interface path: %s ===\n", debugstr_w( iface_detail->DevicePath ) );

        {
            const WCHAR *addr_ptr = wcsstr( iface_detail->DevicePath, L"&" );
            if (!addr_ptr) addr_ptr = wcsstr( iface_detail->DevicePath, L"_" );
            if (addr_ptr)
            {
                addr_ptr++;
                WCHAR path_addr[32] = {0};
                lstrcpynW( path_addr, addr_ptr, 13 );
                ERR( "=== Comparing path address '%s' with search addresses '%s' or '%s' ===\n", debugstr_w( path_addr ), debugstr_w( addr_str ), debugstr_w( addr_str_swapped ) );
                if (_wcsicmp( path_addr, addr_str ) == 0 || _wcsicmp( path_addr, addr_str_swapped ) == 0)
                {
                    ERR( "=== Address match found! Opening device interface ===\n" );
                    device = CreateFileW( iface_detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL );
                    if (device != INVALID_HANDLE_VALUE)
                    {
                        ERR( "=== Successfully opened device handle: %p ===\n", device );
                        break;
                    }
                    else
                    {
                        ERR( "=== CreateFileW failed: %lu ===\n", GetLastError() );
                    }
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList( devinfo );

    if (device == INVALID_HANDLE_VALUE)
        ERR( "=== No matching device interface found for address %I64x ===\n", address );

    return device;
}

static HRESULT bluetooth_le_device_create( HSTRING id, UINT64 address, IBluetoothLEDevice **out )
{
    struct bluetooth_le_device *impl;
    WCHAR device_name[256];

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
    if (impl->device_handle == INVALID_HANDLE_VALUE)
        WARN( "Failed to open device interface for address %I64x\n", address );

    *out = &impl->IBluetoothLEDevice_iface;
    ERR( "=== Created BluetoothLEDevice %p, name=%s, address=%I64x, handle=%p ===\n", impl, debugstr_w( device_name ), address, impl->device_handle );
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

    ERR( "=== FromIdAsync CALLED: id=%s ===\n", debugstr_hstring( id ) );

    str = WindowsGetStringRawBuffer( id, NULL );
    addr_ptr = wcsstr( str, L"&" );
    if (addr_ptr) swscanf( addr_ptr + 1, L"%I64x", &addr );
    else if ((addr_ptr = wcsstr( str, L"_" ))) swscanf( addr_ptr + 1, L"%I64x", &addr );

    ERR( "=== FromIdAsync: parsed address=%I64x ===\n", addr );

    hr = bluetooth_le_device_create( id, addr, &device );
    if (FAILED(hr))
    {
        ERR( "=== FromIdAsync: bluetooth_le_device_create failed: 0x%08lx ===\n", hr );
        return hr;
    }

    ERR( "=== FromIdAsync: device created successfully ===\n" );

    hr = async_le_device_op_create( device, async_op );
    IBluetoothLEDevice_Release( device );
    ERR( "=== FromIdAsync: returning hr=0x%08lx ===\n", hr );
    return hr;
}

static HRESULT WINAPI bluetoothledevice_statics_FromBluetoothAddressAsync( IBluetoothLEDeviceStatics *iface, UINT64 addr, IAsyncOperation_BluetoothLEDevice **async_op )
{
    HRESULT hr;
    IBluetoothLEDevice *device;
    WCHAR buf[64];
    HSTRING id;

    ERR( "=== FromBluetoothAddressAsync CALLED: addr=%I64x ===\n", addr );

    swprintf( buf, ARRAY_SIZE(buf), L"BluetoothLE#BluetoothLE%012I64x", addr );
    WindowsCreateString( buf, wcslen(buf), &id );

    hr = bluetooth_le_device_create( id, addr, &device );
    WindowsDeleteString( id );
    if (FAILED(hr))
    {
        ERR( "=== FromBluetoothAddressAsync: bluetooth_le_device_create failed: 0x%08lx ===\n", hr );
        return hr;
    }

    ERR( "=== FromBluetoothAddressAsync: device created successfully ===\n" );

    hr = async_le_device_op_create( device, async_op );
    IBluetoothLEDevice_Release( device );
    ERR( "=== FromBluetoothAddressAsync: returning hr=0x%08lx ===\n", hr );
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
    ERR( "=== statics2_GetDeviceSelectorFromPairingState CALLED: iface=%p paired=%d result=%p ===\n", iface, paired, result );
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
    ERR( "=== statics2_GetDeviceSelectorFromConnectionStatus CALLED: iface=%p status=%d result=%p ===\n", iface, status, result );
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
    ERR( "=== statics2_GetDeviceSelectorFromDeviceName CALLED: iface=%p name=%s result=%p ===\n", iface, debugstr_hstring(name), result );
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
    ERR( "=== statics2_GetDeviceSelectorFromBluetoothAddress CALLED: iface=%p addr=%I64x result=%p ===\n", iface, addr, result );
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
    ERR( "=== statics2_GetDeviceSelectorFromBluetoothAddressWithBluetoothAddressType CALLED: iface=%p addr=%I64x type=%d result=%p ===\n", iface, addr, type, result );
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

    ERR( "=== FromBluetoothAddressWithBluetoothAddressTypeAsync CALLED: addr=%I64x, type=%d ===\n", addr, type );

    if (!async_op) return E_POINTER;

    swprintf( buf, ARRAY_SIZE(buf), L"BluetoothLE#BluetoothLE%012I64x", addr );
    WindowsCreateString( buf, wcslen(buf), &id );

    hr = bluetooth_le_device_create( id, addr, &device );
    WindowsDeleteString( id );
    if (FAILED(hr))
    {
        ERR( "=== FromBluetoothAddressWithBluetoothAddressTypeAsync: bluetooth_le_device_create failed: 0x%08lx ===\n", hr );
        return hr;
    }

    ERR( "=== FromBluetoothAddressWithBluetoothAddressTypeAsync: device created successfully ===\n" );

    hr = async_le_device_op_create( device, async_op );
    IBluetoothLEDevice_Release( device );
    ERR( "=== FromBluetoothAddressWithBluetoothAddressTypeAsync: returning hr=0x%08lx ===\n", hr );
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
