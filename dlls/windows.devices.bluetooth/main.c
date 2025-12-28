/* WinRT Windows.Devices.Bluetooth Implementation
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

#include "initguid.h"
#include "private.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(bluetooth);

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    FIXME( "clsid %s, riid %s, out %p stub!\n", debugstr_guid(clsid), debugstr_guid(riid), out );
    return CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI DllGetActivationFactory( HSTRING classid, IActivationFactory **factory )
{
    const WCHAR *buffer = WindowsGetStringRawBuffer( classid, NULL );

    TRACE( "class %s\n", debugstr_w(buffer) );

    *factory = NULL;

    if (!wcscmp( buffer, RuntimeClass_Windows_Devices_Bluetooth_BluetoothAdapter ))
        IActivationFactory_QueryInterface( bluetoothadapter_factory, &IID_IActivationFactory, (void **)factory );
    if (!wcscmp( buffer, RuntimeClass_Windows_Devices_Bluetooth_BluetoothDevice ))
        IActivationFactory_QueryInterface( bluetoothdevice_statics_factory, &IID_IActivationFactory, (void **)factory );
    if (!wcscmp( buffer, RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice ))
        IActivationFactory_QueryInterface( bluetoothledevice_statics_factory, &IID_IActivationFactory, (void **)factory );
    if (!wcscmp( buffer, RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementWatcher ))
        IActivationFactory_QueryInterface( advertisement_watcher_factory, &IID_IActivationFactory, (void **)factory );
    if (!wcscmp( buffer, RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementFilter ))
        IActivationFactory_QueryInterface( advertisement_filter_factory, &IID_IActivationFactory, (void **)factory );
    if (!wcscmp( buffer, RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisement ))
        IActivationFactory_QueryInterface( advertisement_factory, &IID_IActivationFactory, (void **)factory );

    if (*factory) return S_OK;
    return CLASS_E_CLASSNOTAVAILABLE;
}

BOOL WINAPI DllMain( HINSTANCE instance, DWORD reason, void *reserved )
{
    TRACE( "(%p, %lu, %p)\n", instance, reason, reserved );

    switch (reason)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls( instance );
            break;
    }

    return TRUE;
}
