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
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

struct corebth_context;
struct corebth_peripheral_entry;

@interface WinePeripheralDelegate : NSObject <CBPeripheralDelegate>
@property (nonatomic, assign) struct corebth_context *ctx;
@property (nonatomic, assign) struct corebth_peripheral_entry *periph;
@end

#define COREBTH_PERIPH_BUCKETS 256

struct corebth_peripheral_map_entry
{
    char uuid_string[64];
    struct corebth_peripheral_entry *entry;
    struct corebth_peripheral_map_entry *next;
};

struct corebth_peripheral_map
{
    struct corebth_peripheral_map_entry *buckets[COREBTH_PERIPH_BUCKETS];
};

typedef int WINE_BOOL;
#define BOOL WINE_BOOL
#define LoadResource WINE_LoadResource

#include <windef.h>
#include <winternl.h>
#include <winbase.h>
#include <bthsdpdef.h>
#include <bluetoothapis.h>
#include <bthledef.h>
#include <bthdef.h>

#undef BOOL
#undef LoadResource
#undef strncpy

#include "winebth_priv.h"
#include "unixlib_priv.h"

typedef int32_t corebth_status;
#define COREBTH_SUCCESS            0
#define COREBTH_NOT_SUPPORTED      ((corebth_status)0xC00000BB)
#define COREBTH_PENDING            ((corebth_status)0x00000103)
#define COREBTH_DEVICE_NOT_READY   ((corebth_status)0xC00000A3)
#define COREBTH_INTERNAL_ERROR     ((corebth_status)0xC00000E5)
#define COREBTH_INVALID_PARAMETER  ((corebth_status)0xC000000D)
#define COREBTH_TIMEOUT            ((corebth_status)0x00000102)

#define BLUETOOTH_MAX_NAME_SIZE 248

#ifndef WINEBLUETOOTH_RADIO_PROPERTY_NAME
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
#endif

#ifndef WINEBLUETOOTH_DEVICE_PROPERTY_NAME
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
#endif

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

struct corebth_device_props_changed_event
{
    uint16_t changed_props_mask;
    struct corebth_device_props props;
    uint16_t invalid_props_mask;
    struct corebth_device device;
};

struct corebth_gatt_service
{
    uintptr_t handle;
};

struct corebth_gatt_characteristic
{
    uintptr_t handle;
};

struct corebth_gatt_service_added_event
{
    struct corebth_device device;
    struct corebth_gatt_service service;
    uint16_t attr_handle;
    int is_primary;
    GUID uuid;
};

struct corebth_gatt_service_removed_event
{
    struct corebth_gatt_service service;
};

struct corebth_gatt_char_added_event
{
    struct corebth_gatt_characteristic characteristic;
    struct corebth_gatt_service service;
    BTH_LE_GATT_CHARACTERISTIC props;
};

struct corebth_gatt_char_removed_event
{
    struct corebth_gatt_characteristic characteristic;
};

struct corebth_watcher_event
{
    enum corebth_watcher_event_type event_type;
    union {
        struct corebth_radio_added_event radio_added;
        struct corebth_radio radio_removed;
        struct corebth_device_added_event device_added;
        struct corebth_device_removed_event device_removed;
        struct corebth_device_props_changed_event device_props_changed;
        struct corebth_gatt_service_added_event gatt_service_added;
        struct corebth_gatt_service_removed_event gatt_service_removed;
        struct corebth_gatt_char_added_event gatt_char_added;
        struct corebth_gatt_char_removed_event gatt_char_removed;
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
    struct unix_name *name;
    uintptr_t handle;
    char uuid_string[64];
    char friendly_name[BLUETOOTH_MAX_NAME_SIZE];
    int has_local_name;
    int device_added;  /* Whether DEVICE_ADDED event has been sent to driver */
    struct corebth_service_entry *services;
    uint16_t next_service_handle;
    void *peripheral_delegate;
    dispatch_semaphore_t services_discovered;
    int services_discovery_complete;
    int pending_char_discovery_count;  /* Number of services awaiting char discovery */
    int char_discovery_started;  /* Guard against duplicate didDiscoverServices callbacks */
};

struct corebth_char_entry;

struct corebth_service_entry
{
    struct corebth_service_entry *next;
    struct corebth_service_entry *next_global;
    struct corebth_peripheral_entry *peripheral;
    CBService *service;
    struct unix_name *path;
    GUID uuid;
    uint16_t attr_handle;
    int is_primary;
    struct corebth_char_entry *chars;
    uint16_t next_char_handle;
};

struct corebth_notification_entry
{
    struct corebth_notification_entry *next;
    unsigned char *data;
    unsigned int data_len;
};

#define COREBTH_MAX_CHAR_VALUE_SIZE 512
#define COREBTH_IDLE               ((corebth_status)0)
#define COREBTH_CANCELLED          ((corebth_status)0xC0000120)
#define COREBTH_DEVICE_DISCONNECTED ((corebth_status)0xC000020B)

struct corebth_char_entry
{
    struct corebth_char_entry *next;
    struct corebth_char_entry *next_global;
    struct corebth_service_entry *service;
    CBCharacteristic *characteristic;
    struct unix_name *path;
    BTH_LE_GATT_CHARACTERISTIC props;
    dispatch_semaphore_t pending_read;
    dispatch_semaphore_t pending_write;
    unsigned char *pending_read_buffer;
    unsigned int pending_read_size;
    unsigned int *pending_read_len;
    _Atomic corebth_status pending_read_status;
    _Atomic corebth_status pending_write_status;
    _Atomic BOOL invalidated;
    _Atomic unsigned int pending_read_actual_len;
    unsigned char pending_read_internal_buffer[COREBTH_MAX_CHAR_VALUE_SIZE];
    uint32_t read_generation;
    uint32_t read_completed_gen;
    uint32_t write_generation;
    uint32_t write_completed_gen;
    BOOL notifications_enabled;
    BOOL read_awaiting_response;  /* Set on bt_queue right before readValueForCharacteristic */
    pthread_mutex_t notification_mutex;
    dispatch_semaphore_t notification_semaphore;
    struct corebth_notification_entry *notification_queue_head;
    struct corebth_notification_entry *notification_queue_tail;
    int ref_count;
    unsigned char *cached_value;
    unsigned int cached_value_len;
    BOOL cached_value_valid;
};

struct corebth_context
{
    CBCentralManager *central_manager;
    id central_delegate;  /* Retained reference to WineBluetoothDelegate */
    dispatch_queue_t bt_queue;

    pthread_mutex_t event_mutex;
    struct corebth_event_entry *event_head;
    struct corebth_event_entry *event_tail;

    pthread_mutex_t peripheral_mutex;
    struct corebth_peripheral_entry *peripherals;
    struct corebth_peripheral_map uuid_map;
    uintptr_t next_peripheral_handle;

    pthread_mutex_t service_list_mutex;
    struct corebth_service_entry *services;
    struct corebth_char_entry *characteristics;

    int initialized;
    int radio_added;
    int discovering;
    CBManagerState state;
    CBManagerState last_state;

    uintptr_t radio_handle;
};

static inline void char_set_read_status(struct corebth_char_entry *ch, corebth_status status)
{
    atomic_store_explicit(&ch->pending_read_status, status, memory_order_release);
}

static inline corebth_status char_get_read_status(struct corebth_char_entry *ch)
{
    return atomic_load_explicit(&ch->pending_read_status, memory_order_acquire);
}

static inline void char_set_write_status(struct corebth_char_entry *ch, corebth_status status)
{
    atomic_store_explicit(&ch->pending_write_status, status, memory_order_release);
}

static inline corebth_status char_get_write_status(struct corebth_char_entry *ch)
{
    return atomic_load_explicit(&ch->pending_write_status, memory_order_acquire);
}

static inline void char_set_invalidated(struct corebth_char_entry *ch)
{
    atomic_store_explicit(&ch->invalidated, YES, memory_order_release);
}

static inline BOOL char_is_invalidated(struct corebth_char_entry *ch)
{
    return atomic_load_explicit(&ch->invalidated, memory_order_acquire);
}

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

    memset(&event, 0, sizeof(event));
    event.event_type = COREBTH_EVENT_RADIO_ADDED;

    radio_added->props_mask = WINEBLUETOOTH_RADIO_ALL_PROPERTIES;

    radio_added->props.discoverable = 0;
    radio_added->props.connectable = 1;
    radio_added->props.discovering = 0;
    radio_added->props.pairable = 1;

    @autoreleasepool {
        IOBluetoothHostController *controller = [IOBluetoothHostController defaultController];
        if (controller) {
            NSString *addrString = [controller addressAsString];
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

            NSString *nameString = [controller nameAsString];
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
    }

    radio_added->props.manufacturer = 0x004C;
    radio_added->props.version = 0x09;

    ctx->radio_handle = (uintptr_t)ctx;
    radio_added->radio.handle = ctx->radio_handle;

    corebth_queue_event(ctx, &event);
    ctx->radio_added = 1;
}

static void corebth_queue_radio_removed(struct corebth_context *ctx)
{
    struct corebth_watcher_event event;

    memset(&event, 0, sizeof(event));
    event.event_type = COREBTH_EVENT_RADIO_REMOVED;
    event.data.radio_removed.handle = ctx->radio_handle;

    corebth_queue_event(ctx, &event);
    ctx->radio_added = 0;
}

static unsigned corebth_peripheral_hash(const char *uuid_str)
{
    unsigned hash = 5381;
    const unsigned char *p = (const unsigned char *)uuid_str;
    while (*p)
        hash = ((hash << 5) + hash) + *p++;
    return hash & (COREBTH_PERIPH_BUCKETS - 1);
}

static struct corebth_peripheral_entry *corebth_peripheral_map_lookup(struct corebth_peripheral_map *map, const char *uuid_str)
{
    struct corebth_peripheral_map_entry *e = map->buckets[corebth_peripheral_hash(uuid_str)];
    while (e)
    {
        if (!strcmp(e->uuid_string, uuid_str))
            return e->entry;
        e = e->next;
    }
    return NULL;
}

static void corebth_peripheral_map_insert(struct corebth_peripheral_map *map, struct corebth_peripheral_entry *entry)
{
    unsigned idx = corebth_peripheral_hash(entry->uuid_string);
    struct corebth_peripheral_map_entry *e = calloc(1, sizeof(*e));
    if (!e) return;
    strncpy(e->uuid_string, entry->uuid_string, sizeof(e->uuid_string) - 1);
    e->entry = entry;
    e->next = map->buckets[idx];
    map->buckets[idx] = e;
}

static void corebth_peripheral_map_remove(struct corebth_peripheral_map *map, const char *uuid_str)
{
    unsigned idx = corebth_peripheral_hash(uuid_str);
    struct corebth_peripheral_map_entry **pp = &map->buckets[idx];
    while (*pp)
    {
        if (!strcmp((*pp)->uuid_string, uuid_str))
        {
            struct corebth_peripheral_map_entry *old = *pp;
            *pp = old->next;
            free(old);
            return;
        }
        pp = &(*pp)->next;
    }
}

static struct corebth_peripheral_entry *corebth_find_peripheral(struct corebth_context *ctx, const char *uuid_str)
{
    return corebth_peripheral_map_lookup(&ctx->uuid_map, uuid_str);
}

static struct corebth_peripheral_entry *corebth_add_peripheral(struct corebth_context *ctx,
                                                                CBPeripheral *peripheral,
                                                                const char *uuid_str)
{
    struct corebth_peripheral_entry *entry;

    entry = calloc(1, sizeof(*entry));
    if (!entry) return NULL;

    entry->peripheral = (CBPeripheral *)CFBridgingRetain(peripheral);
    entry->next_service_handle = 1;
    entry->services_discovered = dispatch_semaphore_create(0);
    entry->services_discovery_complete = 0;
    entry->pending_char_discovery_count = 0;
    entry->char_discovery_started = 0;
    entry->device_added = 0;
    snprintf(entry->uuid_string, sizeof(entry->uuid_string), "%s", uuid_str);

    char path[128];
    snprintf(path, sizeof(path), "/org/wine/corebth/device/%s", uuid_str);
    entry->name = unix_name_get_or_create(path);
    if (!entry->name)
    {
        dispatch_release(entry->services_discovered);
        free(entry);
        return NULL;
    }

    entry->handle = (uintptr_t)entry->name;

    entry->next = ctx->peripherals;
    ctx->peripherals = entry;
    corebth_peripheral_map_insert(&ctx->uuid_map, entry);

    return entry;
}

static void corebth_uuid_to_address(const char *uuid_str, corebth_bt_addr_t *addr)
{
    unsigned long long hash = 5381;
    int c;
    const char *p = uuid_str;

    /* DJB2 Hash Algorithm to generate stable 48-bit address from UUID string */
    while ((c = *p++)) {
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    }

    /* Ensure it is NOT zero and fits in 48-bit (standard MAC size) */
    unsigned long long address = (hash & 0xFFFFFFFFFFFF);

    if (address == 0) address = 0xDEADBEEF; /* Fallback for the 1-in-a-billion chance */

    int i;
    for (i = 0; i < 6; i++) {
        addr->rgBytes[i] = (address >> (i * 8)) & 0xFF;
    }
}

static int corebth_string_to_guid(const char *str, GUID *guid)
{
    unsigned int d1, d2, d3, d4[8];

    /* UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (8-4-4-4-12) */
    if (sscanf(str, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
               &d1, &d2, &d3,
               &d4[0], &d4[1], &d4[2], &d4[3], &d4[4], &d4[5], &d4[6], &d4[7]) != 11)
        return 0;

    guid->Data1 = d1;
    guid->Data2 = d2;
    guid->Data3 = d3;
    guid->Data4[0] = d4[0];
    guid->Data4[1] = d4[1];
    guid->Data4[2] = d4[2];
    guid->Data4[3] = d4[3];
    guid->Data4[4] = d4[4];
    guid->Data4[5] = d4[5];
    guid->Data4[6] = d4[6];
    guid->Data4[7] = d4[7];
    return 1;
}

static void corebth_cbuuid_to_guid(CBUUID *uuid, GUID *guid)
{
    NSData *data = [uuid data];
    const uint8_t *bytes = data.bytes;

    if (data.length == 2)
    {
        unsigned short short_uuid = (bytes[0] << 8) | bytes[1];  /* CoreBluetooth uses big-endian */
        guid->Data1 = short_uuid;
        guid->Data2 = 0x0000;
        guid->Data3 = 0x1000;
        guid->Data4[0] = 0x80;
        guid->Data4[1] = 0x00;
        guid->Data4[2] = 0x00;
        guid->Data4[3] = 0x80;
        guid->Data4[4] = 0x5f;
        guid->Data4[5] = 0x9b;
        guid->Data4[6] = 0x34;
        guid->Data4[7] = 0xfb;
        return;
    }

    NSString *uuid_string = [uuid UUIDString];
    if (uuid_string)
        corebth_string_to_guid([uuid_string UTF8String], guid);
    else
        memset(guid, 0, sizeof(*guid));
}

static void corebth_cbuuid_to_bthle(CBUUID *uuid, BTH_LE_UUID *out)
{
    NSData *data = [uuid data];
    const uint8_t *bytes = data.bytes;

    if (data.length == 2)
    {
        out->IsShortUuid = TRUE;
        out->Value.ShortUuid = (bytes[0] << 8) | bytes[1];  /* CoreBluetooth uses big-endian */
        return;
    }

    out->IsShortUuid = FALSE;
    corebth_cbuuid_to_guid(uuid, &out->Value.LongUuid);
}

static struct corebth_peripheral_entry *corebth_find_peripheral_by_name(struct corebth_context *ctx,
                                                                        struct unix_name *name)
{
    struct corebth_peripheral_entry *entry;

    if (!name || !name->str)
        return NULL;

    for (entry = ctx->peripherals; entry; entry = entry->next)
    {
        if (entry->name && entry->name->str && strcmp(entry->name->str, name->str) == 0)
        {
            NSLog(@"Wine: corebth_find_peripheral_by_name: found match for '%s' -> peripheral=%p uuid=%s",
                  name->str, (void *)entry->peripheral, entry->uuid_string ? entry->uuid_string : "(null)");
            return entry;
        }
    }
    NSLog(@"Wine: corebth_find_peripheral_by_name: NO MATCH for '%s'", name->str);
    return NULL;
}

static struct corebth_peripheral_entry *corebth_find_peripheral_by_object(struct corebth_context *ctx,
                                                                          CBPeripheral *peripheral)
{
    struct corebth_peripheral_entry *entry;

    for (entry = ctx->peripherals; entry; entry = entry->next)
    {
        if (entry->peripheral == peripheral)
            return entry;
    }
    return NULL;
}

static struct corebth_peripheral_entry *corebth_find_peripheral_by_handle(struct corebth_context *ctx,
                                                                          uintptr_t handle)
{
    struct corebth_peripheral_entry *entry;

    for (entry = ctx->peripherals; entry; entry = entry->next)
    {
        if (entry->handle == handle)
            return entry;
    }
    return NULL;
}

static struct corebth_service_entry *corebth_find_service_by_cb(struct corebth_context *ctx, CBService *service)
{
    struct corebth_service_entry *entry;

    for (entry = ctx->services; entry; entry = entry->next_global)
    {
        if (entry->service == service)
            return entry;
    }
    return NULL;
}

static struct corebth_char_entry *corebth_find_char_by_cb(struct corebth_context *ctx, CBCharacteristic *characteristic)
{
    struct corebth_char_entry *entry;

    for (entry = ctx->characteristics; entry; entry = entry->next_global)
    {
        if (entry->characteristic == characteristic)
            return entry;
    }
    return NULL;
}

static struct corebth_char_entry *corebth_find_char_by_path(struct corebth_context *ctx, const char *path)
{
    struct corebth_char_entry *entry;

    for (entry = ctx->characteristics; entry; entry = entry->next_global)
    {
        if (entry->path && entry->path->str && !strcmp(entry->path->str, path))
            return entry;
    }
    return NULL;
}

static void corebth_queue_service_added(struct corebth_context *ctx, struct corebth_service_entry *svc)
{
    struct corebth_watcher_event event;
    memset(&event, 0, sizeof(event));
    event.event_type = COREBTH_EVENT_GATT_SERVICE_ADDED;
    event.data.gatt_service_added.device.handle = svc->peripheral->handle;
    /* Duplicate unix_name references so winebth.sys takes ownership */
    event.data.gatt_service_added.service.handle = (uintptr_t)unix_name_dup(svc->path);
    event.data.gatt_service_added.attr_handle = svc->attr_handle;
    event.data.gatt_service_added.is_primary = svc->is_primary;
    event.data.gatt_service_added.uuid = svc->uuid;
    corebth_queue_event(ctx, &event);
}

static void corebth_queue_char_added(struct corebth_context *ctx, struct corebth_char_entry *ch)
{
    struct corebth_watcher_event event;
    memset(&event, 0, sizeof(event));
    event.event_type = COREBTH_EVENT_GATT_CHAR_ADDED;
    /* Duplicate unix_name references so winebth.sys takes ownership */
    event.data.gatt_char_added.characteristic.handle = (uintptr_t)unix_name_dup(ch->path);
    event.data.gatt_char_added.service.handle = (uintptr_t)unix_name_dup(ch->service->path);
    event.data.gatt_char_added.props = ch->props;
    corebth_queue_event(ctx, &event);
}

static void corebth_free_char(struct corebth_char_entry *ch)
{
    if (ch->path) unix_name_free(ch->path);
    if (ch->pending_read) dispatch_release(ch->pending_read);
    if (ch->pending_write) dispatch_release(ch->pending_write);
    if (ch->notification_semaphore) dispatch_release(ch->notification_semaphore);
    pthread_mutex_destroy(&ch->notification_mutex);

    struct corebth_notification_entry *n = ch->notification_queue_head;
    while (n) {
        struct corebth_notification_entry *next = n->next;
        free(n->data);
        free(n);
        n = next;
    }
    free(ch->cached_value);
    if (ch->characteristic) CFBridgingRelease(ch->characteristic);
    free(ch);
}

static void corebth_char_retain(struct corebth_char_entry *ch)
{
    __sync_fetch_and_add(&ch->ref_count, 1);
}

static void corebth_char_release(struct corebth_char_entry *ch)
{
    if (__sync_sub_and_fetch(&ch->ref_count, 1) == 0) {
        corebth_free_char(ch);
    }
}

static struct corebth_service_entry *corebth_create_service_entry(struct corebth_peripheral_entry *periph,
                                                                   CBService *service,
                                                                   uint16_t attr_handle)
{
    struct corebth_service_entry *entry;
    char path[256];

    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return NULL;

    entry->peripheral = periph;
    entry->service = (CBService *)CFBridgingRetain(service);
    entry->attr_handle = attr_handle;
    entry->is_primary = service.isPrimary;
    corebth_cbuuid_to_guid(service.UUID, &entry->uuid);
    entry->next_char_handle = 1;

    if (!periph->uuid_string[0]) {
        free(entry);
        return NULL;
    }

    snprintf(path, sizeof(path), "/org/wine/corebth/device/%s/service/%u", periph->uuid_string, attr_handle);
    entry->path = unix_name_get_or_create(path);

    if (!entry->path)
    {
        free(entry);
        return NULL;
    }

    return entry;
}

static void corebth_link_service_entry(struct corebth_context *ctx,
                                       struct corebth_peripheral_entry *periph,
                                       struct corebth_service_entry *entry)
{
    entry->next = periph->services;
    periph->services = entry;
    entry->next_global = ctx->services;
    ctx->services = entry;
}

static void corebth_queue_service_removed(struct corebth_context *ctx, struct corebth_service_entry *svc)
{
    struct corebth_watcher_event event;
    memset(&event, 0, sizeof(event));
    event.event_type = COREBTH_EVENT_GATT_SERVICE_REMOVED;
    event.data.gatt_service_removed.service.handle = (uintptr_t)svc->path;
    corebth_queue_event(ctx, &event);
}

static void corebth_clear_peripheral_services(struct corebth_context *ctx, struct corebth_peripheral_entry *periph)
{
    struct corebth_service_entry *svc, *next_svc;
    struct corebth_service_entry **prev_global;

    for (svc = periph->services; svc; svc = next_svc)
    {
        next_svc = svc->next;

        corebth_queue_service_removed(ctx, svc);

        prev_global = &ctx->services;
        while (*prev_global && *prev_global != svc)
            prev_global = &(*prev_global)->next_global;
        if (*prev_global)
            *prev_global = svc->next_global;

        struct corebth_char_entry *ch, *next_ch;
        for (ch = svc->chars; ch; ch = next_ch)
        {
            next_ch = ch->next;

            struct corebth_char_entry **prev_char = &ctx->characteristics;
            while (*prev_char && *prev_char != ch)
                prev_char = &(*prev_char)->next_global;
            if (*prev_char)
                *prev_char = ch->next_global;

            corebth_char_release(ch);
        }

        if (svc->path) unix_name_free(svc->path);
        if (svc->service) CFBridgingRelease(svc->service);
        free(svc);
    }
    periph->services = NULL;
}

static void corebth_invalidate_peripheral_chars(struct corebth_peripheral_entry *periph)
{
    struct corebth_service_entry *svc;
    struct corebth_char_entry *ch;

    /* Called from disconnect callback on bt_queue */
    for (svc = periph->services; svc; svc = svc->next)
    {
        for (ch = svc->chars; ch; ch = ch->next)
        {
            char_set_invalidated(ch);
            char_set_read_status(ch, COREBTH_DEVICE_DISCONNECTED);
            char_set_write_status(ch, COREBTH_DEVICE_DISCONNECTED);
            ch->read_awaiting_response = NO;  /* Clear flag to prevent stale state */
            dispatch_semaphore_signal(ch->pending_read);
            dispatch_semaphore_signal(ch->pending_write);
        }
    }
}

static void corebth_fill_props_from_cbcharacteristic(CBCharacteristic *characteristic,
                                                     BTH_LE_GATT_CHARACTERISTIC *props,
                                                     uint16_t service_handle,
                                                     uint16_t char_handle)
{
    CBCharacteristicProperties cprops = characteristic.properties;

    memset(props, 0, sizeof(*props));
    props->ServiceHandle = service_handle;
    props->AttributeHandle = char_handle;
    props->CharacteristicValueHandle = char_handle;
    corebth_cbuuid_to_bthle(characteristic.UUID, &props->CharacteristicUuid);
    props->IsBroadcastable = (cprops & CBCharacteristicPropertyBroadcast) != 0;
    props->IsReadable = (cprops & CBCharacteristicPropertyRead) != 0;
    props->IsWritable = (cprops & CBCharacteristicPropertyWrite) != 0;
    props->IsWritableWithoutResponse = (cprops & CBCharacteristicPropertyWriteWithoutResponse) != 0;
    props->IsSignedWritable = (cprops & CBCharacteristicPropertyAuthenticatedSignedWrites) != 0;
    props->IsNotifiable = (cprops & CBCharacteristicPropertyNotify) != 0;
    props->IsIndicatable = (cprops & CBCharacteristicPropertyIndicate) != 0;
    props->HasExtendedProperties = (cprops & CBCharacteristicPropertyExtendedProperties) != 0;
}

static struct corebth_char_entry *corebth_add_char(struct corebth_context *ctx,
                                                   struct corebth_service_entry *svc,
                                                   CBCharacteristic *characteristic)
{
    struct corebth_char_entry *entry;
    char path[256];

    entry = calloc(1, sizeof(*entry));
    if (!entry) return NULL;

    entry->service = svc;
    entry->characteristic = (CBCharacteristic *)CFBridgingRetain(characteristic);
    entry->pending_read = dispatch_semaphore_create(0);
    entry->pending_write = dispatch_semaphore_create(0);
    char_set_read_status(entry, COREBTH_IDLE);
    char_set_write_status(entry, COREBTH_IDLE);
    atomic_store_explicit(&entry->invalidated, NO, memory_order_release);
    entry->notifications_enabled = FALSE;
    entry->read_awaiting_response = NO;
    pthread_mutex_init(&entry->notification_mutex, NULL);
    entry->notification_semaphore = dispatch_semaphore_create(0);
    entry->notification_queue_head = NULL;
    entry->notification_queue_tail = NULL;
    entry->ref_count = 1;

    uint16_t char_handle = svc->next_char_handle++;
    corebth_fill_props_from_cbcharacteristic(characteristic, &entry->props, svc->attr_handle, char_handle);

    snprintf(path, sizeof(path), "%s/char/%u", svc->path->str, char_handle);
    entry->path = unix_name_get_or_create(path);
    if (!entry->path)
    {
        if (entry->pending_read) dispatch_release(entry->pending_read);
        if (entry->pending_write) dispatch_release(entry->pending_write);
        free(entry);
        return NULL;
    }

    entry->next = svc->chars;
    svc->chars = entry;
    entry->next_global = ctx->characteristics;
    ctx->characteristics = entry;
    return entry;
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

    localName = advertisementData[CBAdvertisementDataLocalNameKey];

    pthread_mutex_lock(&ctx->peripheral_mutex);
    entry = corebth_find_peripheral(ctx, uuid_str);
    if (entry) {
        if (localName && entry->has_local_name == 0) {
            /* Got LocalName for existing device - update name */
            strncpy(entry->friendly_name, [localName UTF8String], BLUETOOTH_MAX_NAME_SIZE - 1);
            entry->has_local_name = 1;

            if (entry->device_added) {
                /* Device already added to driver - send property change */
                struct corebth_watcher_event props_event;
                struct corebth_device_props_changed_event *props_changed = &props_event.data.device_props_changed;

                memset(&props_event, 0, sizeof(props_event));
                props_event.event_type = COREBTH_EVENT_DEVICE_PROPS_CHANGED;
                props_changed->changed_props_mask = WINEBLUETOOTH_DEVICE_PROPERTY_NAME;
                props_changed->invalid_props_mask = 0;
                strncpy(props_changed->props.name, entry->friendly_name, BLUETOOTH_MAX_NAME_SIZE - 1);
    corebth_uuid_to_address(entry->uuid_string, &props_changed->props.address);
    props_changed->device.handle = entry->handle;

    pthread_mutex_unlock(&ctx->peripheral_mutex);
    corebth_queue_event(ctx, &props_event);
            } else {

                memset(&event, 0, sizeof(event));
                event.event_type = COREBTH_EVENT_DEVICE_ADDED;
                device_added->known_props_mask = WINEBLUETOOTH_DEVICE_ALL_PROPERTIES;
                corebth_uuid_to_address(entry->uuid_string, &device_added->props.address);
                strncpy(device_added->props.name, entry->friendly_name, BLUETOOTH_MAX_NAME_SIZE - 1);
                device_added->props.connected = (peripheral.state == CBPeripheralStateConnected) ? 1 : 0;
                device_added->props.paired = 0;
                device_added->props.legacy_pairing = 0;
                device_added->props.trusted = 0;
                device_added->props.device_class = 0;
                /* Duplicate unix_name references so winebth.sys takes ownership */
                device_added->device.handle = (uintptr_t)unix_name_dup(entry->name);
                device_added->radio.handle = ctx->radio_handle;
                device_added->init_entry = 0;

                goto send_device_added;
            }
            return;
        }
        pthread_mutex_unlock(&ctx->peripheral_mutex);
        return;
    }

    BOOL is_cached_name = NO;
    if (localName) {
        name = [localName UTF8String];
    } else if (peripheral.name) {
        const char *pname = [peripheral.name UTF8String];
        if (strncmp(pname, "BlueZ ", 6) == 0) {
            is_cached_name = YES;
            name = "";
        } else {
            name = pname;
        }
    } else {
        name = NULL;
    }

    if (!name || !name[0]) {
        name = "";
    } else if (strcmp(name, "BLE Device") == 0) {
        name = "";
    }

    entry = corebth_add_peripheral(ctx, peripheral, uuid_str);

    if (!entry)
    {
        pthread_mutex_unlock(&ctx->peripheral_mutex);
        return;
    }

    if (localName) {
        entry->has_local_name = 1;
    } else {
        entry->has_local_name = 0;
    }

    WinePeripheralDelegate *pdel = [[WinePeripheralDelegate alloc] init];
    pdel.ctx = ctx;
    pdel.periph = entry;
    entry->peripheral_delegate = (__bridge_retained void *)pdel;
    peripheral.delegate = pdel;

    memset(&event, 0, sizeof(event));
    event.event_type = COREBTH_EVENT_DEVICE_ADDED;

    device_added->known_props_mask = WINEBLUETOOTH_DEVICE_ALL_PROPERTIES;

    corebth_uuid_to_address(uuid_str, &device_added->props.address);

    strncpy(entry->friendly_name, name, BLUETOOTH_MAX_NAME_SIZE - 1);
    strncpy(device_added->props.name, name, BLUETOOTH_MAX_NAME_SIZE - 1);

    device_added->props.connected = (peripheral.state == CBPeripheralStateConnected) ? 1 : 0;
    device_added->props.paired = 0;
    device_added->props.legacy_pairing = 0;
    device_added->props.trusted = 0;
    device_added->props.device_class = 0;

    unix_name_dup(entry->name);
    device_added->device.handle = entry->handle;
    device_added->radio.handle = ctx->radio_handle;
    device_added->init_entry = 0;

    if (is_cached_name) {
        pthread_mutex_unlock(&ctx->peripheral_mutex);
        return;
    }

send_device_added:
    entry->device_added = 1;

    pthread_mutex_unlock(&ctx->peripheral_mutex);

    corebth_queue_event(ctx, &event);

    /* DO NOT connect automatically here - we hit connection limits.
     * Connection should be triggered on-demand when GATT services are requested
     * via corebth_device_start_pairing(). */
}

@interface WineBluetoothDelegate : NSObject <CBCentralManagerDelegate>
@property (nonatomic, assign) struct corebth_context *ctx;
@end

@implementation WineBluetoothDelegate

- (void)centralManagerDidUpdateState:(CBCentralManager *)central
{
    CBManagerState oldState;

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
    /* Comment out noisy log - fires for every BLE device in range repeatedly */
    /* NSLog(@"Wine: discovered peripheral: %@ name: %@", [peripheral.identifier UUIDString], peripheral.name); */

    if (self.ctx && self.ctx->discovering) {
        corebth_queue_device_added(self.ctx, peripheral, advertisementData, RSSI);
    }
}

- (void)centralManager:(CBCentralManager *)central didConnectPeripheral:(CBPeripheral *)peripheral
{
    struct corebth_peripheral_entry *entry;
    struct corebth_watcher_event props_event;
    struct corebth_device_props_changed_event *props_changed = &props_event.data.device_props_changed;

    NSLog(@"Wine: didConnectPeripheral CALLED for %@ name=%@", [peripheral.identifier UUIDString], peripheral.name);

    if (self.ctx) {
        pthread_mutex_lock(&self.ctx->peripheral_mutex);
        entry = corebth_find_peripheral_by_object(self.ctx, peripheral);
        if (entry) {
            memset(&props_event, 0, sizeof(props_event));
            props_event.event_type = COREBTH_EVENT_DEVICE_PROPS_CHANGED;
            props_changed->changed_props_mask = WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED;
            props_changed->invalid_props_mask = 0;
            props_changed->props.connected = 1;
            corebth_uuid_to_address(entry->uuid_string, &props_changed->props.address);
            props_changed->device.handle = entry->handle;
            pthread_mutex_unlock(&self.ctx->peripheral_mutex);

            NSLog(@"Wine: didConnectPeripheral QUEUING DEVICE_PROPS_CHANGED event connected=1 address=%llx",
                  (unsigned long long)props_changed->props.address.ullLong);
            corebth_queue_event(self.ctx, &props_event);
        } else {
            pthread_mutex_unlock(&self.ctx->peripheral_mutex);
        }
    }

    [peripheral discoverServices:nil];
}

- (void)centralManager:(CBCentralManager *)central didDisconnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    struct corebth_peripheral_entry *entry;
    struct corebth_watcher_event props_event;
    struct corebth_device_props_changed_event *props_changed = &props_event.data.device_props_changed;

    NSLog(@"Wine: didDisconnectPeripheral CALLED peripheral=%@ name=%@ ERROR=%@",
          [peripheral.identifier UUIDString], peripheral.name, error);

    if (self.ctx) {
        pthread_mutex_lock(&self.ctx->peripheral_mutex);
        entry = corebth_find_peripheral_by_object(self.ctx, peripheral);
        if (entry) {
            BOOL was_discovering = (entry->pending_char_discovery_count > 0);
            entry->char_discovery_started = 0;
            entry->services_discovery_complete = 0;
            entry->pending_char_discovery_count = 0;
            entry->next_service_handle = 1;

            if (was_discovering) {
                NSLog(@"Wine: didDisconnectPeripheral: signaling semaphore (discovery was in progress)");
                dispatch_semaphore_signal(entry->services_discovered);
            }

            pthread_mutex_lock(&self.ctx->service_list_mutex);
            corebth_invalidate_peripheral_chars(entry);
            corebth_clear_peripheral_services(self.ctx, entry);
            pthread_mutex_unlock(&self.ctx->service_list_mutex);

            memset(&props_event, 0, sizeof(props_event));
            props_event.event_type = COREBTH_EVENT_DEVICE_PROPS_CHANGED;
            props_changed->changed_props_mask = WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED;
            props_changed->invalid_props_mask = 0;
            props_changed->props.connected = 0;
            corebth_uuid_to_address(entry->uuid_string, &props_changed->props.address);
            props_changed->device.handle = entry->handle;
            pthread_mutex_unlock(&self.ctx->peripheral_mutex);

            corebth_queue_event(self.ctx, &props_event);
        } else {
            pthread_mutex_unlock(&self.ctx->peripheral_mutex);
        }
    }
}

- (void)centralManager:(CBCentralManager *)central didFailToConnectPeripheral:(CBPeripheral *)peripheral error:(NSError *)error
{
    struct corebth_peripheral_entry *entry;

    NSLog(@"Wine: failed to connect to peripheral: %@, error: %@",
          [peripheral.identifier UUIDString],
          error ? [error localizedDescription] : @"(null)");

    if (self.ctx) {
        pthread_mutex_lock(&self.ctx->peripheral_mutex);
        entry = corebth_find_peripheral_by_object(self.ctx, peripheral);
        if (entry) {
            NSLog(@"Wine: didFailToConnectPeripheral: signaling semaphore to unblock any waiters");
            dispatch_semaphore_signal(entry->services_discovered);
        }
        pthread_mutex_unlock(&self.ctx->peripheral_mutex);
    }
}

@end

@implementation WinePeripheralDelegate

- (void)dealloc
{
    NSLog(@"Wine: WinePeripheralDelegate DEALLOC ctx=%p periph=%p", self.ctx, self.periph);
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverServices:(NSError *)error
{
    NSLog(@"Wine: didDiscoverServices CALLED peripheral=%@ error=%@ services=%lu",
          [peripheral.identifier UUIDString], error, (unsigned long)[peripheral.services count]);

    if (!self.ctx || !self.periph)
    {
        NSLog(@"Wine: didDiscoverServices: ctx or periph is NULL! ctx=%p periph=%p", self.ctx, self.periph);
        return;
    }

    NSUInteger count = [peripheral.services count];
    struct corebth_service_entry **new_entries = NULL;
    NSUInteger new_count = 0;

    if (count > 0) {
        new_entries = calloc(count, sizeof(*new_entries));
        if (!new_entries) goto done;

        uint16_t next_handle = self.periph->next_service_handle;
        for (CBService *service in peripheral.services)
        {
            struct corebth_service_entry *entry = corebth_create_service_entry(self.periph, service, next_handle++);
            if (entry)
                new_entries[new_count++] = entry;
        }
        self.periph->next_service_handle = next_handle;
    }

    pthread_mutex_lock(&self.ctx->service_list_mutex);

    for (NSUInteger i = 0; i < new_count; i++)
    {
        struct corebth_service_entry *entry = new_entries[i];
        if (!corebth_find_service_by_cb(self.ctx, entry->service))
        {
            corebth_link_service_entry(self.ctx, self.periph, entry);
            corebth_queue_service_added(self.ctx, entry);
        }
        else
        {
            if (entry->path) unix_name_free(entry->path);
            free(entry);
        }
    }

    pthread_mutex_unlock(&self.ctx->service_list_mutex);

    if (!self.periph->char_discovery_started) {
        self.periph->char_discovery_started = 1;
        NSUInteger service_count = [peripheral.services count];
        self.periph->pending_char_discovery_count = (int)service_count;

        NSLog(@"Wine: === STARTING CHARACTERISTIC DISCOVERY for %lu services ===", (unsigned long)service_count);
        NSLog(@"Wine: peripheral=%p delegate=%@ state=%ld", peripheral, peripheral.delegate, (long)peripheral.state);
        if (service_count == 0) {
            self.periph->services_discovery_complete = 1;
            dispatch_semaphore_signal(self.periph->services_discovered);
        } else {
            for (CBService *service in peripheral.services) {
                NSLog(@"Wine: calling discoverCharacteristics for service=%@ peripheral=%p state=%ld",
                      service.UUID, peripheral, (long)peripheral.state);
                @try {
                    [peripheral discoverCharacteristics:nil forService:service];
                    NSLog(@"Wine: discoverCharacteristics returned for service=%@", service.UUID);
                }
                @catch (NSException *exception) {
                    NSLog(@"Wine: EXCEPTION in discoverCharacteristics: %@", exception);
                }
            }
        }
    }

done:
    if (new_entries) free(new_entries);
}

- (void)peripheral:(CBPeripheral *)peripheral didDiscoverCharacteristicsForService:(CBService *)service error:(NSError *)error
{
    NSLog(@"Wine: didDiscoverCharacteristicsForService: service=%@ chars=%lu error=%@",
          service.UUID, (unsigned long)[service.characteristics count], error);

    if (!self.ctx || !self.periph)
        return;

    pthread_mutex_lock(&self.ctx->service_list_mutex);
    struct corebth_service_entry *svc = corebth_find_service_by_cb(self.ctx, service);
    if (svc)
    {
        for (CBCharacteristic *characteristic in service.characteristics)
        {
            struct corebth_char_entry *ch = corebth_find_char_by_cb(self.ctx, characteristic);
            if (!ch)
            {
                ch = corebth_add_char(self.ctx, svc, characteristic);
                if (ch)
                    corebth_queue_char_added(self.ctx, ch);
            }
        }
    }
    else
    {
        NSLog(@"Wine: didDiscoverCharacteristicsForService: service %@ not found in our list (ignoring chars)", service.UUID);
    }
    pthread_mutex_unlock(&self.ctx->service_list_mutex);

    if (self.periph) {
        int remaining = __sync_sub_and_fetch(&self.periph->pending_char_discovery_count, 1);
        if (remaining == 0) {
            self.periph->services_discovery_complete = 1;
            NSLog(@"Wine: All characteristics discovered - signaling semaphore");
            dispatch_semaphore_signal(self.periph->services_discovered);
        }
    }
}

- (void)peripheral:(CBPeripheral *)peripheral didUpdateValueForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    NSLog(@"Wine: didUpdateValueForCharacteristic CALLED! char=%@ error=%@", characteristic.UUID, error);
    if (!self.ctx) {
        NSLog(@"Wine: didUpdateValueForCharacteristic: self.ctx is NULL, returning");
        return;
    }

    pthread_mutex_lock(&self.ctx->service_list_mutex);
    struct corebth_char_entry *ch = corebth_find_char_by_cb(self.ctx, characteristic);
    if (!ch) {
        pthread_mutex_unlock(&self.ctx->service_list_mutex);
        NSLog(@"Wine: didUpdateValueForCharacteristic: char NOT FOUND");
        return;
    }
    corebth_char_retain(ch);

    if (char_is_invalidated(ch)) {
        pthread_mutex_unlock(&self.ctx->service_list_mutex);
        NSLog(@"Wine: didUpdateValueForCharacteristic: char invalidated, ignoring");
        corebth_char_release(ch);
        return;
    }

    uint32_t current_gen = ch->read_generation;
    BOOL is_read_response = ch->read_awaiting_response;
    NSLog(@"Wine: didUpdateValueForCharacteristic: found char, read_awaiting=%d gen=%u notifications_enabled=%d",
          is_read_response, current_gen, ch->notifications_enabled);

    /* CRITICAL: Check read_awaiting_response first. This flag is set ON bt_queue
     * right before readValueForCharacteristic is called. Since bt_queue is serial,
     * if this flag is YES, this callback MUST be the response to that read request,
     * not a notification. This avoids the race condition where a notification
     * arriving during a pending read was incorrectly processed as the read response. */
    if (is_read_response)
    {
        /* Clear the flag immediately - we're handling the read response now */
        ch->read_awaiting_response = NO;

        NSData *data = characteristic.value;
        if (error || !data) {
            NSLog(@"Wine: didUpdateValueForCharacteristic: READ RESPONSE - error or no data, setting INTERNAL_ERROR");
            char_set_read_status(ch, COREBTH_INTERNAL_ERROR);
            ch->pending_read_actual_len = 0;
        } else {
            uint32_t to_copy = MIN(COREBTH_MAX_CHAR_VALUE_SIZE, (uint32_t)data.length);
            NSLog(@"Wine: didUpdateValueForCharacteristic: READ RESPONSE - copying %u bytes to internal buffer", to_copy);
            if (to_copy > 0 && data.bytes) {
                memcpy(ch->pending_read_internal_buffer, data.bytes, to_copy);
            }
            ch->pending_read_actual_len = to_copy;
            char_set_read_status(ch, COREBTH_SUCCESS);
        }
        ch->read_completed_gen = current_gen;
        dispatch_semaphore_signal(ch->pending_read);
    }
    else if (ch->notifications_enabled && !error)
    {
        NSLog(@"Wine: didUpdateValueForCharacteristic: NOTIFICATION PATH - data.length=%lu", (unsigned long)characteristic.value.length);
        NSData *data = characteristic.value;
        struct corebth_notification_entry *notif_entry = calloc(1, sizeof(*notif_entry));
        if (notif_entry && data.length > 0) {
            notif_entry->data_len = (unsigned int)data.length;
            notif_entry->data = malloc(notif_entry->data_len);
            if (notif_entry->data) {
                memcpy(notif_entry->data, data.bytes, notif_entry->data_len);
                notif_entry->next = NULL;

                NSLog(@"Wine: didUpdateValueForCharacteristic: acquiring notification_mutex...");
                pthread_mutex_lock(&ch->notification_mutex);
                NSLog(@"Wine: didUpdateValueForCharacteristic: notification_mutex acquired");
                if (ch->notification_queue_tail) {
                    ch->notification_queue_tail->next = notif_entry;
                } else {
                    ch->notification_queue_head = notif_entry;
                }
                ch->notification_queue_tail = notif_entry;
                pthread_mutex_unlock(&ch->notification_mutex);
                NSLog(@"Wine: didUpdateValueForCharacteristic: notification_mutex released, signaling semaphore");

                dispatch_semaphore_signal(ch->notification_semaphore);
                NSLog(@"Wine: didUpdateValueForCharacteristic: semaphore signaled");
            } else {
                free(notif_entry);
            }
        }
        NSLog(@"Wine: didUpdateValueForCharacteristic: NOTIFICATION PATH done");
    }
    else if (!error)
    {
        NSData *data = characteristic.value;
        if (data && data.length > 0) {
            NSLog(@"Wine: didUpdateValueForCharacteristic: CACHING value len=%lu for char=%@",
                  (unsigned long)data.length, characteristic.UUID);
            free(ch->cached_value);
            ch->cached_value = malloc((unsigned int)data.length);
            if (ch->cached_value) {
                memcpy(ch->cached_value, data.bytes, data.length);
                ch->cached_value_len = (unsigned int)data.length;
                ch->cached_value_valid = TRUE;
            }
        }
    }

    NSLog(@"Wine: didUpdateValueForCharacteristic: releasing service_list_mutex");
    pthread_mutex_unlock(&self.ctx->service_list_mutex);
    NSLog(@"Wine: didUpdateValueForCharacteristic: DONE");
    corebth_char_release(ch);
}

- (void)peripheral:(CBPeripheral *)peripheral didWriteValueForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    if (!self.ctx)
        return;

    pthread_mutex_lock(&self.ctx->service_list_mutex);
    struct corebth_char_entry *ch = corebth_find_char_by_cb(self.ctx, characteristic);
    if (!ch) {
        pthread_mutex_unlock(&self.ctx->service_list_mutex);
        return;
    }
    corebth_char_retain(ch);

    if (char_is_invalidated(ch)) {
        pthread_mutex_unlock(&self.ctx->service_list_mutex);
        corebth_char_release(ch);
        return;
    }

    if (char_get_write_status(ch) == COREBTH_PENDING)
    {
        if (error) {
            long error_code = error.code;
            if (error_code == 13) {
                char_set_write_status(ch, COREBTH_INVALID_PARAMETER);
            } else {
                char_set_write_status(ch, COREBTH_INTERNAL_ERROR);
            }
        } else {
            char_set_write_status(ch, COREBTH_SUCCESS);
        }
        ch->write_completed_gen = ch->write_generation;
        dispatch_semaphore_signal(ch->pending_write);
    }

    pthread_mutex_unlock(&self.ctx->service_list_mutex);
    corebth_char_release(ch);
}

- (void)peripheral:(CBPeripheral *)peripheral didUpdateNotificationStateForCharacteristic:(CBCharacteristic *)characteristic error:(NSError *)error
{
    if (!self.ctx) return;

    pthread_mutex_lock(&self.ctx->service_list_mutex);
    struct corebth_char_entry *ch = corebth_find_char_by_cb(self.ctx, characteristic);
    if (!ch) {
        pthread_mutex_unlock(&self.ctx->service_list_mutex);
        return;
    }
    corebth_char_retain(ch);

    if (char_is_invalidated(ch)) {
        pthread_mutex_unlock(&self.ctx->service_list_mutex);
        corebth_char_release(ch);
        return;
    }

    if (char_get_write_status(ch) == COREBTH_PENDING)
    {
        if (error) {
            long error_code = error.code;
            if (error_code == 13) {
                char_set_write_status(ch, COREBTH_INVALID_PARAMETER);
            } else {
                char_set_write_status(ch, COREBTH_INTERNAL_ERROR);
            }
        } else {
            char_set_write_status(ch, COREBTH_SUCCESS);
        }
        ch->write_completed_gen = ch->write_generation;
        dispatch_semaphore_signal(ch->pending_write);
    }

    pthread_mutex_unlock(&self.ctx->service_list_mutex);
    corebth_char_release(ch);
}

@end

void *corebth_init( void )
{
    struct corebth_context *ctx;
    WineBluetoothDelegate *delegate;

    ctx = calloc( 1, sizeof( *ctx ) );
    if (!ctx) return NULL;

    pthread_mutex_init( &ctx->event_mutex, NULL );
    pthread_mutex_init( &ctx->peripheral_mutex, NULL );
    pthread_mutex_init( &ctx->service_list_mutex, NULL );
    ctx->event_head = NULL;
    ctx->event_tail = NULL;
    ctx->peripherals = NULL;
    ctx->services = NULL;
    ctx->characteristics = NULL;
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
    ctx->central_delegate = (id)CFBridgingRetain(delegate);  /* Retain the delegate */

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

    return ctx;
}

void corebth_close( void *connection )
{
    struct corebth_context *ctx = connection;

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
        if (peripheral->peripheral)
            CFRelease((__bridge CFTypeRef)peripheral->peripheral);
        if (peripheral->peripheral_delegate)
            CFRelease(peripheral->peripheral_delegate);
        if (peripheral->services_discovered)
            dispatch_release(peripheral->services_discovered);
        if (peripheral->name) unix_name_free(peripheral->name);
        free(peripheral);
        peripheral = next_peripheral;
    }
    for (unsigned i = 0; i < COREBTH_PERIPH_BUCKETS; ++i) {
        struct corebth_peripheral_map_entry *e = ctx->uuid_map.buckets[i];
        while (e) {
            struct corebth_peripheral_map_entry *next = e->next;
            free(e);
            e = next;
        }
        ctx->uuid_map.buckets[i] = NULL;
    }
    ctx->peripherals = NULL;
    pthread_mutex_unlock(&ctx->peripheral_mutex);

    pthread_mutex_lock(&ctx->service_list_mutex);
    struct corebth_char_entry *ch = ctx->characteristics;
    while (ch) {
        struct corebth_char_entry *next = ch->next_global;
        corebth_char_release(ch);
        ch = next;
    }
    struct corebth_service_entry *svc = ctx->services;
    while (svc) {
        struct corebth_service_entry *next = svc->next_global;
        if (svc->path) unix_name_free(svc->path);
        if (svc->service) CFBridgingRelease(svc->service);
        free(svc);
        svc = next;
    }
    ctx->services = NULL;
    ctx->characteristics = NULL;
    pthread_mutex_unlock(&ctx->service_list_mutex);

    if (ctx->bt_queue)
        dispatch_release( ctx->bt_queue );

    if (ctx->central_delegate)
        CFRelease((__bridge CFTypeRef)ctx->central_delegate);

    pthread_mutex_destroy( &ctx->event_mutex );
    pthread_mutex_destroy( &ctx->peripheral_mutex );
    pthread_mutex_destroy( &ctx->service_list_mutex );

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

    if (!ctx || !ctx->central_manager)
        return COREBTH_NOT_SUPPORTED;

    if (ctx->state != CBManagerStatePoweredOn)
        return COREBTH_DEVICE_NOT_READY;

    ctx->discovering = 1;
    @autoreleasepool {
        [ctx->central_manager scanForPeripheralsWithServices:nil
                                                     options:@{CBCentralManagerScanOptionAllowDuplicatesKey: @YES}];
    }

    return COREBTH_SUCCESS;
}

corebth_status corebth_adapter_stop_discovery( void *connection, const char *adapter_path )
{
    struct corebth_context *ctx = connection;

    if (!ctx || !ctx->central_manager)
        return COREBTH_NOT_SUPPORTED;

    ctx->discovering = 0;

    @autoreleasepool {
        [ctx->central_manager stopScan];
    }

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
    *ctx = NULL;
    return COREBTH_SUCCESS;
}

void corebth_auth_agent_stop( void *connection, void *ctx )
{
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
    struct corebth_context *ctx = connection;
    struct unix_name *device_name = (struct unix_name *)device;
    struct corebth_peripheral_entry *entry;
    dispatch_semaphore_t services_sem;
    CBPeripheral *peripheral_to_connect;
    BOOL already_discovered;
    CBPeripheralState peripheral_state;

    if (!ctx || !device_name || !device_name->str)
        return COREBTH_NOT_SUPPORTED;

    pthread_mutex_lock(&ctx->peripheral_mutex);
    entry = corebth_find_peripheral_by_name(ctx, device_name);

    if (!entry || !entry->peripheral)
    {
        pthread_mutex_unlock(&ctx->peripheral_mutex);
        return COREBTH_NOT_SUPPORTED;
    }

    peripheral_to_connect = entry->peripheral;
    services_sem = entry->services_discovered;
    already_discovered = entry->services_discovery_complete;
    peripheral_state = peripheral_to_connect.state;

    if (already_discovered)
    {
        pthread_mutex_unlock(&ctx->peripheral_mutex);
        return COREBTH_SUCCESS;
    }

    entry->services_discovery_complete = 0;
    entry->char_discovery_started = 0;

    @autoreleasepool {
        NSLog(@"Wine: corebth_device_start_pairing: peripheral=%@ state=%ld already_discovered=%d",
              [peripheral_to_connect.identifier UUIDString], (long)peripheral_state, (int)already_discovered);

        if (peripheral_state == CBPeripheralStateConnected)
        {
            NSLog(@"Wine: corebth_device_start_pairing: already connected, calling discoverServices");
            NSLog(@"Wine: corebth_device_start_pairing: BEFORE discoverServices - peripheral=%@ delegate=%@",
                  peripheral_to_connect, peripheral_to_connect.delegate);
            [peripheral_to_connect discoverServices:nil];
            NSLog(@"Wine: corebth_device_start_pairing: AFTER discoverServices - delegate still=%@",
                  peripheral_to_connect.delegate);
        }
        else
        {
            NSLog(@"Wine: corebth_device_start_pairing: calling connectPeripheral (state=%ld)", (long)peripheral_state);
            [ctx->central_manager connectPeripheral:peripheral_to_connect options:nil];
        }
    }

    pthread_mutex_unlock(&ctx->peripheral_mutex);

    /* Wait for service discovery to complete (max 10 seconds - some services may not respond) */
    NSLog(@"Wine: corebth_device_start_pairing: WAITING for services_discovered semaphore (10s timeout)...");
    dispatch_time_t timeout = dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC);
    long result = dispatch_semaphore_wait(services_sem, timeout);
    if (result == 0) {
        NSLog(@"Wine: corebth_device_start_pairing: services_discovered signaled - SUCCESS");
    } else {
        NSLog(@"Wine: corebth_device_start_pairing: services_discovered TIMEOUT after 10s (proceeding anyway). Pending count: %d", 
              entry->pending_char_discovery_count);
    }

    return COREBTH_SUCCESS;
}

corebth_status corebth_watcher_init( void *connection, void **ctx )
{
    *ctx = connection;
    return COREBTH_SUCCESS;
}

void corebth_watcher_close( void *connection, void *ctx )
{
}

corebth_status corebth_characteristic_read( void *connection, const char *char_path,
                                            unsigned char *buffer, unsigned int buffer_size,
                                            unsigned int *len )
{
    struct corebth_context *ctx = connection;
    struct corebth_char_entry *ch;
    dispatch_time_t timeout;
    dispatch_semaphore_t sem;
    corebth_status status;
    CBPeripheral *peripheral;
    CBCharacteristic *characteristic;

    NSLog(@"Wine: corebth_characteristic_read: path=%s buffer=%p buf_size=%u", char_path, buffer, buffer_size);

    if (!ctx || !buffer || !len) {
        NSLog(@"Wine: corebth_characteristic_read: invalid args ctx=%p buffer=%p len=%p", ctx, buffer, len);
        return COREBTH_NOT_SUPPORTED;
    }

    NSLog(@"Wine: corebth_characteristic_read: acquiring service_list_mutex (thread %p)...", pthread_self());
    pthread_mutex_lock(&ctx->service_list_mutex);
    NSLog(@"Wine: corebth_characteristic_read: service_list_mutex acquired, finding char...");
    ch = corebth_find_char_by_path(ctx, char_path);
    NSLog(@"Wine: corebth_characteristic_read: find_char returned ch=%p", ch);
    if (!ch) {
        NSLog(@"Wine: corebth_characteristic_read: char NOT FOUND for path=%s", char_path);
        pthread_mutex_unlock(&ctx->service_list_mutex);
        return COREBTH_NOT_SUPPORTED;
    }
    NSLog(@"Wine: corebth_characteristic_read: found char path=%s", ch->path ? ch->path->str : "(null)");

    if (!ch->service) {
        NSLog(@"Wine: corebth_characteristic_read: ch->service is NULL");
        pthread_mutex_unlock(&ctx->service_list_mutex);
        return COREBTH_NOT_SUPPORTED;
    }

    if (!ch->service->peripheral) {
        NSLog(@"Wine: corebth_characteristic_read: ch->service->peripheral is NULL");
        pthread_mutex_unlock(&ctx->service_list_mutex);
        return COREBTH_NOT_SUPPORTED;
    }

    if (!ch->service->peripheral->peripheral) {
        NSLog(@"Wine: corebth_characteristic_read: ch->service->peripheral->peripheral is NULL");
        pthread_mutex_unlock(&ctx->service_list_mutex);
        return COREBTH_NOT_SUPPORTED;
    }

    if (!ch->characteristic) {
        NSLog(@"Wine: corebth_characteristic_read: ch->characteristic is NULL");
        pthread_mutex_unlock(&ctx->service_list_mutex);
        return COREBTH_NOT_SUPPORTED;
    }

    if (!ch->pending_read) {
        NSLog(@"Wine: corebth_characteristic_read: ch->pending_read is NULL");
        pthread_mutex_unlock(&ctx->service_list_mutex);
        return COREBTH_INTERNAL_ERROR;
    }

    peripheral = ch->service->peripheral->peripheral;
    characteristic = ch->characteristic;

    NSLog(@"Wine: corebth_characteristic_read: peripheral=%@ state=%ld (0=disconnected,1=connecting,2=connected)",
          peripheral.identifier.UUIDString, (long)peripheral.state);

    if (peripheral.state != CBPeripheralStateConnected) {
        NSLog(@"Wine: corebth_characteristic_read: DEVICE NOT CONNECTED, returning DEVICE_NOT_READY");
        pthread_mutex_unlock(&ctx->service_list_mutex);
        return COREBTH_DEVICE_NOT_READY;
    }

    if (ch->cached_value_valid && ch->cached_value && ch->cached_value_len > 0) {
        unsigned int to_copy = MIN(buffer_size, ch->cached_value_len);
        NSLog(@"Wine: corebth_characteristic_read: USING CACHED VALUE len=%u (requested=%u)", ch->cached_value_len, buffer_size);
        memcpy(buffer, ch->cached_value, to_copy);
        *len = to_copy;
        ch->cached_value_valid = FALSE;
        pthread_mutex_unlock(&ctx->service_list_mutex);
        return COREBTH_SUCCESS;
    }

    uint32_t my_gen = ++ch->read_generation;
    ch->read_completed_gen = 0;
    char_set_read_status(ch, COREBTH_PENDING);
    ch->pending_read_actual_len = 0;
    sem = ch->pending_read;
    corebth_char_retain(ch);

    pthread_mutex_unlock(&ctx->service_list_mutex);

    @autoreleasepool {
        NSLog(@"Wine: corebth_characteristic_read: calling readValueForCharacteristic: char=%@ delegate=%@",
              characteristic.UUID, peripheral.delegate);
        NSLog(@"Wine: corebth_characteristic_read: bt_queue=%p (NULL check before dispatch)", ctx->bt_queue);

        /* IMPORTANT: CoreBluetooth requires peripheral methods to be called from the same queue
         * that the CBCentralManager was created with. Dispatch to bt_queue. */
        if (!ctx->bt_queue) {
            NSLog(@"Wine: corebth_characteristic_read: ERROR - bt_queue is NULL!");
            return COREBTH_INTERNAL_ERROR;
        }
        dispatch_sync(ctx->bt_queue, ^{
            NSLog(@"Wine: corebth_characteristic_read: BLOCK STARTED on bt_queue (sync)");
            NSLog(@"Wine: corebth_characteristic_read: peripheral=%@ state=%ld services_count=%lu",
                  peripheral.identifier.UUIDString, (long)peripheral.state, (unsigned long)peripheral.services.count);
            NSLog(@"Wine: corebth_characteristic_read: characteristic=%p uuid=%@ service=%@ properties=0x%lx",
                  characteristic, characteristic.UUID, characteristic.service.UUID, (unsigned long)characteristic.properties);
            @try {
                /* CRITICAL: Set this flag ON bt_queue right before the read call.
                 * Since bt_queue is serial, the callback cannot run until this block completes.
                 * This ensures the callback will see read_awaiting_response=YES and know
                 * this is a read response, not a notification. */
                ch->read_awaiting_response = YES;
                [peripheral readValueForCharacteristic:characteristic];
                NSLog(@"Wine: corebth_characteristic_read: readValueForCharacteristic called successfully");
            }
            @catch (NSException *exception) {
                NSLog(@"Wine: corebth_characteristic_read EXCEPTION in dispatch: %@", exception);
                ch->read_awaiting_response = NO;
                pthread_mutex_lock(&ctx->service_list_mutex);
                char_set_read_status(ch, COREBTH_INTERNAL_ERROR);
                ch->read_completed_gen = my_gen;
                pthread_mutex_unlock(&ctx->service_list_mutex);
                dispatch_semaphore_signal(sem);
            }
        });
        NSLog(@"Wine: corebth_characteristic_read: dispatch_sync returned (waiting for callback)");
    }

    NSLog(@"Wine: corebth_characteristic_read: waiting on semaphore (5s timeout) gen=%u", my_gen);
    timeout = dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC);
    dispatch_semaphore_wait(sem, timeout);

    pthread_mutex_lock(&ctx->service_list_mutex);

    if (ch->read_completed_gen == my_gen) {
        uint32_t result_len = ch->pending_read_actual_len;
        NSLog(@"Wine: corebth_characteristic_read: callback completed gen=%u len=%u", my_gen, result_len);
        if (result_len > 0 && result_len <= buffer_size) {
            memcpy(buffer, ch->pending_read_internal_buffer, result_len);
            *len = result_len;
        } else if (result_len > buffer_size) {
            /* Data larger than buffer - copy what fits and report actual size */
            memcpy(buffer, ch->pending_read_internal_buffer, buffer_size);
            *len = buffer_size;
            NSLog(@"Wine: corebth_characteristic_read: WARNING - data truncated from %u to %u bytes",
                  result_len, buffer_size);
        } else {
            *len = 0;
        }
        status = char_get_read_status(ch);
    } else {
        NSLog(@"Wine: corebth_characteristic_read: TIMEOUT or stale gen=%u completed_gen=%u",
              my_gen, ch->read_completed_gen);

        /* Clear the read_awaiting_response flag on bt_queue since the callback
         * never processed our read request. We dispatch_async because we're not
         * on bt_queue here, and the flag must only be accessed from bt_queue. */
        corebth_char_retain(ch);  /* prevent ch from being freed before block runs */
        dispatch_async(ctx->bt_queue, ^{
            ch->read_awaiting_response = NO;
            corebth_char_release(ch);
        });

        NSData *cachedData = characteristic.value;
        if (cachedData && cachedData.length > 0) {
            unsigned int to_copy = MIN(buffer_size, (unsigned int)cachedData.length);
            NSLog(@"Wine: corebth_characteristic_read: TIMEOUT FALLBACK - using cached value length=%lu", (unsigned long)cachedData.length);
            memcpy(buffer, cachedData.bytes, to_copy);
            *len = to_copy;
            status = COREBTH_SUCCESS;
        } else {
            *len = 0;
            status = COREBTH_TIMEOUT;
        }
    }

    pthread_mutex_unlock(&ctx->service_list_mutex);
    corebth_char_release(ch);
    return status;
}

corebth_status corebth_characteristic_write( void *connection, const char *char_path,
                                             const unsigned char *value, unsigned int len,
                                             int write_type )
{
    struct corebth_context *ctx = connection;
    struct corebth_char_entry *ch;
    dispatch_time_t timeout;
    dispatch_semaphore_t sem;
    CBCharacteristicWriteType cb_type = (write_type == 0) ? CBCharacteristicWriteWithResponse
                                                          : CBCharacteristicWriteWithoutResponse;
    CBPeripheral *peripheral;
    CBCharacteristic *characteristic;
    NSUInteger max_write_len;
    long wait_result;
    corebth_status status;
    BOOL write_without_response = NO;

    if (!ctx || !value) {
        return COREBTH_NOT_SUPPORTED;
    }

    pthread_mutex_lock(&ctx->service_list_mutex);
    ch = corebth_find_char_by_path(ctx, char_path);
    if (!ch || !ch->service || !ch->service->peripheral || !ch->service->peripheral->peripheral) {
        pthread_mutex_unlock(&ctx->service_list_mutex);
        return COREBTH_NOT_SUPPORTED;
    }

    peripheral = ch->service->peripheral->peripheral;
    characteristic = ch->characteristic;

    uint32_t my_gen = ++ch->write_generation;
    ch->write_completed_gen = 0;
    char_set_write_status(ch, COREBTH_PENDING);
    sem = ch->pending_write;
    corebth_char_retain(ch);
    pthread_mutex_unlock(&ctx->service_list_mutex);

    @autoreleasepool {
        NSData *data = [NSData dataWithBytes:value length:len];

        if (!peripheral.delegate) {
            char_set_write_status(ch, COREBTH_INTERNAL_ERROR);
            dispatch_semaphore_signal(sem);
            corebth_char_release(ch);
            return COREBTH_INTERNAL_ERROR;
        }
        if (peripheral.state != CBPeripheralStateConnected) {
            char_set_write_status(ch, COREBTH_DEVICE_NOT_READY);
            dispatch_semaphore_signal(sem);
            corebth_char_release(ch);
            return COREBTH_DEVICE_NOT_READY;
        }
        max_write_len = [peripheral maximumWriteValueLengthForType:cb_type];

        if (len > max_write_len) {
            char_set_write_status(ch, COREBTH_INVALID_PARAMETER);
            dispatch_semaphore_signal(sem);
            corebth_char_release(ch);
            return COREBTH_INVALID_PARAMETER;
        }
        if ((characteristic.properties & CBCharacteristicPropertyWrite) == 0 &&
            (characteristic.properties & CBCharacteristicPropertyWriteWithoutResponse) == 0) {
            char_set_write_status(ch, COREBTH_INVALID_PARAMETER);
            dispatch_semaphore_signal(sem);
            corebth_char_release(ch);
            return COREBTH_INVALID_PARAMETER;
        }

        /* IMPORTANT: CoreBluetooth requires peripheral methods to be called from the same queue
         * that the CBCentralManager was created with. Dispatch to bt_queue. */
        CBCharacteristic *char_to_write = characteristic;
        dispatch_async(ctx->bt_queue, ^{
            @try {
                [peripheral writeValue:data
                      forCharacteristic:char_to_write
                                   type:cb_type];
                NSLog(@"Wine: corebth_characteristic_write: writeValue dispatched successfully");
            }
            @catch (NSException *exception) {
                NSLog(@"Wine: corebth_characteristic_write EXCEPTION in dispatch: %@", exception);
                pthread_mutex_lock(&ctx->service_list_mutex);
                char_set_write_status(ch, COREBTH_INTERNAL_ERROR);
                ch->write_completed_gen = my_gen;
                pthread_mutex_unlock(&ctx->service_list_mutex);
                dispatch_semaphore_signal(sem);
            }
        });

        if (cb_type == CBCharacteristicWriteWithoutResponse) {
            write_without_response = YES;
        }
    }

    if (write_without_response) {
        corebth_char_release(ch);
        return COREBTH_SUCCESS;
    }

    timeout = dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC);
    wait_result = dispatch_semaphore_wait(sem, timeout);

    pthread_mutex_lock(&ctx->service_list_mutex);
    if (ch->write_completed_gen == my_gen) {
        status = char_get_write_status(ch);
    } else {
        status = COREBTH_TIMEOUT;
    }
    pthread_mutex_unlock(&ctx->service_list_mutex);

    corebth_char_release(ch);
    return status;
}

corebth_status corebth_characteristic_set_notify( void *connection, const char *char_path,
                                                  int enable )
{
    struct corebth_context *ctx = connection;
    struct corebth_char_entry *ch;
    CBPeripheral *peripheral;
    CBCharacteristic *characteristic;

    if (!ctx) return COREBTH_NOT_SUPPORTED;

    pthread_mutex_lock(&ctx->service_list_mutex);
    ch = corebth_find_char_by_path(ctx, char_path);
    if (!ch || !ch->service || !ch->service->peripheral || !ch->service->peripheral->peripheral) {
        pthread_mutex_unlock(&ctx->service_list_mutex);
        return COREBTH_NOT_SUPPORTED;
    }

    peripheral = ch->service->peripheral->peripheral;
    characteristic = ch->characteristic;

    ch->notifications_enabled = enable ? TRUE : FALSE;
    corebth_char_retain(ch);
    pthread_mutex_unlock(&ctx->service_list_mutex);

    /* IMPORTANT: CoreBluetooth requires peripheral methods to be called from the same queue
     * that the CBCentralManager was created with. Dispatch to bt_queue. */
    @autoreleasepool {
        BOOL enable_val = enable ? YES : NO;
        dispatch_async(ctx->bt_queue, ^{
            [peripheral setNotifyValue:enable_val forCharacteristic:characteristic];
            NSLog(@"Wine: corebth_characteristic_set_notify: setNotifyValue dispatched (enable=%d)", enable_val);
        });
    }

    corebth_char_release(ch);

    return COREBTH_SUCCESS;
}

corebth_status corebth_characteristic_read_notification( void *connection, const char *char_path,
                                                         unsigned char *buffer, unsigned int buffer_size,
                                                         unsigned int *size )
{
    struct corebth_context *ctx = connection;
    struct corebth_char_entry *ch;
    struct corebth_notification_entry *notif_entry;
    dispatch_time_t timeout;
    long wait_result;

    if (!ctx) return COREBTH_NOT_SUPPORTED;
    if (!buffer || !size) return COREBTH_INVALID_PARAMETER;

    NSLog(@"Wine: corebth_characteristic_read_notification: acquiring service_list_mutex...");
    pthread_mutex_lock(&ctx->service_list_mutex);
    NSLog(@"Wine: corebth_characteristic_read_notification: mutex acquired");
    ch = corebth_find_char_by_path(ctx, char_path);
    if (!ch) {
        pthread_mutex_unlock(&ctx->service_list_mutex);
        NSLog(@"Wine: corebth_characteristic_read_notification: char not found, returning");
        return COREBTH_NOT_SUPPORTED;
    }
    corebth_char_retain(ch);
    pthread_mutex_unlock(&ctx->service_list_mutex);
    NSLog(@"Wine: corebth_characteristic_read_notification: mutex released, waiting on semaphore (5s)...");

    timeout = dispatch_time(DISPATCH_TIME_NOW, 5LL * NSEC_PER_SEC);
    wait_result = dispatch_semaphore_wait(ch->notification_semaphore, timeout);

    if (wait_result != 0) {
        
        *size = 0;
        corebth_char_release(ch);
        return COREBTH_TIMEOUT;
    }

    pthread_mutex_lock(&ch->notification_mutex);
    notif_entry = ch->notification_queue_head;
    if (notif_entry) {
        ch->notification_queue_head = notif_entry->next;
        if (!ch->notification_queue_head) {
            ch->notification_queue_tail = NULL;
        }
    }
    pthread_mutex_unlock(&ch->notification_mutex);

    if (notif_entry) {
        unsigned int to_copy = MIN(buffer_size, notif_entry->data_len);

        memcpy(buffer, notif_entry->data, to_copy);

        *size = to_copy;

        free(notif_entry->data);

        free(notif_entry);
        corebth_char_release(ch);
        return COREBTH_SUCCESS;
    }

    
    corebth_char_release(ch);
    *size = 0;
    return COREBTH_NOT_SUPPORTED;
}

#endif /* __APPLE__ */
