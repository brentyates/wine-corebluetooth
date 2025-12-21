/* WinRT Windows.Storage.Streams DataReader Implementation
 *
 * Copyright (C) 2025 Mohamad Al-Jaf
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
#include <initguid.h>
#include <robuffer.h>

WINE_DEFAULT_DEBUG_CHANNEL(storage);

struct data_reader
{
    IDataReader IDataReader_iface;
    IClosable IClosable_iface;
    LONG ref;

    IBuffer *buffer;
    BYTE *data;
    UINT32 length;
    UINT32 position;

    UnicodeEncoding encoding;
    ByteOrder byte_order;
    InputStreamOptions options;
};

static inline struct data_reader *impl_from_IDataReader( IDataReader *iface )
{
    return CONTAINING_RECORD( iface, struct data_reader, IDataReader_iface );
}

static HRESULT WINAPI data_reader_QueryInterface( IDataReader *iface, REFIID iid, void **out )
{
    struct data_reader *impl = impl_from_IDataReader( iface );

    TRACE( "iface %p, iid %s, out %p.\n", iface, debugstr_guid( iid ), out );

    if (IsEqualGUID( iid, &IID_IUnknown ) ||
        IsEqualGUID( iid, &IID_IInspectable ) ||
        IsEqualGUID( iid, &IID_IDataReader ))
    {
        *out = &impl->IDataReader_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IClosable ))
    {
        *out = &impl->IClosable_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI data_reader_AddRef( IDataReader *iface )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI data_reader_Release( IDataReader *iface )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );

    if (!ref)
    {
        if (impl->buffer) IBuffer_Release(impl->buffer);
        free( impl );
    }
    return ref;
}

static HRESULT WINAPI data_reader_GetIids( IDataReader *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_GetRuntimeClassName( IDataReader *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_GetTrustLevel( IDataReader *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_get_UnconsumedBufferLength( IDataReader *iface, UINT32 *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    if (!value) return E_POINTER;
    if (impl->position > impl->length) *value = 0;
    else *value = impl->length - impl->position;
    TRACE( "iface %p, position=%u length=%u result=%u.\n", iface, impl->position, impl->length, *value );
    return S_OK;
}

static HRESULT WINAPI data_reader_get_UnicodeEncoding( IDataReader *iface, UnicodeEncoding *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->encoding;
    return S_OK;
}

static HRESULT WINAPI data_reader_put_UnicodeEncoding( IDataReader *iface, UnicodeEncoding value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    impl->encoding = value;
    return S_OK;
}

static HRESULT WINAPI data_reader_get_ByteOrder( IDataReader *iface, ByteOrder *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->byte_order;
    return S_OK;
}

static HRESULT WINAPI data_reader_put_ByteOrder( IDataReader *iface, ByteOrder value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    impl->byte_order = value;
    return S_OK;
}

static HRESULT WINAPI data_reader_get_InputStreamOptions( IDataReader *iface, InputStreamOptions *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    *value = impl->options;
    return S_OK;
}

static HRESULT WINAPI data_reader_put_InputStreamOptions( IDataReader *iface, InputStreamOptions value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    TRACE( "iface %p, value %d.\n", iface, value );
    impl->options = value;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadByte( IDataReader *iface, BYTE *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (impl->position >= impl->length) return E_BOUNDS;
    *value = impl->data[impl->position++];
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadBytes( IDataReader *iface, UINT32 length, BYTE *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    TRACE( "iface %p, length=%u position=%u total_length=%u.\n", iface, length, impl->position, impl->length );
    if (impl->position + length > impl->length)
    {
        WARN( "E_BOUNDS: position(%u) + length(%u) > total(%u).\n", impl->position, length, impl->length );
        return E_BOUNDS;
    }
    memcpy(value, impl->data + impl->position, length);
    impl->position += length;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadBuffer( IDataReader *iface, UINT32 length, IBuffer **buffer )
{
    FIXME( "iface %p, length %u, buffer %p stub!\n", iface, length, buffer );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_ReadBoolean( IDataReader *iface, boolean *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->position >= impl->length) return E_BOUNDS;
    *value = impl->data[impl->position++] != 0;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadGuid( IDataReader *iface, GUID *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_ReadInt16( IDataReader *iface, INT16 *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    UINT16 v;
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->position + 2 > impl->length) return E_BOUNDS;

    memcpy(&v, impl->data + impl->position, 2);
    impl->position += 2;

    if (impl->byte_order == ByteOrder_BigEndian)
        v = (v >> 8) | (v << 8);

    *value = (INT16)v;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadInt32( IDataReader *iface, INT32 *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    UINT32 v;
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->position + 4 > impl->length) return E_BOUNDS;
    
    memcpy(&v, impl->data + impl->position, 4);
    impl->position += 4;
    
    if (impl->byte_order == ByteOrder_BigEndian)
        v = (v >> 24) | ((v >> 8) & 0x0000FF00) | ((v << 8) & 0x00FF0000) | (v << 24);
        
    *value = (INT32)v;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadInt64( IDataReader *iface, INT64 *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    UINT64 v;
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->position + 8 > impl->length) return E_BOUNDS;

    memcpy(&v, impl->data + impl->position, 8);
    impl->position += 8;

    if (impl->byte_order == ByteOrder_BigEndian)
    {
        v = ((v >> 56) & 0x00000000000000FFULL) |
            ((v >> 40) & 0x000000000000FF00ULL) |
            ((v >> 24) & 0x0000000000FF0000ULL) |
            ((v >>  8) & 0x00000000FF000000ULL) |
            ((v <<  8) & 0x000000FF00000000ULL) |
            ((v << 24) & 0x0000FF0000000000ULL) |
            ((v << 40) & 0x00FF000000000000ULL) |
            ((v << 56) & 0xFF00000000000000ULL);
    }

    *value = (INT64)v;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadUInt16( IDataReader *iface, UINT16 *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    UINT16 v;
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->position + 2 > impl->length) return E_BOUNDS;

    memcpy(&v, impl->data + impl->position, 2);
    impl->position += 2;

    if (impl->byte_order == ByteOrder_BigEndian)
        v = (v >> 8) | (v << 8);

    *value = v;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadUInt32( IDataReader *iface, UINT32 *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    UINT32 v;
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->position + 4 > impl->length) return E_BOUNDS;

    memcpy(&v, impl->data + impl->position, 4);
    impl->position += 4;

    if (impl->byte_order == ByteOrder_BigEndian)
        v = (v >> 24) | ((v >> 8) & 0x0000FF00) | ((v << 8) & 0x00FF0000) | (v << 24);

    *value = v;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadUInt64( IDataReader *iface, UINT64 *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    UINT64 v;
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->position + 8 > impl->length) return E_BOUNDS;

    memcpy(&v, impl->data + impl->position, 8);
    impl->position += 8;

    if (impl->byte_order == ByteOrder_BigEndian)
    {
        v = ((v >> 56) & 0x00000000000000FFULL) |
            ((v >> 40) & 0x000000000000FF00ULL) |
            ((v >> 24) & 0x0000000000FF0000ULL) |
            ((v >>  8) & 0x00000000FF000000ULL) |
            ((v <<  8) & 0x000000FF00000000ULL) |
            ((v << 24) & 0x0000FF0000000000ULL) |
            ((v << 40) & 0x00FF000000000000ULL) |
            ((v << 56) & 0xFF00000000000000ULL);
    }

    *value = v;
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadSingle( IDataReader *iface, FLOAT *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    UINT32 v;
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->position + 4 > impl->length) return E_BOUNDS;

    memcpy(&v, impl->data + impl->position, 4);
    impl->position += 4;

    if (impl->byte_order == ByteOrder_BigEndian)
        v = (v >> 24) | ((v >> 8) & 0x0000FF00) | ((v << 8) & 0x00FF0000) | (v << 24);

    memcpy(value, &v, 4);
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadDouble( IDataReader *iface, DOUBLE *value )
{
    struct data_reader *impl = impl_from_IDataReader( iface );
    UINT64 v;
    TRACE( "iface %p, value %p.\n", iface, value );
    if (!value) return E_POINTER;
    if (impl->position + 8 > impl->length) return E_BOUNDS;

    memcpy(&v, impl->data + impl->position, 8);
    impl->position += 8;

    if (impl->byte_order == ByteOrder_BigEndian)
    {
        v = ((v >> 56) & 0x00000000000000FFULL) |
            ((v >> 40) & 0x000000000000FF00ULL) |
            ((v >> 24) & 0x0000000000FF0000ULL) |
            ((v >>  8) & 0x00000000FF000000ULL) |
            ((v <<  8) & 0x000000FF00000000ULL) |
            ((v << 24) & 0x0000FF0000000000ULL) |
            ((v << 40) & 0x00FF000000000000ULL) |
            ((v << 56) & 0xFF00000000000000ULL);
    }

    memcpy(value, &v, 8);
    return S_OK;
}

static HRESULT WINAPI data_reader_ReadString( IDataReader *iface, UINT32 code_unit_count, HSTRING *value )
{
    FIXME( "iface %p, code_unit_count %u, value %p stub!\n", iface, code_unit_count, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_ReadDateTime( IDataReader *iface, DateTime *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_ReadTimeSpan( IDataReader *iface, TimeSpan *value )
{
    FIXME( "iface %p, value %p stub!\n", iface, value );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_LoadAsync( IDataReader *iface, UINT32 count, struct IInspectable **operation )
{
    FIXME( "iface %p, count %u, operation %p stub!\n", iface, count, operation );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_DetachBuffer( IDataReader *iface, IBuffer **buffer )
{
    FIXME( "iface %p, buffer %p stub!\n", iface, buffer );
    return E_NOTIMPL;
}

static HRESULT WINAPI data_reader_DetachStream( IDataReader *iface, IInputStream **stream )
{
    FIXME( "iface %p, stream %p stub!\n", iface, stream );
    return E_NOTIMPL;
}

static const struct IDataReaderVtbl data_reader_vtbl =
{
    data_reader_QueryInterface,
    data_reader_AddRef,
    data_reader_Release,
    data_reader_GetIids,
    data_reader_GetRuntimeClassName,
    data_reader_GetTrustLevel,
    data_reader_get_UnconsumedBufferLength,
    data_reader_get_UnicodeEncoding,
    data_reader_put_UnicodeEncoding,
    data_reader_get_ByteOrder,
    data_reader_put_ByteOrder,
    data_reader_get_InputStreamOptions,
    data_reader_put_InputStreamOptions,
    data_reader_ReadByte,
    data_reader_ReadBytes,
    data_reader_ReadBuffer,
    data_reader_ReadBoolean,
    data_reader_ReadGuid,
    data_reader_ReadInt16,
    data_reader_ReadInt32,
    data_reader_ReadInt64,
    data_reader_ReadUInt16,
    data_reader_ReadUInt32,
    data_reader_ReadUInt64,
    data_reader_ReadSingle,
    data_reader_ReadDouble,
    data_reader_ReadString,
    data_reader_ReadDateTime,
    data_reader_ReadTimeSpan,
    data_reader_LoadAsync,
    data_reader_DetachBuffer,
    data_reader_DetachStream
};

// IClosable implementation
static inline struct data_reader *impl_from_IClosable( IClosable *iface )
{
    return CONTAINING_RECORD( iface, struct data_reader, IClosable_iface );
}

static HRESULT WINAPI closable_QueryInterface( IClosable *iface, REFIID iid, void **out )
{
    struct data_reader *impl = impl_from_IClosable( iface );
    return IDataReader_QueryInterface( &impl->IDataReader_iface, iid, out );
}

static ULONG WINAPI closable_AddRef( IClosable *iface )
{
    struct data_reader *impl = impl_from_IClosable( iface );
    return IDataReader_AddRef( &impl->IDataReader_iface );
}

static ULONG WINAPI closable_Release( IClosable *iface )
{
    struct data_reader *impl = impl_from_IClosable( iface );
    return IDataReader_Release( &impl->IDataReader_iface );
}

static HRESULT WINAPI closable_GetIids( IClosable *iface, ULONG *iid_count, IID **iids )
{
    return IDataReader_GetIids( &impl_from_IClosable(iface)->IDataReader_iface, iid_count, iids );
}

static HRESULT WINAPI closable_GetRuntimeClassName( IClosable *iface, HSTRING *class_name )
{
    return IDataReader_GetRuntimeClassName( &impl_from_IClosable(iface)->IDataReader_iface, class_name );
}

static HRESULT WINAPI closable_GetTrustLevel( IClosable *iface, TrustLevel *trust_level )
{
    return IDataReader_GetTrustLevel( &impl_from_IClosable(iface)->IDataReader_iface, trust_level );
}

static HRESULT WINAPI closable_Close( IClosable *iface )
{
    struct data_reader *impl = impl_from_IClosable( iface );
    TRACE( "iface %p.\n", iface );
    if (impl->buffer)
    {
        IBuffer_Release(impl->buffer);
        impl->buffer = NULL;
        impl->data = NULL;
        impl->length = 0;
    }
    return S_OK;
}

static const struct IClosableVtbl closable_vtbl =
{
    closable_QueryInterface,
    closable_AddRef,
    closable_Release,
    closable_GetIids,
    closable_GetRuntimeClassName,
    closable_GetTrustLevel,
    closable_Close
};


struct data_reader_statics
{
    IActivationFactory IActivationFactory_iface;
    IDataReaderFactory IDataReaderFactory_iface;
    IDataReaderStatics IDataReaderStatics_iface;
    LONG ref;
};

static inline struct data_reader_statics *impl_from_IActivationFactory( IActivationFactory *iface )
{
    return CONTAINING_RECORD( iface, struct data_reader_statics, IActivationFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface( IActivationFactory *iface, REFIID iid, void **out )
{
    struct data_reader_statics *impl = impl_from_IActivationFactory( iface );

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

    if (IsEqualGUID( iid, &IID_IDataReaderFactory ))
    {
        *out = &impl->IDataReaderFactory_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    if (IsEqualGUID( iid, &IID_IDataReaderStatics ))
    {
        *out = &impl->IDataReaderStatics_iface;
        IInspectable_AddRef( *out );
        return S_OK;
    }

    FIXME( "%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid( iid ) );
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef( IActivationFactory *iface )
{
    struct data_reader_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedIncrement( &impl->ref );
    TRACE( "iface %p increasing refcount to %lu.\n", iface, ref );
    return ref;
}

static ULONG WINAPI factory_Release( IActivationFactory *iface )
{
    struct data_reader_statics *impl = impl_from_IActivationFactory( iface );
    ULONG ref = InterlockedDecrement( &impl->ref );
    TRACE( "iface %p decreasing refcount to %lu.\n", iface, ref );
    return ref;
}

static HRESULT WINAPI factory_GetIids( IActivationFactory *iface, ULONG *iid_count, IID **iids )
{
    FIXME( "iface %p, iid_count %p, iids %p stub!\n", iface, iid_count, iids );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetRuntimeClassName( IActivationFactory *iface, HSTRING *class_name )
{
    FIXME( "iface %p, class_name %p stub!\n", iface, class_name );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_GetTrustLevel( IActivationFactory *iface, TrustLevel *trust_level )
{
    FIXME( "iface %p, trust_level %p stub!\n", iface, trust_level );
    return E_NOTIMPL;
}

static HRESULT WINAPI factory_ActivateInstance( IActivationFactory *iface, IInspectable **instance )
{
    struct data_reader *reader;
    
    TRACE( "iface %p, instance %p.\n", iface, instance );
    
    reader = calloc(1, sizeof(*reader));
    if (!reader) return E_OUTOFMEMORY;
    
    reader->IDataReader_iface.lpVtbl = &data_reader_vtbl;
    reader->IClosable_iface.lpVtbl = &closable_vtbl;
    reader->ref = 1;
    reader->encoding = UnicodeEncoding_Utf8;
    reader->byte_order = ByteOrder_LittleEndian;
    
    *instance = (IInspectable *)&reader->IDataReader_iface;
    return S_OK;
}

static const struct IActivationFactoryVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_GetIids,
    factory_GetRuntimeClassName,
    factory_GetTrustLevel,
    factory_ActivateInstance,
};

// IDataReaderFactory implementation
static inline struct data_reader_statics *impl_from_IDataReaderFactory( IDataReaderFactory *iface )
{
    return CONTAINING_RECORD( iface, struct data_reader_statics, IDataReaderFactory_iface );
}

static HRESULT WINAPI factory_QueryInterface_IDataReaderFactory( IDataReaderFactory *iface, REFIID iid, void **out )
{
    struct data_reader_statics *impl = impl_from_IDataReaderFactory( iface );
    return factory_QueryInterface( &impl->IActivationFactory_iface, iid, out );
}

static ULONG WINAPI factory_AddRef_IDataReaderFactory( IDataReaderFactory *iface )
{
    struct data_reader_statics *impl = impl_from_IDataReaderFactory( iface );
    return factory_AddRef( &impl->IActivationFactory_iface );
}

static ULONG WINAPI factory_Release_IDataReaderFactory( IDataReaderFactory *iface )
{
    struct data_reader_statics *impl = impl_from_IDataReaderFactory( iface );
    return factory_Release( &impl->IActivationFactory_iface );
}

static HRESULT WINAPI factory_GetIids_IDataReaderFactory( IDataReaderFactory *iface, ULONG *iid_count, IID **iids )
{
    return factory_GetIids( &impl_from_IDataReaderFactory(iface)->IActivationFactory_iface, iid_count, iids );
}

static HRESULT WINAPI factory_GetRuntimeClassName_IDataReaderFactory( IDataReaderFactory *iface, HSTRING *class_name )
{
    return factory_GetRuntimeClassName( &impl_from_IDataReaderFactory(iface)->IActivationFactory_iface, class_name );
}

static HRESULT WINAPI factory_GetTrustLevel_IDataReaderFactory( IDataReaderFactory *iface, TrustLevel *trust_level )
{
    return factory_GetTrustLevel( &impl_from_IDataReaderFactory(iface)->IActivationFactory_iface, trust_level );
}

static HRESULT WINAPI factory_CreateDataReader( IDataReaderFactory *iface, IInputStream *input_stream, IDataReader **data_reader )
{
    FIXME( "iface %p, input_stream %p, data_reader %p stub!\n", iface, input_stream, data_reader );
    return E_NOTIMPL;
}

static const struct IDataReaderFactoryVtbl factory_IDataReaderFactory_vtbl =
{
    factory_QueryInterface_IDataReaderFactory,
    factory_AddRef_IDataReaderFactory,
    factory_Release_IDataReaderFactory,
    factory_GetIids_IDataReaderFactory,
    factory_GetRuntimeClassName_IDataReaderFactory,
    factory_GetTrustLevel_IDataReaderFactory,
    factory_CreateDataReader
};

// IDataReaderStatics implementation
static inline struct data_reader_statics *impl_from_IDataReaderStatics( IDataReaderStatics *iface )
{
    return CONTAINING_RECORD( iface, struct data_reader_statics, IDataReaderStatics_iface );
}

static HRESULT WINAPI factory_QueryInterface_IDataReaderStatics( IDataReaderStatics *iface, REFIID iid, void **out )
{
    struct data_reader_statics *impl = impl_from_IDataReaderStatics( iface );
    return factory_QueryInterface( &impl->IActivationFactory_iface, iid, out );
}

static ULONG WINAPI factory_AddRef_IDataReaderStatics( IDataReaderStatics *iface )
{
    struct data_reader_statics *impl = impl_from_IDataReaderStatics( iface );
    return factory_AddRef( &impl->IActivationFactory_iface );
}

static ULONG WINAPI factory_Release_IDataReaderStatics( IDataReaderStatics *iface )
{
    struct data_reader_statics *impl = impl_from_IDataReaderStatics( iface );
    return factory_Release( &impl->IActivationFactory_iface );
}

static HRESULT WINAPI factory_GetIids_IDataReaderStatics( IDataReaderStatics *iface, ULONG *iid_count, IID **iids )
{
    return factory_GetIids( &impl_from_IDataReaderStatics(iface)->IActivationFactory_iface, iid_count, iids );
}

static HRESULT WINAPI factory_GetRuntimeClassName_IDataReaderStatics( IDataReaderStatics *iface, HSTRING *class_name )
{
    return factory_GetRuntimeClassName( &impl_from_IDataReaderStatics(iface)->IActivationFactory_iface, class_name );
}

static HRESULT WINAPI factory_GetTrustLevel_IDataReaderStatics( IDataReaderStatics *iface, TrustLevel *trust_level )
{
    return factory_GetTrustLevel( &impl_from_IDataReaderStatics(iface)->IActivationFactory_iface, trust_level );
}

static HRESULT WINAPI factory_FromBuffer( IDataReaderStatics *iface, IBuffer *buffer, IDataReader **data_reader )
{
    struct data_reader *reader;
    IBufferByteAccess *byte_access;
    HRESULT hr;

    TRACE( "iface %p, buffer %p, data_reader %p.\n", iface, buffer, data_reader );

    if (!buffer) return E_INVALIDARG;

    reader = calloc(1, sizeof(*reader));
    if (!reader) return E_OUTOFMEMORY;

    reader->IDataReader_iface.lpVtbl = &data_reader_vtbl;
    reader->IClosable_iface.lpVtbl = &closable_vtbl;
    reader->ref = 1;
    reader->encoding = UnicodeEncoding_Utf8;
    reader->byte_order = ByteOrder_LittleEndian;

    hr = IBuffer_QueryInterface(buffer, &IID_IBufferByteAccess, (void**)&byte_access);
    if (FAILED(hr))
    {
        FIXME("Buffer does not support IBufferByteAccess!\n");
        free(reader);
        return hr;
    }

    hr = IBufferByteAccess_Buffer(byte_access, &reader->data);
    IBufferByteAccess_Release(byte_access);
    if (FAILED(hr))
    {
        free(reader);
        return hr;
    }

    IBuffer_AddRef(buffer);
    reader->buffer = buffer;
    IBuffer_get_Length(buffer, &reader->length);

    TRACE( "iface %p, buffer %p, length=%u data=%p.\n", iface, buffer, reader->length, reader->data );

    *data_reader = &reader->IDataReader_iface;
    return S_OK;
}

static const struct IDataReaderStaticsVtbl factory_IDataReaderStatics_vtbl =
{
    factory_QueryInterface_IDataReaderStatics,
    factory_AddRef_IDataReaderStatics,
    factory_Release_IDataReaderStatics,
    factory_GetIids_IDataReaderStatics,
    factory_GetRuntimeClassName_IDataReaderStatics,
    factory_GetTrustLevel_IDataReaderStatics,
    factory_FromBuffer
};

static struct data_reader_statics data_reader_statics =
{
    {&factory_vtbl},
    {&factory_IDataReaderFactory_vtbl},
    {&factory_IDataReaderStatics_vtbl},
    0,
};

IActivationFactory *data_reader_factory = &data_reader_statics.IActivationFactory_iface;









