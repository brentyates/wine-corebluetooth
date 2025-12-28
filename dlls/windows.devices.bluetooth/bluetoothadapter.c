/* WinRT Windows.Devices.Bluetooth BluetoothAdapter Implementation
 *
 * Copyright (C) 2023 Mohamad Al-Jaf
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
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(bluetooth);

struct bluetoothadapter
{
    IActivationFactory IActivationFactory_iface;
    IBluetoothAdapterStatics IBluetoothAdapterStatics_iface;
    LONG ref;
};

static inline struct bluetoothadapter *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct bluetoothadapter, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct bluetoothadapter *impl = impl_from_IActivationFactory( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IActivationFactory ))
    {
        *out = &impl->IActivationFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IBluetoothAdapterStatics ))
    {
        *out = &impl->IBluetoothAdapterStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct bluetoothadapter *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct bluetoothadapter *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IActivationFactory, &IID_IBluetoothAdapterStatics };
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
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.BluetoothAdapter";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    if (!trust_level) return E_POINTER;
    *trust_level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    TRACE( "iface %p, instance %p.\n", iface, instance );
    if (!instance) return E_POINTER;
    *instance = NULL;
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    /* IInspectable methods */
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    /* IActivationFactory methods */
    factory_ActivateInstance,
};

DEFINE_IINSPECTABLE( bluetoothadapter_statics, IBluetoothAdapterStatics, struct bluetoothadapter, IActivationFactory_iface )

struct bluetooth_adapter
{
    IBluetoothAdapter IBluetoothAdapter_iface;
    LONG ref;
    HSTRING id;
    UINT64 address;
};

static inline struct bluetooth_adapter *impl_from_IBluetoothAdapter( IBluetoothAdapter *iface )
{
    return CONTAINING_RECORD( iface, struct bluetooth_adapter, IBluetoothAdapter_iface );
}

static HRESULT WINAPI adapter_QueryInterface( IBluetoothAdapter *iface, REFIID iid, void **out )
{
    struct bluetooth_adapter *impl = impl_from_IBluetoothAdapter( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IBluetoothAdapter ))
    {
        IBluetoothAdapter_AddRef( (*out = &impl->IBluetoothAdapter_iface) );
        return S_OK;
    }

    *out = NULL;
    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    return E_NOINTERFACE;
}

static ULONG WINAPI adapter_AddRef( IBluetoothAdapter *iface )
{
    struct bluetooth_adapter *impl = impl_from_IBluetoothAdapter( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI adapter_Release( IBluetoothAdapter *iface )
{
    struct bluetooth_adapter *impl = impl_from_IBluetoothAdapter( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->id) WindowsDeleteString( impl->id );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI adapter_GetIids( IBluetoothAdapter *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IBluetoothAdapter };
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

static HRESULT WINAPI adapter_GetRuntimeClassName( IBluetoothAdapter *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Devices.Bluetooth.BluetoothAdapter";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI adapter_GetTrustLevel( IBluetoothAdapter *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI adapter_get_DeviceId( IBluetoothAdapter *iface, HSTRING *value )
{
    struct bluetooth_adapter *impl = impl_from_IBluetoothAdapter( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    return WindowsDuplicateString( impl->id, value );
}

static HRESULT WINAPI adapter_get_BluetoothAddress( IBluetoothAdapter *iface, UINT64 *value )
{
    struct bluetooth_adapter *impl = impl_from_IBluetoothAdapter( iface );
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = impl->address;
    return S_OK;
}

static HRESULT WINAPI adapter_get_IsClassicSupported( IBluetoothAdapter *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static HRESULT WINAPI adapter_get_IsLowEnergySupported( IBluetoothAdapter *iface, boolean *value )
{
    TRACE( "(%p, %p)\n", iface, value );
    if (!value) return E_POINTER;
    *value = TRUE;
    return S_OK;
}

static const IBluetoothAdapterVtbl adapter_vtbl =
{
    adapter_QueryInterface,
    adapter_AddRef,
    adapter_Release,
    adapter_GetIids,
    adapter_GetRuntimeClassName,
    adapter_GetTrustLevel,
    adapter_get_DeviceId,
    adapter_get_BluetoothAddress,
    adapter_get_IsClassicSupported,
    adapter_get_IsLowEnergySupported
};

static HRESULT bluetooth_adapter_create( HSTRING id, UINT64 address, IBluetoothAdapter **out )
{
    struct bluetooth_adapter *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IBluetoothAdapter_iface.lpVtbl = &adapter_vtbl;
    impl->ref = 1;
    impl->address = address;
    WindowsDuplicateString( id, &impl->id );

    *out = &impl->IBluetoothAdapter_iface;
    TRACE( "Created BluetoothAdapter %p\n", impl );
    return S_OK;
}

struct async_adapter_op
{
    IAsyncOperation_BluetoothAdapter IAsyncOperation_BluetoothAdapter_iface;
    IAsyncInfo IAsyncInfo_iface;
    LONG ref;
    IAsyncOperationCompletedHandler_BluetoothAdapter *handler;
    IBluetoothAdapter *result;
    AsyncStatus status;
};

static inline struct async_adapter_op *impl_from_IAsyncOperation_BluetoothAdapter( IAsyncOperation_BluetoothAdapter *iface )
{
    return CONTAINING_RECORD( iface, struct async_adapter_op, IAsyncOperation_BluetoothAdapter_iface );
}

static HRESULT WINAPI async_adapter_op_QueryInterface( IAsyncOperation_BluetoothAdapter *iface, REFIID iid, void **out )
{
    struct async_adapter_op *impl = impl_from_IAsyncOperation_BluetoothAdapter( iface );

    TRACE( "(%p, %s, %p)\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IAgileObject ) ||
        IsEqualGUID( iid, &IID_IAsyncOperation_BluetoothAdapter ))
    {
        IAsyncOperation_BluetoothAdapter_AddRef( (*out = &impl->IAsyncOperation_BluetoothAdapter_iface) );
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

static ULONG WINAPI async_adapter_op_AddRef( IAsyncOperation_BluetoothAdapter *iface )
{
    struct async_adapter_op *impl = impl_from_IAsyncOperation_BluetoothAdapter( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    return ref;
}

static ULONG WINAPI async_adapter_op_Release( IAsyncOperation_BluetoothAdapter *iface )
{
    struct async_adapter_op *impl = impl_from_IAsyncOperation_BluetoothAdapter( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "(%p) -> %lu\n", iface, ref );
    if (!ref)
    {
        if (impl->handler) IAsyncOperationCompletedHandler_BluetoothAdapter_Release( impl->handler );
        if (impl->result) IBluetoothAdapter_Release( impl->result );
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI async_adapter_op_GetIids( IAsyncOperation_BluetoothAdapter *iface, ULONG *iid_count, IID **iids )
{
    static const IID *iids_arr[] = { &IID_IUnknown, &IID_IInspectable, &IID_IAgileObject,
                                     &IID_IAsyncOperation_BluetoothAdapter, &IID_IAsyncInfo };
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

static HRESULT WINAPI async_adapter_op_GetRuntimeClassName( IAsyncOperation_BluetoothAdapter *iface, HSTRING *class_name )
{
    static const WCHAR class_name_str[] = L"Windows.Foundation.IAsyncOperation`1<Windows.Devices.Bluetooth.BluetoothAdapter>";
    if (!class_name) return E_POINTER;
    return WindowsCreateString( class_name_str, ARRAY_SIZE( class_name_str ) - 1, class_name );
}

static HRESULT WINAPI async_adapter_op_GetTrustLevel( IAsyncOperation_BluetoothAdapter *iface, TrustLevel *level )
{
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

static HRESULT WINAPI async_adapter_op_put_Completed( IAsyncOperation_BluetoothAdapter *iface,
                                                     IAsyncOperationCompletedHandler_BluetoothAdapter *handler )
{
    struct async_adapter_op *impl = impl_from_IAsyncOperation_BluetoothAdapter( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;
    impl->handler = handler;
    IAsyncOperationCompletedHandler_BluetoothAdapter_AddRef( handler );
    if (impl->status == Started)
        impl->status = Completed;
    IAsyncOperationCompletedHandler_BluetoothAdapter_Invoke( handler, iface, impl->status );
    return S_OK;
}

static HRESULT WINAPI async_adapter_op_get_Completed( IAsyncOperation_BluetoothAdapter *iface,
                                                     IAsyncOperationCompletedHandler_BluetoothAdapter **handler )
{
    struct async_adapter_op *impl = impl_from_IAsyncOperation_BluetoothAdapter( iface );
    TRACE( "(%p, %p)\n", iface, handler );
    if (!handler) return E_POINTER;
    *handler = impl->handler;
    if (*handler) IAsyncOperationCompletedHandler_BluetoothAdapter_AddRef( *handler );
    return S_OK;
}

static HRESULT WINAPI async_adapter_op_GetResults( IAsyncOperation_BluetoothAdapter *iface, IBluetoothAdapter **results )
{
    struct async_adapter_op *impl = impl_from_IAsyncOperation_BluetoothAdapter( iface );
    TRACE( "(%p, %p)\n", iface, results );
    if (!results) return E_POINTER;
    *results = impl->result;
    if (*results) IBluetoothAdapter_AddRef( *results );
    return S_OK;
}

static const IAsyncOperation_BluetoothAdapterVtbl async_adapter_op_vtbl =
{
    async_adapter_op_QueryInterface,
    async_adapter_op_AddRef,
    async_adapter_op_Release,
    async_adapter_op_GetIids,
    async_adapter_op_GetRuntimeClassName,
    async_adapter_op_GetTrustLevel,
    async_adapter_op_put_Completed,
    async_adapter_op_get_Completed,
    async_adapter_op_GetResults
};

static inline struct async_adapter_op *impl_from_IAsyncInfo_adapter( IAsyncInfo *iface )
{
    return CONTAINING_RECORD( iface, struct async_adapter_op, IAsyncInfo_iface );
}

static HRESULT WINAPI async_info_adapter_QueryInterface( IAsyncInfo *iface, REFIID iid, void **out )
{
    struct async_adapter_op *impl = impl_from_IAsyncInfo_adapter( iface );
    return IAsyncOperation_BluetoothAdapter_QueryInterface( &impl->IAsyncOperation_BluetoothAdapter_iface, iid, out );
}

static ULONG WINAPI async_info_adapter_AddRef( IAsyncInfo *iface )
{
    struct async_adapter_op *impl = impl_from_IAsyncInfo_adapter( iface );
    return IAsyncOperation_BluetoothAdapter_AddRef( &impl->IAsyncOperation_BluetoothAdapter_iface );
}

static ULONG WINAPI async_info_adapter_Release( IAsyncInfo *iface )
{
    struct async_adapter_op *impl = impl_from_IAsyncInfo_adapter( iface );
    return IAsyncOperation_BluetoothAdapter_Release( &impl->IAsyncOperation_BluetoothAdapter_iface );
}

static HRESULT WINAPI async_info_adapter_GetIids( IAsyncInfo *iface, ULONG *iid_count, IID **iids )
{
    struct async_adapter_op *impl = impl_from_IAsyncInfo_adapter( iface );
    return IAsyncOperation_BluetoothAdapter_GetIids( &impl->IAsyncOperation_BluetoothAdapter_iface, iid_count, iids );
}

static HRESULT WINAPI async_info_adapter_GetRuntimeClassName( IAsyncInfo *iface, HSTRING *class_name )
{
    struct async_adapter_op *impl = impl_from_IAsyncInfo_adapter( iface );
    return IAsyncOperation_BluetoothAdapter_GetRuntimeClassName( &impl->IAsyncOperation_BluetoothAdapter_iface, class_name );
}

static HRESULT WINAPI async_info_adapter_GetTrustLevel( IAsyncInfo *iface, TrustLevel *level )
{
    struct async_adapter_op *impl = impl_from_IAsyncInfo_adapter( iface );
    return IAsyncOperation_BluetoothAdapter_GetTrustLevel( &impl->IAsyncOperation_BluetoothAdapter_iface, level );
}

static HRESULT WINAPI async_info_adapter_get_Id( IAsyncInfo *iface, UINT32 *id )
{
    *id = 1;
    return S_OK;
}

static HRESULT WINAPI async_info_adapter_get_Status( IAsyncInfo *iface, AsyncStatus *status )
{
    struct async_adapter_op *impl = impl_from_IAsyncInfo_adapter( iface );
    *status = impl->status;
    return S_OK;
}

static HRESULT WINAPI async_info_adapter_get_ErrorCode( IAsyncInfo *iface, HRESULT *error_code )
{
    *error_code = S_OK;
    return S_OK;
}

static HRESULT WINAPI async_info_adapter_Cancel( IAsyncInfo *iface )
{
    return S_OK;
}

static HRESULT WINAPI async_info_adapter_Close( IAsyncInfo *iface )
{
    return S_OK;
}

static const IAsyncInfoVtbl async_info_adapter_vtbl =
{
    async_info_adapter_QueryInterface,
    async_info_adapter_AddRef,
    async_info_adapter_Release,
    async_info_adapter_GetIids,
    async_info_adapter_GetRuntimeClassName,
    async_info_adapter_GetTrustLevel,
    async_info_adapter_get_Id,
    async_info_adapter_get_Status,
    async_info_adapter_get_ErrorCode,
    async_info_adapter_Cancel,
    async_info_adapter_Close
};

static HRESULT async_adapter_op_create( IBluetoothAdapter *adapter, IAsyncOperation_BluetoothAdapter **out )
{
    struct async_adapter_op *impl;

    if (!(impl = calloc( 1, sizeof( *impl ) ))) return E_OUTOFMEMORY;
    impl->IAsyncOperation_BluetoothAdapter_iface.lpVtbl = &async_adapter_op_vtbl;
    impl->IAsyncInfo_iface.lpVtbl = &async_info_adapter_vtbl;
    impl->ref = 1;
    impl->result = adapter;
    impl->status = Started;
    if (adapter) IBluetoothAdapter_AddRef( adapter );

    *out = &impl->IAsyncOperation_BluetoothAdapter_iface;
    return S_OK;
}

static HRESULT WINAPI bluetoothadapter_statics_GetDeviceSelector( IBluetoothAdapterStatics *iface, HSTRING *result )
{
    static const WCHAR *default_res = L"System.Devices.InterfaceClassGuid:=\"{92383B0E-F90E-4AC9-8D44-8C2D0D0EBDA2}\" "
                                      L"AND System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True";

    TRACE( "iface %p, result %p.\n", iface, result );

    if (!result) return E_POINTER;
    return WindowsCreateString( default_res, wcslen(default_res), result );
}

static HRESULT WINAPI bluetoothadapter_statics_FromIdAsync( IBluetoothAdapterStatics *iface, HSTRING id, IAsyncOperation_BluetoothAdapter **operation )
{
    HRESULT hr;
    IBluetoothAdapter *adapter;
    UINT64 addr = 0;
    const WCHAR *str = WindowsGetStringRawBuffer( id, NULL );

    TRACE( "iface %p, id %s, operation %p.\n", iface, debugstr_hstring(id), operation );
    if (!operation) return E_POINTER;

    const WCHAR *addr_ptr = wcsstr( str, L"&" );
    if (addr_ptr) swscanf( addr_ptr + 1, L"%I64x", &addr );
    else if ((addr_ptr = wcsstr( str, L"_" ))) swscanf( addr_ptr + 1, L"%I64x", &addr );

    hr = bluetooth_adapter_create( id, addr, &adapter );
    if (FAILED(hr)) return hr;

    hr = async_adapter_op_create( adapter, operation );
    IBluetoothAdapter_Release( adapter );
    return hr;
}

static HRESULT WINAPI bluetoothadapter_statics_GetDefaultAsync( IBluetoothAdapterStatics *iface, IAsyncOperation_BluetoothAdapter **operation )
{
    HRESULT hr;
    IBluetoothAdapter *adapter;
    WCHAR buf[64];
    HSTRING id;

    TRACE( "iface %p, operation %p.\n", iface, operation );
    if (!operation) return E_POINTER;

    swprintf( buf, ARRAY_SIZE(buf), L"BluetoothAdapter#BluetoothAdapter0" );
    WindowsCreateString( buf, wcslen(buf), &id );

    hr = bluetooth_adapter_create( id, 0, &adapter );
    WindowsDeleteString( id );
    if (FAILED(hr)) return hr;

    hr = async_adapter_op_create( adapter, operation );
    IBluetoothAdapter_Release( adapter );
    return hr;
}

static const struct IBluetoothAdapterStaticsVtbl bluetoothadapter_statics_vtbl =
{
    bluetoothadapter_statics_QueryInterface,
    bluetoothadapter_statics_AddRef,
    bluetoothadapter_statics_Release,
    /* IInspectable methods */
    bluetoothadapter_statics_GetIids,
    bluetoothadapter_statics_GetRuntimeClassName,
    bluetoothadapter_statics_GetTrustLevel,
    /* IBluetoothAdapterStatics methods */
    bluetoothadapter_statics_GetDeviceSelector,
    bluetoothadapter_statics_FromIdAsync,
    bluetoothadapter_statics_GetDefaultAsync,
};

static struct bluetoothadapter bluetoothadapter_statics =
{
    {&factory_vtbl},
    {&bluetoothadapter_statics_vtbl},
    1,
};

IActivationFactory *bluetoothadapter_factory = &bluetoothadapter_statics.IActivationFactory_iface;
