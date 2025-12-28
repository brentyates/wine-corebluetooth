#ifndef __WINE_COREBTH_PRIV_H
#define __WINE_COREBTH_PRIV_H

#include "winebluetooth.h"

// CoreBluetooth context structure
typedef struct corebth_context {
    CBCentralManager *central_manager;
    CBPeripheralManager *peripheral_manager;
    dispatch_queue_t bt_queue;

    // Event queue (thread-safe)
    pthread_mutex_t event_mutex;
    struct list event_queue;

    // Device tracking
    NSMutableDictionary<NSUUID*, CBPeripheral*> *discovered_peripherals;
    NSMutableDictionary<NSUUID*, struct unix_name*> *peripheral_names;
} corebth_context_t;

// Function declarations
struct corebth_context *corebth_init(void);
void corebth_close(struct corebth_context *ctx);
void corebth_free(struct corebth_context *ctx);
int corebth_loop(struct corebth_context *ctx);
BOOL corebth_adapter_set_prop(struct corebth_context *ctx, enum winebluetooth_radio_property prop, void *value);
BOOL corebth_adapter_start_discovery(struct corebth_context *ctx);
BOOL corebth_adapter_stop_discovery(struct corebth_context *ctx);
BOOL corebth_adapter_remove_device(struct corebth_context *ctx, BLUETOOTH_ADDRESS address);
BOOL corebth_device_disconnect(struct corebth_context *ctx, BLUETOOTH_ADDRESS address);
BOOL corebth_device_start_pairing(struct corebth_context *ctx, BLUETOOTH_ADDRESS address);
BOOL corebth_watcher_init(struct corebth_context *ctx);
BOOL corebth_watcher_close(struct corebth_context *ctx);

// Helper functions
BLUETOOTH_ADDRESS corebth_uuid_to_address(NSUUID *uuid);
void corebth_get_device_properties(CBPeripheral *peripheral, struct winebluetooth_device_properties *props);

#endif // __WINE_COREBTH_PRIV_H
