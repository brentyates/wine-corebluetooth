/*
 * Copyright 2022 Nikolay Sivov for CodeWeavers
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

#include "initguid.h"
#include "private.h"

#include <assert.h>

#include "wine/debug.h"
#include "objbase.h"

#include "bcrypt.h"
#include "wincrypt.h"

#define WIDL_using_Windows_Security_Cryptography
#include "windows.security.cryptography.h"
#include "robuffer.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypto);

struct crypto_buffer
{
    IBuffer IBuffer_iface;
    IBufferByteAccess IBufferByteAccess_iface;
    LONG refcount;
    UINT32 length;
    UINT32 capacity;
    BYTE *data;
};

static inline struct crypto_buffer *impl_from_IBuffer( IBuffer *iface )
{
    return CONTAINING_RECORD( iface, struct crypto_buffer, IBuffer_iface );
}

static inline struct crypto_buffer *impl_from_IBufferByteAccess( IBufferByteAccess *iface )
{
    return CONTAINING_RECORD( iface, struct crypto_buffer, IBufferByteAccess_iface );
}

static HRESULT WINAPI crypto_buffer_QueryInterface( IBuffer *iface, REFIID iid, void **out )
{
    struct crypto_buffer *impl = impl_from_IBuffer( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IBuffer ))
    {
        IBuffer_AddRef( &impl->IBuffer_iface );
        *out = &impl->IBuffer_iface;
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IBufferByteAccess ))
    {
        IBufferByteAccess_AddRef( &impl->IBufferByteAccess_iface );
        *out = &impl->IBufferByteAccess_iface;
        return S_OK;
    }

    WARN( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI crypto_buffer_AddRef( IBuffer *iface )
{
    struct crypto_buffer *impl = impl_from_IBuffer( iface );
    ULONG refcount = InterlockedIncrement( &impl->refcount );
    TRACE( "iface %p, refcount %lu.\n", iface, refcount );
    return refcount;
}

static ULONG WINAPI crypto_buffer_Release( IBuffer *iface )
{
    struct crypto_buffer *impl = impl_from_IBuffer( iface );
    ULONG refcount = InterlockedDecrement( &impl->refcount );
    TRACE( "iface %p, refcount %lu.\n", iface, refcount );
    if (!refcount)
    {
        free( impl->data );
        free( impl );
    }
    return refcount;
}

static HRESULT WINAPI crypto_buffer_GetIids( IBuffer *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI crypto_buffer_GetRuntimeClassName( IBuffer *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI crypto_buffer_GetTrustLevel( IBuffer *iface, TrustLevel *level )
{
    FIXME( "iface %p, level %p stub!\n", iface, level );
    return E_NOTIMPL;
}

static HRESULT WINAPI crypto_buffer_get_Capacity( IBuffer *iface, UINT32 *value )
{
    struct crypto_buffer *impl = impl_from_IBuffer( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->capacity;
    return S_OK;
}

static HRESULT WINAPI crypto_buffer_get_Length( IBuffer *iface, UINT32 *value )
{
    struct crypto_buffer *impl = impl_from_IBuffer( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->length;
    return S_OK;
}

static HRESULT WINAPI crypto_buffer_put_Length( IBuffer *iface, UINT32 value )
{
    struct crypto_buffer *impl = impl_from_IBuffer( iface );
    TRACE( "iface %p, value %u.\n", iface, value );
    if (value > impl->capacity) return E_INVALIDARG;
    impl->length = value;
    return S_OK;
}

static const IBufferVtbl crypto_buffer_vtbl =
{
    crypto_buffer_QueryInterface,
    crypto_buffer_AddRef,
    crypto_buffer_Release,
    crypto_buffer_GetIids,
    crypto_buffer_GetRuntimeClassName,
    crypto_buffer_GetTrustLevel,
    crypto_buffer_get_Capacity,
    crypto_buffer_get_Length,
    crypto_buffer_put_Length,
};

static HRESULT WINAPI crypto_buffer_byte_access_QueryInterface( IBufferByteAccess *iface, REFIID iid, void **out )
{
    struct crypto_buffer *impl = impl_from_IBufferByteAccess( iface );
    return crypto_buffer_QueryInterface( &impl->IBuffer_iface, iid, out );
}

static ULONG WINAPI crypto_buffer_byte_access_AddRef( IBufferByteAccess *iface )
{
    struct crypto_buffer *impl = impl_from_IBufferByteAccess( iface );
    return crypto_buffer_AddRef( &impl->IBuffer_iface );
}

static ULONG WINAPI crypto_buffer_byte_access_Release( IBufferByteAccess *iface )
{
    struct crypto_buffer *impl = impl_from_IBufferByteAccess( iface );
    return crypto_buffer_Release( &impl->IBuffer_iface );
}

static HRESULT WINAPI crypto_buffer_byte_access_Buffer( IBufferByteAccess *iface, BYTE **value )
{
    struct crypto_buffer *impl = impl_from_IBufferByteAccess( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->data;
    return S_OK;
}

static const IBufferByteAccessVtbl crypto_buffer_byte_access_vtbl =
{
    crypto_buffer_byte_access_QueryInterface,
    crypto_buffer_byte_access_AddRef,
    crypto_buffer_byte_access_Release,
    crypto_buffer_byte_access_Buffer,
};

static HRESULT create_crypto_buffer( UINT32 size, const BYTE *data, IBuffer **out )
{
    struct crypto_buffer *impl;

    if (!(impl = calloc( 1, sizeof(*impl) ))) return E_OUTOFMEMORY;
    if (size > 0)
    {
        if (!(impl->data = malloc( size )))
        {
            free( impl );
            return E_OUTOFMEMORY;
        }
        if (data) memcpy( impl->data, data, size );
    }

    impl->IBuffer_iface.lpVtbl = &crypto_buffer_vtbl;
    impl->IBufferByteAccess_iface.lpVtbl = &crypto_buffer_byte_access_vtbl;
    impl->refcount = 1;
    impl->length = size;
    impl->capacity = size;

    *out = &impl->IBuffer_iface;
    return S_OK;
}

struct cryptobuffer_factory
{
    IActivationFactory IActivationFactory_iface;
    ICryptographicBufferStatics ICryptographicBufferStatics_iface;
    LONG refcount;
};

static inline struct cryptobuffer_factory *impl_from_IActivationFactory(IActivationFactory *iface)
{
    return CONTAINING_RECORD(iface, struct cryptobuffer_factory, IActivationFactory_iface);
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_factory_QueryInterface(
        IActivationFactory *iface, REFIID iid, void **out)
{
    struct cryptobuffer_factory *factory = impl_from_IActivationFactory(iface);

    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IUnknown) ||
        IsEqualGUID(iid, &IID_IInspectable) ||
        IsEqualGUID(iid, &IID_IAgileObject) ||
        IsEqualGUID(iid, &IID_IActivationFactory))
    {
        IUnknown_AddRef(iface);
        *out = &factory->IActivationFactory_iface;
        return S_OK;
    }

    if (IsEqualGUID(iid, &IID_ICryptographicBufferStatics))
    {
        IUnknown_AddRef(iface);
        *out = &factory->ICryptographicBufferStatics_iface;
        return S_OK;
    }

    FIXME("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE cryptobuffer_factory_AddRef(IActivationFactory *iface)
{
    struct cryptobuffer_factory *factory = impl_from_IActivationFactory(iface);
    ULONG refcount = InterlockedIncrement(&factory->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    return refcount;
}

static ULONG STDMETHODCALLTYPE cryptobuffer_factory_Release(IActivationFactory *iface)
{
    struct cryptobuffer_factory *factory = impl_from_IActivationFactory(iface);
    ULONG refcount = InterlockedDecrement(&factory->refcount);

    TRACE("iface %p, refcount %lu.\n", iface, refcount);

    return refcount;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_factory_GetIids(
        IActivationFactory *iface, ULONG *iid_count, IID **iids)
{
    FIXME("iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_factory_GetRuntimeClassName(
        IActivationFactory *iface, HSTRING *class_name)
{
    FIXME("iface %p, class_name %p stub!\n", iface, class_name);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_factory_GetTrustLevel(
        IActivationFactory *iface, TrustLevel *trust_level)
{
    FIXME("iface %p, trust_level %p stub!\n", iface, trust_level);
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_factory_ActivateInstance(
        IActivationFactory *iface, IInspectable **instance)
{
    FIXME("iface %p, instance %p stub!\n", iface, instance);
    return E_NOTIMPL;
}

static const struct IActivationFactoryVtbl cryptobuffer_factory_vtbl =
{
    cryptobuffer_factory_QueryInterface,
    cryptobuffer_factory_AddRef,
    cryptobuffer_factory_Release,
    /* IInspectable methods */
    cryptobuffer_factory_GetIids,
    cryptobuffer_factory_GetRuntimeClassName,
    cryptobuffer_factory_GetTrustLevel,
    /* IActivationFactory methods */
    cryptobuffer_factory_ActivateInstance,
};

DEFINE_IINSPECTABLE(cryptobuffer_statics, ICryptographicBufferStatics, struct cryptobuffer_factory, IActivationFactory_iface);

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_Compare(
        ICryptographicBufferStatics *iface, IBuffer *object1, IBuffer *object2, boolean *is_equal)
{
    FIXME("iface %p, object1 %p, object2 %p, is_equal %p stub!\n", iface, object1, object2, is_equal);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_GenerateRandom(
        ICryptographicBufferStatics *iface, UINT32 length, IBuffer **buffer)
{
    FIXME("iface %p, length %u, buffer %p stub!\n", iface, length, buffer);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_GenerateRandomNumber(
        ICryptographicBufferStatics *iface, UINT32 *value)
{
    TRACE("iface %p, value %p.\n", iface, value);

    BCryptGenRandom(NULL, (UCHAR *)value, sizeof(*value), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_CreateFromByteArray(
        ICryptographicBufferStatics *iface, UINT32 value_size, BYTE *value, IBuffer **buffer)
{
    TRACE("iface %p, value_size %u, value %p, buffer %p.\n", iface, value_size, value, buffer);

    if (!buffer) return E_INVALIDARG;
    return create_crypto_buffer( value_size, value, buffer );
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_CopyToByteArray(
        ICryptographicBufferStatics *iface, IBuffer *buffer, UINT32 *value_size, BYTE **value)
{
    IBufferByteAccess *byte_access;
    BYTE *src_data;
    UINT32 length;
    HRESULT hr;

    TRACE("iface %p, buffer %p, value_size %p, value %p.\n", iface, buffer, value_size, value);

    if (!value_size || !value) return E_INVALIDARG;

    *value_size = 0;
    *value = NULL;

    if (!buffer) return S_OK;

    if (FAILED(hr = IBuffer_get_Length( buffer, &length ))) return hr;
    if (!length) return S_OK;

    if (FAILED(hr = IBuffer_QueryInterface( buffer, &IID_IBufferByteAccess, (void **)&byte_access )))
        return hr;

    hr = IBufferByteAccess_Buffer( byte_access, &src_data );
    IBufferByteAccess_Release( byte_access );
    if (FAILED(hr)) return hr;

    if (!(*value = CoTaskMemAlloc( length ))) return E_OUTOFMEMORY;

    memcpy( *value, src_data, length );
    *value_size = length;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_DecodeFromHexString(
        ICryptographicBufferStatics *iface, HSTRING value, IBuffer **buffer)
{
    FIXME("iface %p, value %p, buffer %p stub!\n", iface, value, buffer);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_EncodeToHexString(
        ICryptographicBufferStatics *iface, IBuffer *buffer, HSTRING *value)
{
    FIXME("iface %p, buffer %p, value %p stub!\n", iface, buffer, value);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_DecodeFromBase64String(
        ICryptographicBufferStatics *iface, HSTRING value, IBuffer **buffer)
{
    FIXME("iface %p, value %p, buffer %p stub!\n", iface, value, buffer);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_EncodeToBase64String(
        ICryptographicBufferStatics *iface, IBuffer *buffer, HSTRING *value)
{
    IBufferByteAccess *buffer_access;
    HSTRING_BUFFER str_buffer;
    void *data = NULL;
    UINT32 length = 0;
    DWORD ret_length;
    WCHAR *str;
    HRESULT hr;

    TRACE("iface %p, buffer %p, value %p.\n", iface, buffer, value);

    if (buffer)
    {
        IBuffer_get_Length(buffer, &length);
        if (length)
        {
            if (SUCCEEDED(IBuffer_QueryInterface(buffer, &IID_IBufferByteAccess, (void **)&buffer_access)))
            {
                IBufferByteAccess_Buffer(buffer_access, (byte **)&data);
                IBufferByteAccess_Release(buffer_access);
            }
        }
    }

    if (!length)
        return WindowsCreateString(NULL, 0, value);

    if (!data)
        return E_FAIL;

    if (!CryptBinaryToStringW(data, length, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &ret_length))
        return E_FAIL;

    if (FAILED(hr = WindowsPreallocateStringBuffer(ret_length, &str, &str_buffer)))
        return hr;

    if (!CryptBinaryToStringW(data, length, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, str, &ret_length))
    {
        WindowsDeleteStringBuffer(str_buffer);
        return E_FAIL;
    }

    return WindowsPromoteStringBuffer(str_buffer, value);
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_ConvertStringToBinary(
        ICryptographicBufferStatics *iface, HSTRING value, BinaryStringEncoding encoding,
        IBuffer **buffer)
{
    FIXME("iface %p, value %p, encoding %d, buffer %p stub!\n", iface, value, encoding, buffer);

    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE cryptobuffer_statics_ConvertBinaryToString(
        ICryptographicBufferStatics *iface, BinaryStringEncoding encoding, IBuffer *buffer, HSTRING *value)
{
    FIXME("iface %p, encoding %d, buffer %p, value %p stub!\n", iface, encoding, buffer, value);

    return E_NOTIMPL;
}

static const struct ICryptographicBufferStaticsVtbl cryptobuffer_statics_vtbl =
{
    cryptobuffer_statics_QueryInterface,
    cryptobuffer_statics_AddRef,
    cryptobuffer_statics_Release,
    /* IInspectable methods */
    cryptobuffer_statics_GetIids,
    cryptobuffer_statics_GetRuntimeClassName,
    cryptobuffer_statics_GetTrustLevel,
    /* ICryptographicBufferStatics methods */
    cryptobuffer_statics_Compare,
    cryptobuffer_statics_GenerateRandom,
    cryptobuffer_statics_GenerateRandomNumber,
    cryptobuffer_statics_CreateFromByteArray,
    cryptobuffer_statics_CopyToByteArray,
    cryptobuffer_statics_DecodeFromHexString,
    cryptobuffer_statics_EncodeToHexString,
    cryptobuffer_statics_DecodeFromBase64String,
    cryptobuffer_statics_EncodeToBase64String,
    cryptobuffer_statics_ConvertStringToBinary,
    cryptobuffer_statics_ConvertBinaryToString,
};

static struct cryptobuffer_factory cryptobuffer_factory =
{
    .IActivationFactory_iface.lpVtbl = &cryptobuffer_factory_vtbl,
    .ICryptographicBufferStatics_iface.lpVtbl = &cryptobuffer_statics_vtbl,
    .refcount = 1,
};

IActivationFactory *cryptobuffer_activation_factory = &cryptobuffer_factory.IActivationFactory_iface;

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void **out)
{
    FIXME("clsid %s, riid %s, out %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), out);
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory(HSTRING classid, IActivationFactory **factory)
{
    const WCHAR *name = WindowsGetStringRawBuffer(classid, NULL);

    TRACE("classid %s, factory %p.\n", debugstr_hstring(classid), factory);

    *factory = NULL;

    if (!wcscmp(name, RuntimeClass_Windows_Security_Cryptography_CryptographicBuffer))
        IActivationFactory_QueryInterface(cryptobuffer_activation_factory, &IID_IActivationFactory, (void **)factory);
    if (!wcscmp(name, RuntimeClass_Windows_Security_Credentials_KeyCredentialManager))
        IActivationFactory_QueryInterface(credentials_activation_factory, &IID_IActivationFactory, (void **)factory);

    if (*factory) return S_OK;
    return CLASS_E_CLASSNOTAVAILABLE;
}
