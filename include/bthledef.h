/*
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
#ifndef __BTHLEDEFS_H
#define __BTHLEDEFS_H

typedef struct _BTH_LE_UUID
{
    BOOLEAN IsShortUuid;
    union
    {
        USHORT ShortUuid;
        GUID LongUuid;
    } Value;
} BTH_LE_UUID, *PBTH_LE_UUID;

typedef struct _BTH_LE_GATT_SERVICE
{
    BTH_LE_UUID ServiceUuid;
    USHORT AttributeHandle;
} BTH_LE_GATT_SERVICE, *PBTH_LE_GATT_SERVICE;

typedef struct _BTH_LE_GATT_CHARACTERISTIC
{
    USHORT ServiceHandle;
    BTH_LE_UUID CharacteristicUuid;
    USHORT AttributeHandle;
    USHORT CharacteristicValueHandle;
    BOOLEAN IsBroadcastable;
    BOOLEAN IsReadable;
    BOOLEAN IsWritable;
    BOOLEAN IsWritableWithoutResponse;
    BOOLEAN IsSignedWritable;
    BOOLEAN IsNotifiable;
    BOOLEAN IsIndicatable;
    BOOLEAN HasExtendedProperties;
} BTH_LE_GATT_CHARACTERISTIC, *PBTH_LE_GATT_CHARACTERISTIC;

typedef struct _BTH_LE_GATT_CHARACTERISTIC_VALUE
{
    ULONG DataSize;
    UCHAR Data[1];
} BTH_LE_GATT_CHARACTERISTIC_VALUE, *PBTH_LE_GATT_CHARACTERISTIC_VALUE;

typedef ULONG64 BTH_LE_GATT_RELIABLE_WRITE_CONTEXT, *PBTH_LE_GATT_RELIABLE_WRITE_CONTEXT;

#define BLUETOOTH_GATT_FLAG_NONE                       0x00000000
#define BLUETOOTH_GATT_FLAG_CONNECTION_ENCRYPTED       0x00000001
#define BLUETOOTH_GATT_FLAG_CONNECTION_AUTHENTICATED   0x00000002
#define BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE     0x00000004
#define BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_CACHE      0x00000008
#define BLUETOOTH_GATT_FLAG_SIGNED_WRITE               0x00000010
#define BLUETOOTH_GATT_FLAG_WRITE_WITHOUT_RESPONSE     0x00000020
#define BLUETOOTH_GATT_FLAG_RETURN_ALL                 0x00000040

DEFINE_GUID( GUID_BLUETOOTHLE_DEVICE_INTERFACE, 0x781aee18, 0x7733, 0x4ce4, 0xad, 0xd0, 0x91, 0xf4, 0x1c, 0x67, 0xb5, 0x92 );
DEFINE_GUID( BTH_LE_ATT_BLUETOOTH_BASE_GUID, 0, 0, 0x1000, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb );
#endif
