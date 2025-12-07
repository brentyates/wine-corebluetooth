/*
 * CoreBluetooth backend for Wine Bluetooth driver
 *
 * Copyright 2024 Wine Project
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

#include "config.h"

#ifdef __APPLE__

#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

typedef int32_t corebth_status;
#define COREBTH_SUCCESS            0
#define COREBTH_NOT_SUPPORTED      ((corebth_status)0xC00000BB)
#define COREBTH_PENDING            ((corebth_status)0x00000103)
#define COREBTH_DEVICE_NOT_READY   ((corebth_status)0xC00000A3)
#define COREBTH_INTERNAL_ERROR     ((corebth_status)0xC00000E5)

#define BLUETOOTH_MAX_NAME_SIZE 248

#define WINEBLUETOOTH_RADIO_PROPERTY_NAME         (1)
#define WINEBLUETOOTH_RADIO_PROPERTY_ADDRESS      (1 << 2)
#define WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERABLE (1 << 3)
#define WINEBLUETOOTH_RADIO_PROPERTY_CONNECTABLE  (1 << 4)
#define WINEBLUETOOTH_RADIO_PROPERTY_CLASS        (1 << 5)
#define WINEBLUETOOTH_RADIO_PROPERTY_MANUFACTURER (1 << 6)
#define WINEBLUETOOTH_RADIO_PROPERTY_VERSION      (1 << 7)
#define WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERING  (1 << 8)
#define WINEBLUETOOTH_RADIO_PROPERTY_PAIRABLE     (1 << 9)

#define WINEBLUETOOTH_RADIO_ALL_PROPERTIES \
    (WINEBLUETOOTH_RADIO_PROPERTY_NAME | WINEBLUETOOTH_RADIO_PROPERTY_ADDRESS | \
     WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERABLE | WINEBLUETOOTH_RADIO_PROPERTY_CONNECTABLE | \
     WINEBLUETOOTH_RADIO_PROPERTY_CLASS | WINEBLUETOOTH_RADIO_PROPERTY_MANUFACTURER | \
     WINEBLUETOOTH_RADIO_PROPERTY_VERSION | WINEBLUETOOTH_RADIO_PROPERTY_DISCOVERING | \
     WINEBLUETOOTH_RADIO_PROPERTY_PAIRABLE)

#define WINEBLUETOOTH_DEVICE_PROPERTY_NAME           (1)
#define WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS        (1 << 1)
#define WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED      (1 << 2)
#define WINEBLUETOOTH_DEVICE_PROPERTY_PAIRED         (1 << 3)
#define WINEBLUETOOTH_DEVICE_PROPERTY_LEGACY_PAIRING (1 << 4)
#define WINEBLUETOOTH_DEVICE_PROPERTY_TRUSTED        (1 << 5)
#define WINEBLUETOOTH_DEVICE_PROPERTY_CLASS          (1 << 6)

#define WINEBLUETOOTH_DEVICE_ALL_PROPERTIES \
    ( WINEBLUETOOTH_DEVICE_PROPERTY_NAME | WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS | \
      WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED | WINEBLUETOOTH_DEVICE_PROPERTY_PAIRED | \
      WINEBLUETOOTH_DEVICE_PROPERTY_LEGACY_PAIRING | WINEBLUETOOTH_DEVICE_PROPERTY_TRUSTED | \
      WINEBLUETOOTH_DEVICE_PROPERTY_CLASS )

enum corebth_watcher_event_type
{
    COREBTH_EVENT_RADIO_ADDED = 0,
    COREBTH_EVENT_RADIO_REMOVED,
    COREBTH_EVENT_RADIO_PROPS_CHANGED,
    COREBTH_EVENT_DEVICE_ADDED,
    COREBTH_EVENT_DEVICE_REMOVED,
    COREBTH_EVENT_DEVICE_PROPS_CHANGED,
    COREBTH_EVENT_PAIRING_FINISHED,
    COREBTH_EVENT_GATT_SERVICE_ADDED,
    COREBTH_EVENT_GATT_SERVICE_REMOVED,
    COREBTH_EVENT_GATT_CHAR_ADDED,
    COREBTH_EVENT_GATT_CHAR_REMOVED,
};

enum corebth_event_type
{
    COREBTH_EVENT_WATCHER = 0,
    COREBTH_EVENT_AUTH,
};

typedef union {
    uint64_t ullLong;
    uint8_t rgBytes[6];
} corebth_bt_addr_t;

struct corebth_radio_props
{
    int32_t discoverable;
    int32_t connectable;
    int32_t discovering;
    int32_t pairable;
    corebth_bt_addr_t address;
    char name[BLUETOOTH_MAX_NAME_SIZE];
    uint32_t device_class;
    uint16_t manufacturer;
    uint8_t version;
};

struct corebth_radio
{
    uintptr_t handle;
};

struct corebth_radio_added_event
{
    uint16_t props_mask;  /* Must match winebluetooth_radio_props_mask_t (UINT16) */
    struct corebth_radio_props props;
    struct corebth_radio radio;
};

struct corebth_device
{
    uintptr_t handle;
};

struct corebth_device_props
{
    corebth_bt_addr_t address;
    char name[BLUETOOTH_MAX_NAME_SIZE];
    int32_t connected;
    int32_t paired;
    int32_t legacy_pairing;
    int32_t trusted;
    uint32_t device_class;
};

struct corebth_device_added_event
{
    uint32_t known_props_mask;
    struct corebth_device_props props;
    struct corebth_device device;
    struct corebth_radio radio;
    int init_entry;
};

struct corebth_device_removed_event
{
    struct corebth_device device;
};

struct corebth_watcher_event
{
    enum corebth_watcher_event_type event_type;
    union {
        struct corebth_radio_added_event radio_added;
        struct corebth_radio radio_removed;
        struct corebth_device_added_event device_added;
        struct corebth_device_removed_event device_removed;
    } data;
};

struct corebth_event
{
    enum corebth_event_type status;
    union {
        struct corebth_watcher_event watcher_event;
    } data;
};

struct corebth_event_entry
{
    struct corebth_event_entry *next;
    struct corebth_watcher_event event;
};

struct corebth_peripheral_entry
{
    struct corebth_peripheral_entry *next;
    CBPeripheral *peripheral;
    uintptr_t handle;
    char uuid_string[64];
};

struct corebth_context
{
    CBCentralManager *central_manager;
    dispatch_queue_t bt_queue;

    pthread_mutex_t event_mutex;
    struct corebth_event_entry *event_head;
    struct corebth_event_entry *event_tail;

    pthread_mutex_t peripheral_mutex;
    struct corebth_peripheral_entry *peripherals;
    uintptr_t next_peripheral_handle;

    int initialized;
    int radio_added;
    int discovering;
    CBManagerState state;
    CBManagerState last_state;

    uintptr_t radio_handle;
};

static struct corebth_context *global_context;

static void corebth_queue_event(struct corebth_context *ctx, struct corebth_watcher_event *event)
{
    struct corebth_event_entry *entry = calloc(1, sizeof(*entry));
    if (!entry) return;

    entry->event = *event;
    entry->next = NULL;

    pthread_mutex_lock(&ctx->event_mutex);
    if (ctx->event_tail) {
        ctx->event_tail->next = entry;
    } else {
        ctx->event_head = entry;
    }
    ctx->event_tail = entry;
    pthread_mutex_unlock(&ctx->event_mutex);
}

static int corebth_dequeue_event(struct corebth_context *ctx, struct corebth_watcher_event *event)
{
    struct corebth_event_entry *entry;

    pthread_mutex_lock(&ctx->event_mutex);
    entry = ctx->event_head;
    if (entry) {
        ctx->event_head = entry->next;
        if (!ctx->event_head) {
            ctx->event_tail = NULL;
        }
    }
    pthread_mutex_unlock(&ctx->event_mutex);

    if (entry) {
        *event = entry->event;
        free(entry);
        return 1;
    }
    return 0;
}

static void corebth_queue_radio_added(struct corebth_context *ctx)
{
    struct corebth_watcher_event event;
    struct corebth_radio_added_event *radio_added = &event.data.radio_added;
    IOBluetoothHostController *controller;
    NSString *addrString;
    NSString *nameString;

    memset(&event, 0, sizeof(event));
    event.event_type = COREBTH_EVENT_RADIO_ADDED;

    radio_added->props_mask = WINEBLUETOOTH_RADIO_ALL_PROPERTIES;

    radio_added->props.discoverable = 0;
    radio_added->props.connectable = 1;
    radio_added->props.discovering = 0;
    radio_added->props.pairable = 1;

    controller = [IOBluetoothHostController defaultController];
    if (controller) {
        addrString = [controller addressAsString];
        if (addrString) {
            const char *addr = [addrString UTF8String];
            unsigned int b[6];
            if (sscanf(addr, "%x-%x-%x-%x-%x-%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6 ||
                sscanf(addr, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
                radio_added->props.address.rgBytes[0] = b[5];
                radio_added->props.address.rgBytes[1] = b[4];
                radio_added->props.address.rgBytes[2] = b[3];
                radio_added->props.address.rgBytes[3] = b[2];
                radio_added->props.address.rgBytes[4] = b[1];
                radio_added->props.address.rgBytes[5] = b[0];
            }
        }

        nameString = [controller nameAsString];
        if (nameString) {
            strncpy(radio_added->props.name, [nameString UTF8String], BLUETOOTH_MAX_NAME_SIZE - 1);
        } else {
            strncpy(radio_added->props.name, "macOS Bluetooth", BLUETOOTH_MAX_NAME_SIZE - 1);
        }

        radio_added->props.device_class = [controller classOfDevice];
    } else {
        strncpy(radio_added->props.name, "macOS Bluetooth", BLUETOOTH_MAX_NAME_SIZE - 1);
        radio_added->props.device_class = 0;
    }

    radio_added->props.manufacturer = 0x004C;
    radio_added->props.version = 0x09;

    ctx->radio_handle = (uintptr_t)ctx;
    radio_added->radio.handle = ctx->radio_handle;

    NSLog(@"Wine: Queuing RADIO_ADDED event - name=%s addr=%02x:%02x:%02x:%02x:%02x:%02x",
          radio_added->props.name,
          radio_added->props.address.rgBytes[5], radio_added->props.address.rgBytes[4],
          radio_added->props.address.rgBytes[3], radio_added->props.address.rgBytes[2],
          radio_added->props.address.rgBytes[1], radio_added->props.address.rgBytes[0]);

    corebth_queue_event(ctx, &event);
    ctx->radio_added = 1;
}

static void corebth_queue_radio_removed(struct corebth_context *ctx)
{
    struct corebth_watcher_event event;

    memset(&event, 0, sizeof(event));
    event.event_type = COREBTH_EVENT_RADIO_REMOVED;
    event.data.radio_removed.handle = ctx->radio_handle;

    NSLog(@"Wine: Queuing RADIO_REMOVED event");

    corebth_queue_event(ctx, &event);
    ctx->radio_added = 0;
}

static struct corebth_peripheral_entry *corebth_find_peripheral(struct corebth_context *ctx, const char *uuid_str)
{
    struct corebth_peripheral_entry *entry;

    for (entry = ctx->peripherals; entry; entry = entry->next) {
        if (strcmp(entry->uuid_string, uuid_str) == 0)
            return entry;
    }
    return NULL;
}

static struct corebth_peripheral_entry *corebth_add_peripheral(struct corebth_context *ctx,
                                                                CBPeripheral *peripheral,
                                                                const char *uuid_str)
{
    struct corebth_peripheral_entry *entry;

    entry = calloc(1, sizeof(*entry));
    if (!entry) return NULL;

    entry->peripheral = peripheral;
    entry->handle = ++ctx->next_peripheral_handle;
    strncpy(entry->uuid_string, uuid_str, sizeof(entry->uuid_string) - 1);

    entry->next = ctx->peripherals;
    ctx->peripherals = entry;

    return entry;
}

static void corebth_uuid_to_address(const char *uuid_str, corebth_bt_addr_t *addr)
{
    unsigned int hash = 0;
    const char *p;
    int i;

    for (p = uuid_str; *p; p++) {
        hash = hash * 31 + (unsigned char)*p;
    }

    for (i = 0; i < 6; i++) {
        addr->rgBytes[i] = (hash >> (i * 5)) & 0xFF;
    }
    addr->rgBytes[0] |= 0xC0;
}

static void corebth_queue_device_added(struct corebth_context *ctx,
                                        CBPeripheral *peripheral,
                                        NSDictionary *advertisementData,
                                        NSNumber *rssi)
{
    struct corebth_watcher_event event;
    struct corebth_device_added_event *device_added = &event.data.device_added;
    struct corebth_peripheral_entry *entry;
    const char *uuid_str;
    const char *name;
    NSString *uuidString;
    NSString *localName;

    uuidString = [peripheral.identifier UUIDString];
    uuid_str = [uuidString UTF8String];

    pthread_mutex_lock(&ctx->peripheral_mutex);
    entry = corebth_find_peripheral(ctx, uuid_str);
    if (entry) {
        pthread_mutex_unlock(&ctx->peripheral_mutex);
        return;
    }

    entry = corebth_add_peripheral(ctx, peripheral, uuid_str);
    pthread_mutex_unlock(&ctx->peripheral_mutex);

    if (!entry) return;

    memset(&event, 0, sizeof(event));
    event.event_type = COREBTH_EVENT_DEVICE_ADDED;

    device_added->known_props_mask = WINEBLUETOOTH_DEVICE_ALL_PROPERTIES;

    corebth_uuid_to_address(uuid_str, &device_added->props.address);

    localName = advertisementData[CBAdvertisementDataLocalNameKey];
    if (localName) {
        name = [localName UTF8String];
    } else if (peripheral.name) {
        name = [peripheral.name UTF8String];
    } else {
        name = "BLE Device";
    }
    strncpy(device_added->props.name, name, BLUETOOTH_MAX_NAME_SIZE - 1);

    device_added->props.connected = (peripheral.state == CBPeripheralStateConnected) ? 1 : 0;
    device_added->props.paired = 0;
    device_added->props.legacy_pairing = 0;
    device_added->props.trusted = 0;
    device_added->props.device_class = 0;

    device_added->device.handle = entry->handle;
    device_added->radio.handle = ctx->radio_handle;
    device_added->init_entry = 0;

    NSLog(@"Wine: Queuing DEVICE_ADDED event - name=%s uuid=%s handle=%lu",
          device_added->props.name, uuid_str, (unsigned long)entry->handle);

    corebth_queue_event(ctx, &event);
}

@interface WineBluetoothDelegate : NSObject <CBCentralManagerDelegate>
@property (nonatomic, assign) struct corebth_context *ctx;
@end

@implementation WineBluetoothDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    CBManagerState oldState;

    NSLog(@"Wine: CoreBluetooth state changed to %ld", (long)central.state);

    if (!self.ctx) return;

    oldState = self.ctx->last_state;
    self.ctx->state = central.state;
    self.ctx->last_state = central.state;

    if (central.state == CBManagerStatePoweredOn && !self.ctx->radio_added) {
        corebth_queue_radio_added(self.ctx);
    }
    else if (oldState == CBManagerStatePoweredOn && central.state != CBManagerStatePoweredOn && self.ctx->radio_added) {
        corebth_queue_radio_removed(self.ctx);
    }
}

- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary<NSString *,id> *)advertisementData
                  RSSI:(NSNumber *)RSSI
{
    NSLog(@"Wine: discovered peripheral: %@ name: %@", [peripheral.identifier UUIDString], peripheral.name);

    if (self.ctx && self.ctx->discovering) {
        corebth_queue_device_added(self.ctx, peripheral, advertisementData, RSSI);
    }
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
    NSLog(@"Wine: connected to peripheral: %@", [peripheral.identifier UUIDString]);
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    NSLog(@"Wine: disconnected from peripheral: %@", [peripheral.identifier UUIDString]);
}

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    NSLog(@"Wine: failed to connect to peripheral: %@, error: %@",
          [peripheral.identifier UUIDString],
          error ? [error localizedDescription] : @"(null)");
}

@end

void *corebth_init( void )
{
    struct corebth_context *ctx;
    WineBluetoothDelegate *delegate;

    NSLog(@"Wine: initializing CoreBluetooth backend");

    ctx = calloc( 1, sizeof( *ctx ) );
    if (!ctx) return NULL;

    pthread_mutex_init( &ctx->event_mutex, NULL );
    pthread_mutex_init( &ctx->peripheral_mutex, NULL );
    ctx->event_head = NULL;
    ctx->event_tail = NULL;
    ctx->peripherals = NULL;
    ctx->next_peripheral_handle = 0;
    ctx->radio_added = 0;
    ctx->discovering = 0;
    ctx->last_state = CBManagerStateUnknown;

    ctx->bt_queue = dispatch_queue_create( "org.winehq.bluetooth", DISPATCH_QUEUE_SERIAL );
    if (!ctx->bt_queue)
    {
        free( ctx );
        return NULL;
    }

    delegate = [[WineBluetoothDelegate alloc] init];
    delegate.ctx = ctx;

    ctx->central_manager = [[CBCentralManager alloc]
        initWithDelegate:delegate
        queue:ctx->bt_queue
        options:@{CBCentralManagerOptionShowPowerAlertKey: @NO}];

    if (!ctx->central_manager)
    {
        dispatch_release( ctx->bt_queue );
        free( ctx );
        return NULL;
    }

    ctx->initialized = 1;
    global_context = ctx;

    NSLog(@"Wine: CoreBluetooth backend initialized: ctx=%p", ctx);
    return ctx;
}

void corebth_close( void *connection )
{
    struct corebth_context *ctx = connection;

    NSLog(@"Wine: closing CoreBluetooth backend: ctx=%p", ctx);

    if (!ctx) return;

    if (ctx->central_manager)
    {
        [ctx->central_manager stopScan];
        ctx->central_manager = nil;
    }

    ctx->initialized = 0;
}

void corebth_free( void *connection )
{
    struct corebth_context *ctx = connection;
    struct corebth_event_entry *entry, *next_event;
    struct corebth_peripheral_entry *peripheral, *next_peripheral;

    NSLog(@"Wine: freeing CoreBluetooth backend: ctx=%p", ctx);

    if (!ctx) return;

    pthread_mutex_lock(&ctx->event_mutex);
    entry = ctx->event_head;
    while (entry) {
        next_event = entry->next;
        free(entry);
        entry = next_event;
    }
    ctx->event_head = NULL;
    ctx->event_tail = NULL;
    pthread_mutex_unlock(&ctx->event_mutex);

    pthread_mutex_lock(&ctx->peripheral_mutex);
    peripheral = ctx->peripherals;
    while (peripheral) {
        next_peripheral = peripheral->next;
        free(peripheral);
        peripheral = next_peripheral;
    }
    ctx->peripherals = NULL;
    pthread_mutex_unlock(&ctx->peripheral_mutex);

    if (ctx->bt_queue)
        dispatch_release( ctx->bt_queue );

    pthread_mutex_destroy( &ctx->event_mutex );
    pthread_mutex_destroy( &ctx->peripheral_mutex );

    if (global_context == ctx)
        global_context = NULL;

    free( ctx );
}

corebth_status corebth_loop( void *connection, void *watcher_ctx, void *auth_agent,
                             void *result )
{
    struct corebth_context *ctx = connection;
    struct corebth_event *evt = (struct corebth_event *)result;
    struct corebth_watcher_event watcher_event;

    if (!ctx || !ctx->initialized)
        return COREBTH_NOT_SUPPORTED;

    while (1)
    {
        if (corebth_dequeue_event(ctx, &watcher_event)) {
            evt->status = COREBTH_EVENT_WATCHER;
            memcpy(&evt->data.watcher_event, &watcher_event, sizeof(watcher_event));
            return COREBTH_PENDING;
        }

        usleep(100000);
    }
}

corebth_status corebth_adapter_set_prop( void *connection, void *params )
{
    return COREBTH_NOT_SUPPORTED;
}

corebth_status corebth_adapter_start_discovery( void *connection, const char *adapter_path )
{
    struct corebth_context *ctx = connection;

    NSLog(@"Wine: starting discovery");

    if (!ctx || !ctx->central_manager)
        return COREBTH_NOT_SUPPORTED;

    if (ctx->state != CBManagerStatePoweredOn)
    {
        NSLog(@"Wine: Bluetooth not powered on (state=%ld)", (long)ctx->state);
        return COREBTH_DEVICE_NOT_READY;
    }

    ctx->discovering = 1;
    [ctx->central_manager scanForPeripheralsWithServices:nil
                                                 options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @NO}];

    return COREBTH_SUCCESS;
}

corebth_status corebth_adapter_stop_discovery( void *connection, const char *adapter_path )
{
    struct corebth_context *ctx = connection;

    NSLog(@"Wine: stopping discovery");

    if (!ctx || !ctx->central_manager)
        return COREBTH_NOT_SUPPORTED;

    ctx->discovering = 0;

    [ctx->central_manager stopScan];

    return COREBTH_SUCCESS;
}

corebth_status corebth_adapter_remove_device( void *connection, const char *adapter_path,
                                              const char *device_path )
{
    return COREBTH_NOT_SUPPORTED;
}

corebth_status corebth_auth_agent_request_default( void *connection )
{
    return COREBTH_SUCCESS;
}

corebth_status corebth_auth_agent_start( void *connection, void **ctx )
{
    NSLog(@"Wine: auth agent not needed on macOS");
    *ctx = NULL;
    return COREBTH_SUCCESS;
}

void corebth_auth_agent_stop( void *connection, void *ctx )
{
    NSLog(@"Wine: auth agent stop (no-op on macOS)");
}

corebth_status corebth_auth_agent_send_response( void *auth_agent, void *device,
                                                 int method, uint32_t numeric_or_passkey,
                                                 int negative, int *authenticated )
{
    if (authenticated) *authenticated = 1;
    return COREBTH_SUCCESS;
}

corebth_status corebth_device_disconnect( void *connection, const char *device_path )
{
    return COREBTH_NOT_SUPPORTED;
}

corebth_status corebth_device_start_pairing( void *connection, void *watcher_ctx,
                                             void *device, void *irp )
{
    return COREBTH_NOT_SUPPORTED;
}

corebth_status corebth_watcher_init( void *connection, void **ctx )
{
    NSLog(@"Wine: watcher init");
    *ctx = connection;
    return COREBTH_SUCCESS;
}

void corebth_watcher_close( void *connection, void *ctx )
{
    NSLog(@"Wine: watcher close");
}

#endif /* __APPLE__ */
