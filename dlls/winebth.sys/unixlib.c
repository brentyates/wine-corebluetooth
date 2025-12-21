/*
 * winebluetooth Unix interface
 *
 * Copyright 2024 Vibhav Pant
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

#if 0
#pragma makedep unix
#endif

#include <config.h>

#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <pthread.h>

#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <winternl.h>
#include <winbase.h>
#include <windef.h>
#include <wine/list.h>
#include <wine/rbtree.h>
#define WINE_NO_DEBUG_MSGS
#include <wine/debug.h>

#include "unixlib.h"
#include "unixlib_priv.h"

WINE_DEFAULT_DEBUG_CHANNEL( winebth );

/* Provide minimal stubs to satisfy debug symbols on host build */
int __wine_dbg_output( const char *str ) { (void)str; return 0; }
int __wine_dbg_header( enum __wine_debug_class cls, struct __wine_debug_channel *channel, const char *function )
{
    (void)cls; (void)channel; (void)function;
    return 0;
}

static int compare_string( const void *key, const struct wine_rb_entry *entry )
{
    struct unix_name *str = WINE_RB_ENTRY_VALUE( entry, struct unix_name, entry );
    return strcmp( key, str->str );
}

static struct rb_tree names = { .compare = compare_string };
static pthread_mutex_t names_mutex = PTHREAD_MUTEX_INITIALIZER;

struct unix_name *unix_name_dup( struct unix_name *name )
{
    pthread_mutex_lock( &names_mutex );
    name->refcnt++;
    pthread_mutex_unlock( &names_mutex );
    return name;
}

struct unix_name *unix_name_get_or_create( const char *str )
{
    struct rb_entry *entry;
    struct unix_name *s;

    pthread_mutex_lock( &names_mutex );
    entry = rb_get( &names, str );
    if (!entry)
    {
        struct unix_name *s = malloc( sizeof( struct unix_name ) );
        if (!s)
        {
            pthread_mutex_unlock( &names_mutex );
            return NULL;
        }
        s->str = strdup( str );
        s->refcnt = 0;
        rb_put( &names, str, &s->entry );
        entry = &s->entry;
    }
    s = RB_ENTRY_VALUE( entry, struct unix_name, entry );
    s->refcnt++;
    pthread_mutex_unlock( &names_mutex );
    return s;
}

void unix_name_free( struct unix_name *name )
{
    pthread_mutex_lock( &names_mutex );
    name->refcnt--;
    if (name->refcnt == 0)
    {
        rb_remove( &names, &name->entry );
        free( name );
    }
    pthread_mutex_unlock( &names_mutex );
}

static void *dbus_connection;
static void *bluetooth_watcher;
static void *bluetooth_auth_agent;

static NTSTATUS bluetooth_init ( void *params )
{
    NTSTATUS status;

#ifdef __APPLE__
    dbus_connection = corebth_init();
    if (!dbus_connection)
        return STATUS_INTERNAL_ERROR;

    status = corebth_auth_agent_start( dbus_connection, &bluetooth_auth_agent );
    if (status)
    {
        corebth_close( dbus_connection );
        return status;
    }

    status = corebth_watcher_init( dbus_connection, &bluetooth_watcher );
    if (status)
    {
        corebth_auth_agent_stop( dbus_connection, bluetooth_auth_agent );
        corebth_close( dbus_connection );
    }
    else
        TRACE( "corebth_connection=%p bluetooth_watcher=%p bluetooth_auth_agent=%p\n", dbus_connection, bluetooth_watcher,
               bluetooth_auth_agent );
#else
    dbus_connection = bluez_dbus_init();
    if (!dbus_connection)
        return STATUS_INTERNAL_ERROR;

    status = bluez_auth_agent_start( dbus_connection, &bluetooth_auth_agent );
    if (status)
    {
        bluez_dbus_close( dbus_connection );
        return status;
    }

    status = bluez_watcher_init( dbus_connection, &bluetooth_watcher );
    if (status)
    {
        bluez_auth_agent_stop( dbus_connection, bluetooth_auth_agent );
        bluez_dbus_close( dbus_connection );
    }
    else
        TRACE( "dbus_connection=%p bluetooth_watcher=%p bluetooth_auth_agent=%p\n", dbus_connection, bluetooth_watcher,
               bluetooth_auth_agent );
#endif
    return status;
}

static NTSTATUS bluetooth_shutdown( void *params )
{
    if (!dbus_connection) return STATUS_NOT_SUPPORTED;

#ifdef __APPLE__
    corebth_auth_agent_stop( dbus_connection, bluetooth_auth_agent );
    corebth_close( dbus_connection );
    corebth_watcher_close( dbus_connection, bluetooth_watcher );
    corebth_free( dbus_connection );
#else
    bluez_auth_agent_stop( dbus_connection, bluetooth_auth_agent );
    bluez_dbus_close( dbus_connection );
    bluez_watcher_close( dbus_connection, bluetooth_watcher );
    bluez_dbus_free( dbus_connection );
#endif
    return STATUS_SUCCESS;
}

static NTSTATUS get_unique_name( const struct unix_name *name, char *buf, SIZE_T *buf_size )
{
    SIZE_T path_len, i;

    path_len = strlen( name->str );
    if (*buf_size <= (path_len * sizeof(char)))
    {
        *buf_size = (path_len + 1) * sizeof(char);
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (i = 0; i < path_len; i++)
    {
        if (name->str[i] == '/') buf[i] = '_';
        else
            buf[i] = name->str[i];
    }
    buf[path_len] = '\0';
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_adapter_get_unique_name( void *args )
{
    struct bluetooth_adapter_get_unique_name_params *params = args;
    if (!dbus_connection) return STATUS_NOT_SUPPORTED;

    return get_unique_name( params->adapter, params->buf, &params->buf_size );
}

static NTSTATUS bluetooth_adapter_free( void *args )
{
    struct bluetooth_adapter_free_params *params = args;
    unix_name_free( params->adapter );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_adapter_dup( void *args )
{
    struct bluetooth_adapter_dup_params *params = args;
    unix_name_dup( params->adapter );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_adapter_set_prop( void *arg )
{
    struct bluetooth_adapter_set_prop_params *params = arg;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
#ifdef __APPLE__
    return corebth_adapter_set_prop( dbus_connection, params );
#else
    return bluez_adapter_set_prop( dbus_connection, params );
#endif
}

static NTSTATUS bluetooth_device_free( void *args )
{
    struct bluetooth_device_free_params *params = args;
    unix_name_free( params->device );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_device_dup( void *args )
{
    struct bluetooth_device_dup_params *params = args;
    unix_name_dup( params->device );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_adapter_start_discovery( void *args )
{
    struct bluetooth_adapter_start_discovery_params *params = args;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
#ifdef __APPLE__
    return corebth_adapter_start_discovery( dbus_connection, params->adapter->str );
#else
    return bluez_adapter_start_discovery( dbus_connection, params->adapter->str );
#endif
}

static NTSTATUS bluetooth_adapter_stop_discovery( void *args )
{
    struct bluetooth_adapter_stop_discovery_params *params = args;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
#ifdef __APPLE__
    return corebth_adapter_stop_discovery( dbus_connection, params->adapter->str );
#else
    return bluez_adapter_stop_discovery( dbus_connection, params->adapter->str );
#endif
}


static NTSTATUS bluetooth_adapter_remove_device( void *args )
{
    struct bluetooth_adapter_remove_device_params *params = args;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
#ifdef __APPLE__
    return corebth_adapter_remove_device( dbus_connection, params->adapter->str, params->device->str );
#else
    return bluez_adapter_remove_device( dbus_connection, params->adapter->str, params->device->str );
#endif
}

static NTSTATUS bluetooth_auth_agent_enable_incoming( void *args )
{
    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
#ifdef __APPLE__
    return corebth_auth_agent_request_default( dbus_connection );
#else
    return bluez_auth_agent_request_default( dbus_connection );
#endif
}

static NTSTATUS bluetooth_auth_send_response( void *args )
{
    struct bluetooth_auth_send_response_params *params = args;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
#ifdef __APPLE__
    return corebth_auth_agent_send_response( bluetooth_auth_agent, params->device, params->method,
                                             params->numeric_or_passkey, params->negative, params->authenticated );
#else
    return bluez_auth_agent_send_response( bluetooth_auth_agent, params->device, params->method,
                                           params->numeric_or_passkey, params->negative, params->authenticated );
#endif
}

static NTSTATUS bluetooth_device_disconnect( void *args )
{
    struct bluetooth_device_disconnect_params *params = args;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
#ifdef __APPLE__
    return corebth_device_disconnect( dbus_connection, params->device->str );
#else
    return bluez_device_disconnect( dbus_connection, params->device->str );
#endif
}

static NTSTATUS bluetooth_device_start_pairing( void *args )
{
    struct bluetooth_device_start_pairing_params *params = args;
    NTSTATUS status;

    TRACE( "dbus_connection=%p device=%p irp=%p\n", dbus_connection, (void *)params->device, params->irp );

    if (!dbus_connection)
    {
        TRACE( "dbus_connection is NULL\n" );
        return STATUS_NOT_SUPPORTED;
    }
#ifdef __APPLE__
    status = corebth_device_start_pairing( dbus_connection, bluetooth_watcher, params->device, params->irp );
    TRACE( "corebth_device_start_pairing returned %#lx\n", (unsigned long)status );
    return status;
#else
    return bluez_device_start_pairing( dbus_connection, bluetooth_watcher, params->device, params->irp );
#endif
}

static NTSTATUS bluetooth_gatt_service_free( void *args )
{
    struct bluetooth_gatt_service_free_params *params = args;
    unix_name_free( params->service );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_gatt_characteristic_free( void *args )
{
    struct bluetooth_gatt_characteristic_free_params *params = args;
    unix_name_free( params->characteristic );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_gatt_characteristic_dup( void *args )
{
    struct bluetooth_gatt_characteristic_dup_params *params = args;
    unix_name_dup( params->characteristic );
    return STATUS_SUCCESS;
}

static NTSTATUS bluetooth_gatt_characteristic_read( void *args )
{
    struct bluetooth_gatt_characteristic_read_params *params = args;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
    if (!params->characteristic || !params->characteristic->str)
    {
        ERR("bluetooth_gatt_characteristic_read: characteristic is NULL!\n");
        return STATUS_INVALID_PARAMETER;
    }
#ifdef __APPLE__
    return corebth_characteristic_read( dbus_connection, params->characteristic->str,
                                        params->buffer, params->buffer_size, params->data_len );
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static NTSTATUS bluetooth_gatt_characteristic_write( void *args )
{
    struct bluetooth_gatt_characteristic_write_params *params = args;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
    if (!params->characteristic || !params->characteristic->str)
    {
        ERR("bluetooth_gatt_characteristic_write: characteristic is NULL!\n");
        return STATUS_INVALID_PARAMETER;
    }
#ifdef __APPLE__
    return corebth_characteristic_write( dbus_connection, params->characteristic->str,
                                         params->data, params->size, params->write_type );
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static NTSTATUS bluetooth_gatt_characteristic_set_notify( void *args )
{
    struct bluetooth_gatt_characteristic_set_notify_params *params = args;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
    if (!params->characteristic || !params->characteristic->str)
    {
        ERR("bluetooth_gatt_characteristic_set_notify: characteristic is NULL!\n");
        return STATUS_INVALID_PARAMETER;
    }
#ifdef __APPLE__
    return corebth_characteristic_set_notify( dbus_connection, params->characteristic->str,
                                             params->enable );
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static NTSTATUS bluetooth_gatt_characteristic_read_notification( void *args )
{
    struct bluetooth_gatt_characteristic_read_notification_params *params = args;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
    if (!params->characteristic || !params->characteristic->str)
    {
        ERR("bluetooth_gatt_characteristic_read_notification: characteristic is NULL!\n");
        return STATUS_INVALID_PARAMETER;
    }
#ifdef __APPLE__
    {
        corebth_status ret;
        ret = corebth_characteristic_read_notification( dbus_connection, params->characteristic->str,
                                                        params->buffer, params->buffer_size, params->size );
        if (ret == COREBTH_SUCCESS) return STATUS_SUCCESS;
        if (ret == COREBTH_TIMEOUT) return STATUS_TIMEOUT;
        if (ret == COREBTH_NOT_SUPPORTED) return STATUS_NOT_SUPPORTED;
        return STATUS_INTERNAL_ERROR;
    }
#else
    return STATUS_NOT_IMPLEMENTED;
#endif
}

static NTSTATUS bluetooth_get_event( void *args )
{
    struct bluetooth_get_event_params *params = args;

    if (!dbus_connection) return STATUS_NOT_SUPPORTED;
    memset( &params->result, 0, sizeof( params->result ) );
#ifdef __APPLE__
    return corebth_loop( dbus_connection, bluetooth_watcher, bluetooth_auth_agent, &params->result );
#else
    return bluez_dbus_loop( dbus_connection, bluetooth_watcher, bluetooth_auth_agent, &params->result );
#endif
}

const unixlib_entry_t __wine_unix_call_funcs[] = {
    bluetooth_init,
    bluetooth_shutdown,

    bluetooth_adapter_set_prop,
    bluetooth_adapter_get_unique_name,
    bluetooth_adapter_start_discovery,
    bluetooth_adapter_stop_discovery,
    bluetooth_adapter_remove_device,
    bluetooth_adapter_free,
    bluetooth_adapter_dup,

    bluetooth_device_free,
    bluetooth_device_dup,
    bluetooth_device_disconnect,
    bluetooth_device_start_pairing,

    bluetooth_auth_agent_enable_incoming,
    bluetooth_auth_send_response,

    bluetooth_gatt_service_free,

    bluetooth_gatt_characteristic_free,
    bluetooth_gatt_characteristic_dup,
    bluetooth_gatt_characteristic_read,
    bluetooth_gatt_characteristic_write,
    bluetooth_gatt_characteristic_set_notify,
    bluetooth_gatt_characteristic_read_notification,

    bluetooth_get_event,
};

C_ASSERT( ARRAYSIZE( __wine_unix_call_funcs ) == unix_funcs_count );
