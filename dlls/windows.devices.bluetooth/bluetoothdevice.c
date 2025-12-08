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

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL( bluetooth );

static const GUID btle_device_interface_guid = { 0x781aee18, 0x7733, 0x4ce4, { 0xad, 0xd0, 0x91, 0xf4, 0x1c, 0x67, 0xb5, 0x92 } };
static const GUID btle_att_base_guid = { 0, 0, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };

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
    impl->status = Started;
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
    IClosable IClosable_iface;
    LONG ref;
    HSTRING id;
    UINT64 address;
    WCHAR name[256];
    HANDLE device_handle;
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
        if (impl->id) WindowsDeleteString( impl->id );
        if (impl->device_handle != INVALID_HANDLE_VALUE)
            CloseHandle( impl->device_handle );
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
    BTH_DEVICE_INFO_LIST *list;
    DWORD bytes_returned;
    SIZE_T buffer_size;
    UINT64 search_addr_48;
    DWORD i;
    BOOL found = FALSE;

    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;

    if (impl->device_handle == INVALID_HANDLE_VALUE)
    {
        *value = BluetoothConnectionStatus_Disconnected;
        return S_OK;
    }

    search_addr_48 = impl->address & 0xFFFFFFFFFFFFULL;

    buffer_size = sizeof(BTH_DEVICE_INFO_LIST) + sizeof(BTH_DEVICE_INFO) * 16;
    list = malloc( buffer_size );
    if (!list)
    {
        *value = BluetoothConnectionStatus_Disconnected;
        return E_OUTOFMEMORY;
    }

    memset( list, 0, buffer_size );
    list->numOfDevices = 16;

    if (DeviceIoControl( impl->device_handle, IOCTL_BTH_GET_DEVICE_INFO, NULL, 0,
                        list, buffer_size, &bytes_returned, NULL ))
    {
        for (i = 0; i < list->numOfDevices; i++)
        {
            if (list->deviceList[i].address == search_addr_48)
            {
                found = TRUE;
                if (list->deviceList[i].flags & BDIF_CONNECTED)
                    *value = BluetoothConnectionStatus_Connected;
                else
                    *value = BluetoothConnectionStatus_Disconnected;
                break;
            }
        }
    }

    free( list );

    if (!found)
        *value = BluetoothConnectionStatus_Disconnected;

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
    FIXME( "(%p, %p, %p): stub!\n", iface, handler, token );
    return S_OK;
}

static HRESULT WINAPI le_device_remove_NameChanged( IBluetoothLEDevice *iface, EventRegistrationToken token )
{
    FIXME( "(%p, %I64x): stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI le_device_add_GattServicesChanged( IBluetoothLEDevice *iface, ITypedEventHandler_BluetoothLEDevice_IInspectable *handler, EventRegistrationToken *token )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, handler, token );
    return S_OK;
}

static HRESULT WINAPI le_device_remove_GattServicesChanged( IBluetoothLEDevice *iface, EventRegistrationToken token )
{
    FIXME( "(%p, %I64x): stub!\n", iface, token.value );
    return S_OK;
}

static HRESULT WINAPI le_device_add_ConnectionStatusChanged( IBluetoothLEDevice *iface, ITypedEventHandler_BluetoothLEDevice_IInspectable *handler, EventRegistrationToken *token )
{
    FIXME( "(%p, %p, %p): stub!\n", iface, handler, token );
    return S_OK;
}

static HRESULT WINAPI le_device_remove_ConnectionStatusChanged( IBluetoothLEDevice *iface, EventRegistrationToken token )
{
    FIXME( "(%p, %I64x): stub!\n", iface, token.value );
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

static HANDLE open_le_device_interface( UINT64 address )
{
    BYTE buffer[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + MAX_PATH * sizeof( WCHAR )];
    SP_DEVICE_INTERFACE_DETAIL_DATA_W *iface_detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buffer;
    SP_DEVICE_INTERFACE_DATA iface_data;
    HDEVINFO devinfo;
    HANDLE device = INVALID_HANDLE_VALUE;
    DWORD idx = 0;
    WCHAR addr_str[32];
    UINT64 addr_48 = address & 0xFFFFFFFFFFFFULL;

    swprintf( addr_str, ARRAY_SIZE(addr_str), L"%012I64x", addr_48 );

    ERR( "=== open_le_device_interface: searching for address %I64x (48-bit: %I64x, str: %s) ===\n", address, addr_48, debugstr_w( addr_str ) );

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
                ERR( "=== Comparing path address '%s' with search address '%s' ===\n", debugstr_w( path_addr ), debugstr_w( addr_str ) );
                if (_wcsicmp( path_addr, addr_str ) == 0)
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
    impl->IClosable_iface.lpVtbl = &closable_vtbl;
    impl->ref = 1;
    impl->address = address;
    impl->device_handle = INVALID_HANDLE_VALUE;
    WindowsDuplicateString( id, &impl->id );

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
    impl->status = Started;
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
