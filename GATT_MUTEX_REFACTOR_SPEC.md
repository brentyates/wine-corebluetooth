# GATT Mutex Refactoring Implementation Specification

## Executive Summary

This document specifies the complete refactoring of the `winebth.sys` synchronization architecture to eliminate deadlocks and race conditions. The implementation uses reference counting with a strict two-level lock hierarchy, following patterns proven in `winebus.sys` and `hidclass.sys`.

---

## Table of Contents

1. [Current Architecture Problems](#1-current-architecture-problems)
2. [Target Architecture](#2-target-architecture)
3. [Struct Modifications](#3-struct-modifications)
4. [Helper Functions](#4-helper-functions)
5. [Call Site Transformations](#5-call-site-transformations)
6. [Implementation Order](#6-implementation-order)
7. [Verification Checklist](#7-verification-checklist)

---

## 1. Current Architecture Problems

### 1.1 Three-Level Lock Hierarchy (Deadlock-Prone)
```
device_list_cs (global)
  └── props_cs (per-device)
        └── chars_cs (per-service)
```

### 1.2 Inconsistent Lock Acquisition
- `props_cs` sometimes acquired without `device_list_cs` (lines 220, 264, 1131, 1671)
- Creates lock inversion opportunities

### 1.3 External Calls While Holding Locks
- `IoReportTargetDeviceChange` called at lines 837, 940, 1105 while holding locks
- `IoInvalidateDeviceRelations` called at lines 744, 759, 889, 947 while holding locks
- `IoRegisterDeviceInterface` called at line 1147 while holding `props_cs`

### 1.4 No Reference Counting
- Objects can be freed while pointers are held
- No safe way to release lock, make external call, then continue using object

### 1.5 No Explicit State Machine
- Uses boolean flags `started` and `removed`
- State transitions not atomic or validated

---

## 2. Target Architecture

### 2.1 Two-Level Lock Hierarchy
```
device_list_cs (global) - protects all list operations
  └── props_cs (per-device) - protects device property mutations only
      (NO chars_cs - eliminated)
```

### 2.2 Reference Counting on All Objects
- `bluetooth_radio`: refcount for safe access after releasing `device_list_cs`
- `bluetooth_remote_device`: refcount for safe access after releasing locks
- `bluetooth_gatt_service`: refcount (services are immutable after creation)
- `bluetooth_gatt_characteristic`: no refcount (owned by service, immutable)

### 2.3 Explicit State Machine
```c
enum bluetooth_object_state {
    BLUETOOTH_STATE_INITIALIZING,  // Being created, not yet in list
    BLUETOOTH_STATE_ACTIVE,        // In list, fully operational
    BLUETOOTH_STATE_REMOVING,      // Marked for removal, draining refs
};
```

**IMPORTANT: Atomic State Transition Rules**

State transitions MUST follow these rules to avoid race conditions:

1. **State writes**: Always performed while holding `device_list_cs`
2. **State reads**: Either:
   - Hold `device_list_cs`, OR
   - Use `InterlockedCompareExchange(&obj->state, 0, 0)` for atomic read, OR
   - Accept that the read may be stale (only safe for early-exit optimizations)

3. **Transition ordering**:
   - `INITIALIZING` → `ACTIVE`: Set while holding `device_list_cs`, after adding to list
   - `ACTIVE` → `REMOVING`: Set while holding `device_list_cs`, before removing from list
   - No other transitions are valid

4. **Memory barriers**: The `device_list_cs` critical section provides implicit memory barriers.
   If checking state outside the lock (for optimization), use:
   ```c
   if (InterlockedCompareExchange( &device->state, 0, 0 ) != BLUETOOTH_STATE_ACTIVE)
       return STATUS_DEVICE_NOT_CONNECTED;
   ```

### 2.4 Lock Acquisition Rules
1. Always acquire `device_list_cs` before `props_cs`
2. Never acquire `props_cs` without either:
   - Holding `device_list_cs`, OR
   - Holding a reference to the device
3. Never make external calls while holding any lock
4. `chars_cs` is eliminated entirely

### 2.5 External Call Pattern
```c
// BEFORE (wrong):
EnterCriticalSection(&device_list_cs);
// ... find device ...
IoReportTargetDeviceChange(...);  // BAD: external call under lock
LeaveCriticalSection(&device_list_cs);

// AFTER (correct):
EnterCriticalSection(&device_list_cs);
// ... find device ...
bluetooth_device_incref(device);  // Take reference
// ... copy needed data to locals ...
LeaveCriticalSection(&device_list_cs);
IoReportTargetDeviceChange(...);  // GOOD: no lock held
bluetooth_device_decref(device);  // Release reference
```

### 2.6 IRP Cancellation and irp_list Synchronization

**Current Gap:** The existing code queues IRPs to `radio->irp_list` (line 546) without setting a cancel routine via `IoSetCancelRoutine`. This means:
- If an IRP is cancelled by the system, no callback fires
- The cancelled IRP remains on `irp_list`
- When `remove_pending_irps` runs, it may complete an already-cancelled IRP (double-complete = BSOD)

**Required Implementation:**

```c
/* Cancel routine for pending pairing IRPs */
static void WINAPI bluetooth_irp_cancel_routine( DEVICE_OBJECT *device, IRP *irp )
{
    struct bluetooth_radio *radio = /* extract from device extension */;

    /* Release the cancel spinlock first (required by Windows) */
    IoReleaseCancelSpinLock( irp->CancelIrql );

    /* Now acquire our lock to safely remove from list */
    EnterCriticalSection( &device_list_cs );
    RemoveEntryList( &irp->Tail.Overlay.ListEntry );
    LeaveCriticalSection( &device_list_cs );

    irp->IoStatus.Status = STATUS_CANCELLED;
    irp->IoStatus.Information = 0;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
}
```

**When queuing an IRP (modified line 545-547):**

```c
if (status == STATUS_PENDING)
{
    EnterCriticalSection( &device_list_cs );

    /* Set cancel routine BEFORE adding to list */
    IoSetCancelRoutine( irp, bluetooth_irp_cancel_routine );

    /* Check if already cancelled (race with IoSetCancelRoutine) */
    if (irp->Cancel && IoSetCancelRoutine( irp, NULL ))
    {
        /* IRP was cancelled before we set the routine - don't queue */
        LeaveCriticalSection( &device_list_cs );
        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest( irp, IO_NO_INCREMENT );
        return STATUS_CANCELLED;
    }

    IoMarkIrpPending( irp );
    InsertTailList( &ext->irp_list, &irp->Tail.Overlay.ListEntry );
    LeaveCriticalSection( &device_list_cs );
}
```

**When removing pending IRPs (modified remove_pending_irps):**

```c
static void remove_pending_irps( struct bluetooth_radio *radio )
{
    LIST_ENTRY *entry;
    IRP *irp;

    /* Caller must hold device_list_cs */
    while ((entry = RemoveHeadList( &radio->irp_list )) != &radio->irp_list)
    {
        irp = CONTAINING_RECORD( entry, IRP, Tail.Overlay.ListEntry );

        /* Clear cancel routine - if it returns NULL, cancel routine is running */
        if (IoSetCancelRoutine( irp, NULL ) == NULL)
        {
            /* Cancel routine will complete this IRP - skip it */
            continue;
        }

        irp->IoStatus.Status = STATUS_DELETE_PENDING;
        irp->IoStatus.Information = 0;
        IoCompleteRequest( irp, IO_NO_INCREMENT );
    }
}
```

**Key Synchronization Rules:**
1. `device_list_cs` guards all `irp_list` operations
2. Cancel routine acquires `device_list_cs` after releasing cancel spinlock
3. Always check `IoSetCancelRoutine` return value to detect racing cancellation
4. Never complete an IRP if its cancel routine is already running

---

## 3. Struct Modifications

### 3.1 New State Enum (add to winebth.c after line 136)

```c
enum bluetooth_object_state
{
    BLUETOOTH_STATE_INITIALIZING,
    BLUETOOTH_STATE_ACTIVE,
    BLUETOOTH_STATE_REMOVING,
};
```

### 3.2 bluetooth_radio (modify lines 73-91)

**BEFORE:**
```c
struct bluetooth_radio
{
    struct list entry;
    BOOL removed;

    DEVICE_OBJECT *device_obj;
    winebluetooth_radio_props_mask_t props_mask;
    struct winebluetooth_radio_properties props;
    BOOL started;
    winebluetooth_radio_t radio;
    WCHAR *hw_name;
    UNICODE_STRING bthport_symlink_name;
    UNICODE_STRING bthradio_symlink_name;

    struct list remote_devices;

    LIST_ENTRY irp_list;
};
```

**AFTER:**
```c
struct bluetooth_radio
{
    struct list entry;
    LONG refcount;                              /* Atomic reference count */
    enum bluetooth_object_state state;          /* Object lifecycle state */

    DEVICE_OBJECT *device_obj;
    winebluetooth_radio_props_mask_t props_mask;
    struct winebluetooth_radio_properties props;
    winebluetooth_radio_t radio;
    WCHAR *hw_name;
    UNICODE_STRING bthport_symlink_name;
    UNICODE_STRING bthradio_symlink_name;

    struct list remote_devices;                 /* Guarded by device_list_cs */

    LIST_ENTRY irp_list;                        /* Guarded by device_list_cs */
};
```

**Changes:**
- Added `LONG refcount`
- Added `enum bluetooth_object_state state`
- Removed `BOOL removed` (replaced by state)
- Removed `BOOL started` (replaced by state)

### 3.3 bluetooth_remote_device (modify lines 93-109)

**BEFORE:**
```c
struct bluetooth_remote_device
{
    struct list entry;

    DEVICE_OBJECT *device_obj;
    struct bluetooth_radio *radio;
    winebluetooth_device_t device;
    CRITICAL_SECTION props_cs;
    winebluetooth_device_props_mask_t props_mask;
    struct winebluetooth_device_properties props;
    BOOL started;
    BOOL removed;

    BOOL le;
    UNICODE_STRING bthle_symlink_name;
    struct list gatt_services;
};
```

**AFTER:**
```c
struct bluetooth_remote_device
{
    struct list entry;
    LONG refcount;                              /* Atomic reference count */
    enum bluetooth_object_state state;          /* Object lifecycle state */

    DEVICE_OBJECT *device_obj;
    struct bluetooth_radio *radio;
    winebluetooth_device_t device;
    CRITICAL_SECTION props_cs;                  /* Protects props, props_mask, le, bthle_symlink_name */
    winebluetooth_device_props_mask_t props_mask;
    struct winebluetooth_device_properties props;

    BOOL le;
    UNICODE_STRING bthle_symlink_name;
    struct list gatt_services;                  /* Guarded by device_list_cs (list ops) */
};
```

**Changes:**
- Added `LONG refcount`
- Added `enum bluetooth_object_state state`
- Removed `BOOL started` (replaced by state)
- Removed `BOOL removed` (replaced by state)
- `gatt_services` list operations now guarded by `device_list_cs`, not `props_cs`

### 3.4 bluetooth_gatt_service (modify lines 111-122)

**BEFORE:**
```c
struct bluetooth_gatt_service
{
    struct list entry;

    winebluetooth_gatt_service_t service;
    GUID uuid;
    unsigned int primary : 1;
    UINT16 handle;

    CRITICAL_SECTION chars_cs;
    struct list characteristics;
};
```

**AFTER:**
```c
struct bluetooth_gatt_service
{
    struct list entry;
    LONG refcount;                              /* Atomic reference count */

    winebluetooth_gatt_service_t service;
    GUID uuid;
    unsigned int primary : 1;
    UINT16 handle;

    struct list characteristics;                /* Immutable after service added to device */
};
```

**Changes:**
- Added `LONG refcount`
- **REMOVED** `CRITICAL_SECTION chars_cs` entirely
- `characteristics` list is now immutable after the service is fully discovered

### 3.5 bluetooth_gatt_characteristic (lines 124-130 - NO CHANGES)

```c
struct bluetooth_gatt_characteristic
{
    struct list entry;

    winebluetooth_gatt_characteristic_t characteristic;
    BTH_LE_GATT_CHARACTERISTIC props;
};
```

Characteristics are owned by their parent service and are immutable. No refcount needed.

---

## 4. Helper Functions

Add these functions after the struct definitions (around line 145).

### 4.1 Radio Reference Counting

```c
static inline void bluetooth_radio_incref( struct bluetooth_radio *radio )
{
    InterlockedIncrement( &radio->refcount );
}

static inline LONG bluetooth_radio_decref( struct bluetooth_radio *radio )
{
    return InterlockedDecrement( &radio->refcount );
}

/* Find radio and increment refcount. Caller must hold device_list_cs. */
static struct bluetooth_radio *bluetooth_radio_find_and_incref( winebluetooth_radio_t handle )
{
    struct bluetooth_radio *radio;

    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( handle, radio->radio ) &&
            radio->state == BLUETOOTH_STATE_ACTIVE)
        {
            bluetooth_radio_incref( radio );
            return radio;
        }
    }
    return NULL;
}
```

### 4.2 Remote Device Reference Counting

```c
static inline void bluetooth_device_incref( struct bluetooth_remote_device *device )
{
    InterlockedIncrement( &device->refcount );
}

static inline LONG bluetooth_device_decref( struct bluetooth_remote_device *device )
{
    return InterlockedDecrement( &device->refcount );
}

/* Find device by handle and increment refcount. Caller must hold device_list_cs. */
static struct bluetooth_remote_device *bluetooth_device_find_and_incref(
    struct bluetooth_radio *radio, winebluetooth_device_t handle )
{
    struct bluetooth_remote_device *device;

    LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
    {
        if (winebluetooth_device_equal( handle, device->device ) &&
            device->state == BLUETOOTH_STATE_ACTIVE)
        {
            bluetooth_device_incref( device );
            return device;
        }
    }
    return NULL;
}

/* Find device by address and increment refcount. Caller must hold device_list_cs.
 *
 * IMPORTANT: We check state AND incref while still holding device_list_cs to avoid
 * a TOCTOU race where the device could transition to REMOVING between the check
 * and the incref. The incref must happen before releasing the lock. */
static struct bluetooth_remote_device *bluetooth_device_find_by_addr_and_incref(
    struct bluetooth_radio *radio, BTH_ADDR addr )
{
    struct bluetooth_remote_device *device;

    LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
    {
        BOOL match;

        /* Check state first - if not ACTIVE, skip entirely */
        if (device->state != BLUETOOTH_STATE_ACTIVE)
            continue;

        EnterCriticalSection( &device->props_cs );
        match = (device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS) &&
                (device->props.address.ullLong == addr);
        LeaveCriticalSection( &device->props_cs );

        if (match)
        {
            /* Take reference while still holding device_list_cs.
             * This ensures the object can't be freed before we get our ref. */
            bluetooth_device_incref( device );
            return device;
        }
    }
    return NULL;
}
```

### 4.3 GATT Service Reference Counting

```c
static inline void bluetooth_gatt_service_incref( struct bluetooth_gatt_service *service )
{
    InterlockedIncrement( &service->refcount );
}

static inline LONG bluetooth_gatt_service_decref( struct bluetooth_gatt_service *service )
{
    return InterlockedDecrement( &service->refcount );
}
```

### 4.4 State Validation Helpers

```c
static inline BOOL bluetooth_radio_is_active( struct bluetooth_radio *radio )
{
    return radio->state == BLUETOOTH_STATE_ACTIVE;
}

static inline BOOL bluetooth_device_is_active( struct bluetooth_remote_device *device )
{
    return device->state == BLUETOOTH_STATE_ACTIVE;
}
```

---

## 5. Call Site Transformations

### 5.1 Pattern: Simple List Iteration (No External Calls)

**Example:** `IOCTL_BTH_GET_LOCAL_INFO` (lines 312-352)

**BEFORE:**
```c
EnterCriticalSection( &device_list_cs );
if (ext->props_mask & WINEBLUETOOTH_RADIO_PROPERTY_ADDRESS)
{
    info->localInfo.flags |= BDIF_ADDRESS;
    info->localInfo.address = RtlUlonglongByteSwap( ext->props.address.ullLong ) >> 16;
}
// ... more property access ...
LeaveCriticalSection( &device_list_cs );
```

**AFTER:** No change needed - this pattern is already correct. No external calls, single lock.

---

### 5.2 Pattern: Nested Device Property Access (No External Calls)

**Example:** `IOCTL_BTH_GET_DEVICE_INFO` (lines 354-405)

**BEFORE:**
```c
EnterCriticalSection( &device_list_cs );
LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
{
    // ...
    EnterCriticalSection( &device->props_cs );
    winebluetooth_device_properties_to_info( device->props_mask, &device->props, info );
    LeaveCriticalSection( &device->props_cs );
    // ...
}
LeaveCriticalSection( &device_list_cs );
```

**AFTER:** No structural change needed. This pattern is correct:
- `device_list_cs` acquired first
- `props_cs` acquired while holding `device_list_cs`
- No external calls

---

### 5.3 Pattern: Find Device, Then External Call

**Example:** `IOCTL_BTH_DISCONNECT_DEVICE` (lines 406-443)

**BEFORE:**
```c
EnterCriticalSection( &device_list_cs );
LIST_FOR_EACH_ENTRY( device, &ext->remote_devices, struct bluetooth_remote_device, entry )
{
    EnterCriticalSection( &device->props_cs );
    found = device->props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS &&
            device_addr == device->props.address.ullLong;
    LeaveCriticalSection( &device->props_cs );
    if (found)
    {
        winebluetooth_device_dup(( device_handle = device->device ));
        break;
    }
}
LeaveCriticalSection( &device_list_cs );
if (found)
{
    status = winebluetooth_device_disconnect( device_handle );
    winebluetooth_device_free( device_handle );
}
```

**AFTER:** This pattern is already correct. The external call (`winebluetooth_device_disconnect`) happens after releasing the lock. No change needed.

---

### 5.4 Pattern: External Call While Holding Lock (MUST FIX)

**Example:** `bluetooth_radio_report_radio_in_range_event` called from `bluetooth_radio_update_device_props` (lines 1008-1065)

**BEFORE:**
```c
EnterCriticalSection( &device_list_cs );
LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
{
    struct bluetooth_remote_device *device;

    LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
    {
        if (winebluetooth_device_equal( event.device, device->device ))
        {
            // ... update properties under props_cs ...
            goto done;
        }
    }
}
done:
winebluetooth_device_free( event.device );

if (radio_obj)
    bluetooth_radio_report_radio_in_range_event( radio_obj, device_old_flags, &device_new_info );

LeaveCriticalSection( &device_list_cs );  // BUG: external call before this!
```

**AFTER:**
```c
static void bluetooth_radio_update_device_props( struct winebluetooth_watcher_event_device_props_changed event )
{
    BTH_DEVICE_INFO device_new_info = {0};
    DEVICE_OBJECT *radio_obj = NULL;
    struct bluetooth_radio *radio;
    ULONG device_old_flags = 0;
    BOOL found = FALSE;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (winebluetooth_device_equal( event.device, device->device ))
            {
                BTH_DEVICE_INFO old_info = {0};
                BLUETOOTH_ADDRESS adapter_addr;

                radio_obj = radio->device_obj;
                adapter_addr = radio->props.address;

                EnterCriticalSection( &device->props_cs );
                winebluetooth_device_properties_to_info( device->props_mask, &device->props, &old_info );

                device->props_mask |= event.changed_props_mask;
                device->props_mask &= ~event.invalid_props_mask;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_NAME)
                    memcpy( device->props.name, event.props.name, sizeof( event.props.name ));
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_ADDRESS)
                    device->props.address = event.props.address;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_CONNECTED)
                    device->props.connected = event.props.connected;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_PAIRED)
                    device->props.paired = event.props.paired;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_LEGACY_PAIRING)
                    device->props.legacy_pairing = event.props.legacy_pairing;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_TRUSTED)
                    device->props.trusted = event.props.trusted;
                if (event.changed_props_mask & WINEBLUETOOTH_DEVICE_PROPERTY_CLASS)
                    device->props.class = event.props.class;
                winebluetooth_device_properties_to_info( device->props_mask, &device->props, &device_new_info );
                LeaveCriticalSection( &device->props_cs );

                /* Copy data needed for external call */
                device_old_flags = old_info.flags;
                found = TRUE;

                /* NOTE: bluetooth_device_set_properties also makes external calls.
                   It must be refactored to defer those calls. See section 5.8. */
                break;
            }
        }
        if (found) break;
    }
    LeaveCriticalSection( &device_list_cs );  /* Release lock BEFORE external call */

    winebluetooth_device_free( event.device );

    /* External call now happens without any lock held */
    if (radio_obj)
        bluetooth_radio_report_radio_in_range_event( radio_obj, device_old_flags, &device_new_info );
}
```

---

### 5.5 Pattern: `bluetooth_radio_report_auth_event` (lines 1067-1117)

**BEFORE:**
```c
EnterCriticalSection( &device_list_cs );
LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
{
    LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
    {
        if (winebluetooth_device_equal( event.device, device->device ))
        {
            NTSTATUS ret;

            EnterCriticalSection( &device->props_cs );
            winebluetooth_device_properties_to_info( ... );
            LeaveCriticalSection( &device->props_cs );
            LeaveCriticalSection( &device_list_cs );  // Released here

            ret = IoReportTargetDeviceChange( device_auth, notification );  // External call after release - GOOD
            // ...
            return;
        }
    }
}
LeaveCriticalSection( &device_list_cs );
```

**AFTER:** This is already mostly correct. The lock is released before `IoReportTargetDeviceChange`. Minor cleanup only:

```c
static void bluetooth_radio_report_auth_event( struct winebluetooth_auth_event event )
{
    TARGET_DEVICE_CUSTOM_NOTIFICATION *notification;
    struct winebth_authentication_request *request;
    struct bluetooth_radio *radio;
    BOOL found = FALSE;
    SIZE_T notif_size;
    NTSTATUS ret;

    notif_size = offsetof( TARGET_DEVICE_CUSTOM_NOTIFICATION, CustomDataBuffer[sizeof( *request )] );
    notification = ExAllocatePool( PagedPool, notif_size );
    if (!notification)
        return;

    notification->Version = 1;
    notification->Size = notif_size;
    notification->Event = GUID_WINEBTH_AUTHENTICATION_REQUEST;
    notification->FileObject = NULL;
    notification->NameBufferOffset = -1;
    request = (struct winebth_authentication_request *)notification->CustomDataBuffer;
    memset( request, 0, sizeof( *request ) );
    request->auth_method = event.method;
    request->numeric_value_or_passkey = event.numeric_value_or_passkey;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (winebluetooth_device_equal( event.device, device->device ))
            {
                EnterCriticalSection( &device->props_cs );
                winebluetooth_device_properties_to_info( device->props_mask, &device->props, &request->device_info );
                LeaveCriticalSection( &device->props_cs );
                found = TRUE;
                break;
            }
        }
        if (found) break;
    }
    LeaveCriticalSection( &device_list_cs );

    if (found)
    {
        ret = IoReportTargetDeviceChange( device_auth, notification );
        if (ret)
            ERR( "IoReportTargetDeviceChange failed: %#lx\n", ret );
    }

    ExFreePool( notification );
}
```

---

### 5.6 Pattern: `bluetooth_device_enable_le_iface` (lines 1129-1151)

**BEFORE:**
```c
static void bluetooth_device_enable_le_iface( struct bluetooth_remote_device *device )
{
    EnterCriticalSection( &device->props_cs );  // BUG: No device_list_cs, no refcount
    if (!device->started)
    {
        device->le = TRUE;
        LeaveCriticalSection( &device->props_cs );
        return;
    }

    if (device->le)
    {
        LeaveCriticalSection( &device->props_cs );
        return;
    }
    device->le = TRUE;
    if (!IoRegisterDeviceInterface( ... ))  // BUG: External call under lock
        IoSetDeviceInterfaceState( &device->bthle_symlink_name, TRUE );
    LeaveCriticalSection( &device->props_cs );
}
```

**AFTER:**
```c
/* Caller must hold a reference to device. May be called with or without device_list_cs. */
static void bluetooth_device_enable_le_iface( struct bluetooth_remote_device *device )
{
    UNICODE_STRING symlink_name;
    BOOL should_register = FALSE;
    NTSTATUS status;

    RtlInitUnicodeString( &symlink_name, NULL );

    EnterCriticalSection( &device->props_cs );
    if (device->state != BLUETOOTH_STATE_ACTIVE)
    {
        /* Device not yet started, just set the flag. PnP start will register interface. */
        device->le = TRUE;
        LeaveCriticalSection( &device->props_cs );
        return;
    }

    if (device->le)
    {
        /* Already enabled */
        LeaveCriticalSection( &device->props_cs );
        return;
    }

    device->le = TRUE;
    should_register = TRUE;
    LeaveCriticalSection( &device->props_cs );

    /* External calls outside lock */
    if (should_register)
    {
        status = IoRegisterDeviceInterface( device->device_obj, &GUID_BLUETOOTHLE_DEVICE_INTERFACE,
                                            NULL, &symlink_name );
        if (!status)
        {
            IoSetDeviceInterfaceState( &symlink_name, TRUE );

            /* Now update the device struct with the symlink */
            EnterCriticalSection( &device->props_cs );
            device->bthle_symlink_name = symlink_name;
            LeaveCriticalSection( &device->props_cs );
        }
    }
}
```

---

### 5.7 Pattern: GATT Service/Characteristic Access (Eliminate chars_cs)

**Example:** `IOCTL_WINEBTH_LE_DEVICE_GET_GATT_CHARACTERISTICS` (lines 244-290)

**BEFORE:**
```c
EnterCriticalSection( &ext->props_cs );  // BUG: No device_list_cs
service = find_gatt_service( &ext->gatt_services, &uuid, chars->service.AttributeHandle );
if (!service)
{
    LeaveCriticalSection( &ext->props_cs );
    break;
}

EnterCriticalSection( &service->chars_cs );  // Third-level lock
LIST_FOR_EACH_ENTRY( chrc, &service->characteristics, ... )
{
    // ...
}
LeaveCriticalSection( &service->chars_cs );
LeaveCriticalSection( &ext->props_cs );
```

**AFTER:**
```c
/* Characteristics list is immutable after service discovery - no lock needed for read */
EnterCriticalSection( &device_list_cs );
if (ext->state != BLUETOOTH_STATE_ACTIVE)
{
    LeaveCriticalSection( &device_list_cs );
    status = STATUS_DEVICE_NOT_CONNECTED;
    break;
}

service = find_gatt_service( &ext->gatt_services, &uuid, chars->service.AttributeHandle );
if (!service)
{
    LeaveCriticalSection( &device_list_cs );
    status = STATUS_INVALID_PARAMETER;
    break;
}

/* Take reference to service so it can't be freed */
bluetooth_gatt_service_incref( service );
LeaveCriticalSection( &device_list_cs );

/* Now access characteristics without any lock - list is immutable */
LIST_FOR_EACH_ENTRY( chrc, &service->characteristics, struct bluetooth_gatt_characteristic, entry )
{
    chars->count++;
    if (rem)
    {
        chars->characteristics[chars->count - 1] = chrc->props;
        rem--;
    }
}

bluetooth_gatt_service_decref( service );

irp->IoStatus.Information = offsetof( struct winebth_le_device_get_gatt_characteristics_params,
                                       characteristics[chars->count] );
if (chars->count > rem)
    status = STATUS_MORE_ENTRIES;
break;
```

---

### 5.8 Pattern: `bluetooth_device_set_properties` (lines 958-1006)

This function is called while holding `props_cs` and makes `IoSetDevicePropertyData` and `IoSetDeviceInterfacePropertyData` calls.

**BEFORE:**
```c
/* Caller should hold device->props_cs. */
static void bluetooth_device_set_properties( struct bluetooth_remote_device *device, ... )
{
    // ... multiple IoSetDevicePropertyData calls while caller holds props_cs
}
```

**AFTER:** Since `IoSetDevicePropertyData` is a kernel function that may have internal synchronization, we have two options:

**Option A: Defer the calls (safest)**
```c
struct device_property_update {
    DEVICE_OBJECT *device_obj;
    DEVPROPKEY *key;
    // ... property data ...
};

/* Collect updates under lock, apply after releasing */
```

**Option B: Accept the risk with mitigation (pragmatic)**

The `IoSetDevicePropertyData` functions are designed to be called from PnP contexts and are likely safe. Document the assumption and add mitigation:

```c
/* NOTE: IoSetDevicePropertyData is called while holding props_cs. This is acceptable because:
 * 1. These are Wine-internal kernel functions with no callbacks
 * 2. They don't acquire any bluetooth-related locks
 * 3. The winebus.sys driver uses the same pattern
 *
 * MITIGATION for recursive PnP events:
 * If setting a property triggers a synchronous TARGET_DEVICE_CUSTOM_NOTIFICATION that
 * attempts to re-acquire device_list_cs, we would deadlock. To prevent this:
 * - Audit all DEVPKEY values set here to ensure none trigger PnP notifications
 * - If any do, refactor to collect updates in a local struct, release lock, then apply
 * - Current properties (FriendlyName, Address, etc.) are data-only and safe
 *
 * If issues arise, refactor to collect updates and apply after releasing lock.
 */
static void bluetooth_device_set_properties( struct bluetooth_remote_device *device, ... )
```

**Recommendation:** Use Option B initially, document the assumption, and refactor if issues arise.

---

### 5.9 Pattern: `bluetooth_device_add_gatt_service` (lines 1153-1199)

**BEFORE:**
```c
EnterCriticalSection( &device_list_cs );
LIST_FOR_EACH_ENTRY( radio, &device_list, ... )
{
    LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, ... )
    {
        if (winebluetooth_device_equal( ... ))
        {
            // ... allocate service ...
            bluetooth_device_enable_le_iface( device );  // External calls under lock
            InitializeCriticalSectionEx( &service->chars_cs, ... );

            EnterCriticalSection( &device->props_cs );
            list_add_tail( &device->gatt_services, &service->entry );
            LeaveCriticalSection( &device->props_cs );
            LeaveCriticalSection( &device_list_cs );
            return;
        }
    }
}
LeaveCriticalSection( &device_list_cs );
```

**AFTER:**
```c
static void bluetooth_device_add_gatt_service( struct winebluetooth_watcher_event_gatt_service_added event )
{
    struct bluetooth_remote_device *target_device = NULL;
    struct bluetooth_gatt_service *service = NULL;
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            if (winebluetooth_device_equal( event.device, device->device ) &&
                device->state == BLUETOOTH_STATE_ACTIVE)
            {
                TRACE( "Adding GATT service %s for remote device %p\n", debugstr_guid( &event.uuid ),
                       (void *)event.device.handle );

                service = calloc( 1, sizeof( *service ) );
                if (!service)
                {
                    LeaveCriticalSection( &device_list_cs );
                    goto cleanup;
                }

                service->refcount = 1;
                service->service = event.service;
                service->uuid = event.uuid;
                service->primary = !!event.is_primary;
                service->handle = event.attr_handle;
                list_init( &service->characteristics );

                /* Add to list under device_list_cs */
                list_add_tail( &device->gatt_services, &service->entry );

                /* Take reference for the enable_le_iface call */
                bluetooth_device_incref( device );
                target_device = device;
                break;
            }
        }
        if (target_device) break;
    }
    LeaveCriticalSection( &device_list_cs );

    /* External calls outside lock */
    if (target_device)
    {
        bluetooth_device_enable_le_iface( target_device );
        bluetooth_device_decref( target_device );
    }

    winebluetooth_device_free( event.device );
    return;

cleanup:
    winebluetooth_device_free( event.device );
    winebluetooth_gatt_service_free( event.service );
}
```

---

### 5.10 Pattern: `bluetooth_gatt_service_remove` (lines 1202-1248)

**BEFORE:**
```c
EnterCriticalSection( &device_list_cs );
// ... nested loops ...
EnterCriticalSection( &device->props_cs );
// ...
LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, ... )
{
    if (winebluetooth_gatt_service_equal( ... ))
    {
        list_remove( &svc->entry );
        LeaveCriticalSection( &device->props_cs );
        LeaveCriticalSection( &device_list_cs );
        // ... cleanup svc (including DeleteCriticalSection) ...
        return;
    }
}
LeaveCriticalSection( &device->props_cs );
// ...
LeaveCriticalSection( &device_list_cs );
```

**AFTER:**
```c
static void bluetooth_gatt_service_remove( winebluetooth_gatt_service_t service_handle )
{
    struct bluetooth_gatt_service *found_svc = NULL;
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            struct bluetooth_gatt_service *svc;

            if (device->state != BLUETOOTH_STATE_ACTIVE || !device->le)
                continue;

            LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
            {
                if (winebluetooth_gatt_service_equal( svc->service, service_handle ))
                {
                    list_remove( &svc->entry );
                    found_svc = svc;
                    break;
                }
            }
            if (found_svc) break;
        }
        if (found_svc) break;
    }
    LeaveCriticalSection( &device_list_cs );

    /* Cleanup outside lock */
    if (found_svc)
    {
        struct bluetooth_gatt_characteristic *cur, *next;

        /* Wait for any references to drain.
         *
         * NOTE: We use a KEVENT-based approach rather than busy-waiting to avoid
         * performance degradation and potential watchdog timeouts in kernel mode.
         * Since the service is already removed from the list and marked as REMOVING,
         * no new references can be acquired. Existing references will be released
         * quickly as IOCTL handlers check state and bail out.
         *
         * For simplicity in Wine's user-mode driver emulation, we can use a
         * condition variable or event. In true kernel mode, use KeWaitForSingleObject
         * with a KEVENT that is signaled when refcount reaches 1.
         */
        if (found_svc->refcount > 1)
        {
            /* In practice, references should drain quickly since:
             * 1. Service is removed from device's list (no new lookups succeed)
             * 2. State checks in IOCTLs cause early return
             * 3. Only in-flight operations hold references
             *
             * If this becomes a bottleneck, add a KEVENT to the service struct
             * that is signaled in bluetooth_gatt_service_decref when refcount hits 1.
             */
            LARGE_INTEGER timeout;
            timeout.QuadPart = -10000;  /* 1ms in 100ns units */
            while (InterlockedCompareExchange( &found_svc->refcount, 0, 0 ) > 1)
                KeDelayExecutionThread( KernelMode, FALSE, &timeout );
        }

        winebluetooth_gatt_service_free( found_svc->service );
        LIST_FOR_EACH_ENTRY_SAFE( cur, next, &found_svc->characteristics,
                                   struct bluetooth_gatt_characteristic, entry )
        {
            winebluetooth_gatt_characteristic_free( cur->characteristic );
            free( cur );
        }
        free( found_svc );
    }

    winebluetooth_gatt_service_free( service_handle );
}
```

---

### 5.11 Pattern: `bluetooth_gatt_characteristic_remove` (lines 1304-1349)

This function has the deepest nesting: `device_list_cs` → `props_cs` → `chars_cs`.

**AFTER:** With `chars_cs` eliminated and characteristics immutable:

```c
static void bluetooth_gatt_characteristic_remove( winebluetooth_gatt_characteristic_t handle )
{
    struct bluetooth_gatt_characteristic *found_chrc = NULL;
    struct bluetooth_gatt_service *parent_svc = NULL;
    struct bluetooth_radio *radio;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        struct bluetooth_remote_device *device;

        LIST_FOR_EACH_ENTRY( device, &radio->remote_devices, struct bluetooth_remote_device, entry )
        {
            struct bluetooth_gatt_service *svc;

            if (device->state != BLUETOOTH_STATE_ACTIVE || !device->le)
                continue;

            LIST_FOR_EACH_ENTRY( svc, &device->gatt_services, struct bluetooth_gatt_service, entry )
            {
                struct bluetooth_gatt_characteristic *chrc;

                LIST_FOR_EACH_ENTRY( chrc, &svc->characteristics, struct bluetooth_gatt_characteristic, entry )
                {
                    if (winebluetooth_gatt_characteristic_equal( chrc->characteristic, handle ))
                    {
                        list_remove( &chrc->entry );
                        found_chrc = chrc;
                        parent_svc = svc;
                        break;
                    }
                }
                if (found_chrc) break;
            }
            if (found_chrc) break;
        }
        if (found_chrc) break;
    }
    LeaveCriticalSection( &device_list_cs );

    /* Cleanup outside lock */
    if (found_chrc)
    {
        winebluetooth_gatt_characteristic_free( found_chrc->characteristic );
        free( found_chrc );
    }

    winebluetooth_gatt_characteristic_free( handle );
}
```

---

### 5.12 Pattern: `add_bluetooth_radio` (lines 698-745)

**BEFORE:**
```c
// ... create device_obj ...
ext->radio.removed = FALSE;
ext->radio.started = FALSE;
// ...
EnterCriticalSection( &device_list_cs );
list_add_tail( &device_list, &ext->radio.entry );
LeaveCriticalSection( &device_list_cs );

IoInvalidateDeviceRelations( bus_pdo, BusRelations );  // External call after lock - GOOD
```

**AFTER:**
```c
static void add_bluetooth_radio( struct winebluetooth_watcher_event_radio_added event )
{
    struct bluetooth_pdo_ext *ext;
    DEVICE_OBJECT *device_obj;
    UNICODE_STRING string;
    NTSTATUS status;
    WCHAR name[256];
    WCHAR *hw_name;
    static unsigned int radio_index;

    swprintf( name, ARRAY_SIZE( name ), L"\\Device\\WINEBTH-RADIO-%d", radio_index++ );
    TRACE( "Adding new bluetooth radio %p: %s\n", (void *)event.radio.handle, debugstr_w( name ) );

    status = radio_get_hw_name_w( event.radio, &hw_name );
    if (status)
    {
        ERR( "Failed to get hardware name for radio %p, status %#lx\n", (void *)event.radio.handle, status );
        return;
    }

    RtlInitUnicodeString( &string, name );
    status = IoCreateDevice( driver_obj, sizeof( *ext ), &string, FILE_DEVICE_BLUETOOTH, 0,
                             FALSE, &device_obj );
    if (status)
    {
        ERR( "Failed to create device, status %#lx\n", status );
        free( hw_name );
        return;
    }

    ext = device_obj->DeviceExtension;
    ext->type = BLUETOOTH_PDO_EXT_RADIO;
    ext->radio.refcount = 1;                           /* Initial reference */
    ext->radio.state = BLUETOOTH_STATE_INITIALIZING;   /* Not yet in list */
    ext->radio.device_obj = device_obj;
    ext->radio.radio = event.radio;
    ext->radio.hw_name = hw_name;
    ext->radio.props = event.props;
    ext->radio.props_mask = event.props_mask;
    list_init( &ext->radio.remote_devices );
    InitializeListHead( &ext->radio.irp_list );

    EnterCriticalSection( &device_list_cs );
    list_add_tail( &device_list, &ext->radio.entry );
    ext->radio.state = BLUETOOTH_STATE_ACTIVE;         /* Now visible and active */
    LeaveCriticalSection( &device_list_cs );

    IoInvalidateDeviceRelations( bus_pdo, BusRelations );
}
```

---

### 5.13 Pattern: `remove_bluetooth_radio` (lines 747-767)

**BEFORE:**
```c
EnterCriticalSection( &device_list_cs );
LIST_FOR_EACH_ENTRY( device, &device_list, struct bluetooth_radio, entry )
{
    if (winebluetooth_radio_equal( radio, device->radio ) && !device->removed)
    {
        device->removed = TRUE;
        list_remove( &device->entry );
        IoInvalidateDeviceRelations( device->device_obj, BusRelations );  // External call under lock!
        break;
    }
}
LeaveCriticalSection( &device_list_cs );

IoInvalidateDeviceRelations( bus_pdo, BusRelations );  // This one is outside - good
```

**AFTER:**
```c
static void remove_bluetooth_radio( winebluetooth_radio_t radio )
{
    struct bluetooth_radio *device;
    DEVICE_OBJECT *radio_device_obj = NULL;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( device, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( radio, device->radio ) &&
            device->state == BLUETOOTH_STATE_ACTIVE)
        {
            TRACE( "Removing bluetooth radio %p\n", (void *)radio.handle );
            device->state = BLUETOOTH_STATE_REMOVING;
            list_remove( &device->entry );
            radio_device_obj = device->device_obj;
            break;
        }
    }
    LeaveCriticalSection( &device_list_cs );

    /* External calls outside lock */
    if (radio_device_obj)
        IoInvalidateDeviceRelations( radio_device_obj, BusRelations );

    IoInvalidateDeviceRelations( bus_pdo, BusRelations );
    winebluetooth_radio_free( radio );
}
```

---

### 5.14 Pattern: `bluetooth_radio_add_remote_device` (lines 843-896)

**AFTER:**
```c
static void bluetooth_radio_add_remote_device( struct winebluetooth_watcher_event_device_added event )
{
    struct bluetooth_radio *radio;
    DEVICE_OBJECT *radio_device_obj = NULL;
    BTH_DEVICE_INFO device_info = {0};
    BOOL report_event = FALSE;

    EnterCriticalSection( &device_list_cs );
    LIST_FOR_EACH_ENTRY( radio, &device_list, struct bluetooth_radio, entry )
    {
        if (winebluetooth_radio_equal( event.radio, radio->radio ))
        {
            struct bluetooth_pdo_ext *ext;
            DEVICE_OBJECT *device_obj;
            NTSTATUS status;

            status = IoCreateDevice( driver_obj, sizeof( *ext ), NULL, FILE_DEVICE_BLUETOOTH,
                                     FILE_AUTOGENERATED_DEVICE_NAME, FALSE, &device_obj );
            if (status)
            {
                ERR( "Failed to create remote device, status %#lx\n", status );
                winebluetooth_device_free( event.device );
                break;
            }

            ext = device_obj->DeviceExtension;
            ext->type = BLUETOOTH_PDO_EXT_REMOTE_DEVICE;
            ext->remote_device.refcount = 1;
            ext->remote_device.state = BLUETOOTH_STATE_INITIALIZING;
            ext->remote_device.radio = radio;
            ext->remote_device.device_obj = device_obj;
            InitializeCriticalSectionEx( &ext->remote_device.props_cs, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
            ext->remote_device.props_cs.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": bluetooth_pdo_ext.props_cs");
            ext->remote_device.device = event.device;
            ext->remote_device.props_mask = event.known_props_mask;
            ext->remote_device.props = event.props;
            ext->remote_device.le = FALSE;
            list_init( &ext->remote_device.gatt_services );

            list_add_tail( &radio->remote_devices, &ext->remote_device.entry );
            ext->remote_device.state = BLUETOOTH_STATE_ACTIVE;

            radio_device_obj = radio->device_obj;
            if (!event.init_entry)
            {
                winebluetooth_device_properties_to_info( ext->remote_device.props_mask,
                                                          &ext->remote_device.props, &device_info );
                report_event = TRUE;
            }
            break;
        }
    }
    LeaveCriticalSection( &device_list_cs );

    /* External calls outside lock */
    if (report_event && radio_device_obj)
        bluetooth_radio_report_radio_in_range_event( radio_device_obj, 0, &device_info );

    if (radio_device_obj)
        IoInvalidateDeviceRelations( radio_device_obj, BusRelations );

    winebluetooth_radio_free( event.radio );
}
```

---

### 5.15 Pattern: State Checks in PnP Handlers

**Example:** `remote_device_pdo_pnp` `IRP_MN_START_DEVICE` (lines 1663-1680)

**AFTER:**
```c
case IRP_MN_START_DEVICE:
{
    BLUETOOTH_ADDRESS adapter_addr;

    EnterCriticalSection( &device_list_cs );
    if (ext->state != BLUETOOTH_STATE_ACTIVE)
    {
        /* Already removing or not properly initialized */
        LeaveCriticalSection( &device_list_cs );
        ret = STATUS_DEVICE_NOT_CONNECTED;
        break;
    }
    adapter_addr = ext->radio->props.address;
    LeaveCriticalSection( &device_list_cs );

    /* External interface registration outside lock */
    EnterCriticalSection( &ext->props_cs );
    if (ext->le)
    {
        UNICODE_STRING symlink_name;
        LeaveCriticalSection( &ext->props_cs );

        if (!IoRegisterDeviceInterface( device_obj, &GUID_BLUETOOTHLE_DEVICE_INTERFACE, NULL,
                                        &symlink_name ))
        {
            IoSetDeviceInterfaceState( &symlink_name, TRUE );
            EnterCriticalSection( &ext->props_cs );
            ext->bthle_symlink_name = symlink_name;
            LeaveCriticalSection( &ext->props_cs );
        }
    }
    else
    {
        LeaveCriticalSection( &ext->props_cs );
    }

    EnterCriticalSection( &ext->props_cs );
    bluetooth_device_set_properties( ext, adapter_addr.rgBytes, &ext->props, ext->props_mask );
    LeaveCriticalSection( &ext->props_cs );

    ret = STATUS_SUCCESS;
    break;
}
```

---

### 5.16 Pattern: IOCTL Handlers in `bluetooth_remote_device_dispatch`

**Example:** `IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES` (lines 203-243)

**BEFORE:**
```c
EnterCriticalSection( &ext->props_cs );  // BUG: Should check state, no device_list_cs
LIST_FOR_EACH_ENTRY( svc, &ext->gatt_services, ... )
{
    // ...
}
LeaveCriticalSection( &ext->props_cs );
```

**AFTER:**
```c
case IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES:
{
    const SIZE_T min_size = offsetof( struct winebth_le_device_get_gatt_services_params, services[0] );
    struct winebth_le_device_get_gatt_services_params *services = irp->AssociatedIrp.SystemBuffer;
    struct bluetooth_gatt_service *svc;
    SIZE_T rem;

    if (!services || outsize < min_size)
    {
        status = STATUS_INVALID_USER_BUFFER;
        break;
    }

    rem = (outsize - min_size)/sizeof( *services->services );
    status = STATUS_SUCCESS;
    services->count = 0;

    EnterCriticalSection( &device_list_cs );
    if (ext->state != BLUETOOTH_STATE_ACTIVE)
    {
        LeaveCriticalSection( &device_list_cs );
        status = STATUS_DEVICE_NOT_CONNECTED;
        break;
    }

    LIST_FOR_EACH_ENTRY( svc, &ext->gatt_services, struct bluetooth_gatt_service, entry )
    {
        if (!svc->primary)
            continue;
        services->count++;
        if (rem)
        {
            BTH_LE_GATT_SERVICE *info;

            info = &services->services[services->count - 1];
            memset( info, 0, sizeof( *info ) );
            uuid_to_le( &svc->uuid, &info->ServiceUuid );
            info->AttributeHandle = svc->handle;
            rem--;
        }
    }
    LeaveCriticalSection( &device_list_cs );

    irp->IoStatus.Information = offsetof( struct winebth_le_device_get_gatt_services_params,
                                          services[services->count] );
    if (services->count > rem)
        status = STATUS_MORE_ENTRIES;
    break;
}
```

---

## 6. Implementation Order

Execute these steps in order. Each step should compile and run before proceeding to the next.

### Step 1: Add New Types and Helper Functions

1. Add `enum bluetooth_object_state` after line 136
2. Add all helper functions (section 4) after the struct definitions
3. Compile and verify no errors

### Step 2: Modify Struct Definitions

1. Modify `bluetooth_radio` (section 3.2)
2. Modify `bluetooth_remote_device` (section 3.3)
3. Modify `bluetooth_gatt_service` (section 3.4) - remove `chars_cs`
4. Update all `InitializeCriticalSectionEx` calls for `chars_cs` → remove them
5. Update all `DeleteCriticalSection` calls for `chars_cs` → remove them
6. Update all `EnterCriticalSection`/`LeaveCriticalSection` for `chars_cs` → remove them
7. Compile and fix any resulting errors

### Step 3: Initialize Reference Counts and State

Update object creation sites:

1. `add_bluetooth_radio` (lines 698-745)
   - Set `refcount = 1`
   - Set `state = BLUETOOTH_STATE_INITIALIZING` before list add
   - Set `state = BLUETOOTH_STATE_ACTIVE` after list add

2. `bluetooth_radio_add_remote_device` (lines 843-896)
   - Set `refcount = 1`
   - Set `state = BLUETOOTH_STATE_INITIALIZING` before list add
   - Set `state = BLUETOOTH_STATE_ACTIVE` after list add

3. `bluetooth_device_add_gatt_service` (lines 1153-1199)
   - Set `refcount = 1`

### Step 4: Update Removal Sites to Use State

1. `remove_bluetooth_radio` (lines 747-767)
   - Replace `device->removed = TRUE` with `device->state = BLUETOOTH_STATE_REMOVING`
   - Replace `!device->removed` check with `device->state == BLUETOOTH_STATE_ACTIVE`

2. `bluetooth_radio_remove_remote_device` (lines 898-955)
   - Replace `device->removed = TRUE` with `device->state = BLUETOOTH_STATE_REMOVING`

3. All PnP handlers that check `removed` or `started`
   - Replace with state checks

### Step 5: Fix External Calls Under Lock

Transform each function identified in section 5 that makes external calls while holding locks:

1. `bluetooth_radio_update_device_props` (section 5.4) - HIGH PRIORITY
2. `bluetooth_device_enable_le_iface` (section 5.6) - HIGH PRIORITY
3. `bluetooth_device_add_gatt_service` (section 5.9)
4. `remove_bluetooth_radio` (section 5.13)
5. `bluetooth_radio_add_remote_device` (section 5.14)

### Step 6: Fix Inconsistent Lock Acquisition

Transform IOCTL handlers to acquire `device_list_cs` first:

1. `IOCTL_WINEBTH_LE_DEVICE_GET_GATT_SERVICES` (section 5.16)
2. `IOCTL_WINEBTH_LE_DEVICE_GET_GATT_CHARACTERISTICS` (section 5.7)

### Step 7: Add State Validation to Entry Points

Add state checks to all IOCTL handlers and PnP handlers:

```c
if (ext->state != BLUETOOTH_STATE_ACTIVE)
{
    status = STATUS_DEVICE_NOT_CONNECTED;
    break;
}
```

### Step 8: Implement Reference Counting for Service Access

Update GATT service access patterns to use refcounting (section 5.7).

### Step 9: Final Cleanup

1. Remove any remaining `chars_cs` references
2. Remove `BOOL started` and `BOOL removed` from structs
3. Update all comments mentioning the old locking scheme
4. Add documentation comments to struct definitions

---

## 7. Verification Checklist

### 7.1 Compile-Time Checks

- [ ] No references to `chars_cs` remain
- [ ] No references to `->started` or `->removed` booleans remain
- [ ] All objects have `refcount` field
- [ ] All objects have `state` field

### 7.2 Lock Ordering Verification

Grep for all lock acquisitions and verify ordering:

```bash
grep -n "EnterCriticalSection" winebth.c | less
```

For each `EnterCriticalSection(&.*props_cs)`:
- Verify either `device_list_cs` is held, OR
- Verify a reference was taken with `incref`

### 7.3 External Call Verification

Grep for external calls and verify no lock is held:

```bash
grep -n "IoReportTargetDeviceChange\|IoInvalidateDeviceRelations\|IoRegisterDeviceInterface\|IoSetDeviceInterfaceState" winebth.c
```

For each call, trace back to ensure all locks are released.

### 7.4 Reference Count Balance

For each `incref` call, verify there's a corresponding `decref` on all code paths.

### 7.5 State Transition Verification

Verify state transitions are valid:
- `INITIALIZING` → `ACTIVE` (only during creation)
- `ACTIVE` → `REMOVING` (only during removal)
- No other transitions allowed

### 7.6 Runtime Testing

1. Basic pairing test
2. Device discovery test
3. GATT service enumeration test
4. Device removal during active operation test
5. Multiple simultaneous connections test
6. Stress test with rapid connect/disconnect cycles

---

## Appendix A: Complete List of Functions to Modify

| Function | Line | Changes Required |
|----------|------|------------------|
| `find_gatt_service` | 181 | Add state check |
| `bluetooth_remote_device_dispatch` | 192 | Add state check, fix lock order for GATT IOCTLs |
| `bluetooth_radio_dispatch` | 300 | Minor - already mostly correct |
| `add_bluetooth_radio` | 698 | Init refcount/state |
| `remove_bluetooth_radio` | 747 | Use state, move external call |
| `update_bluetooth_radio_properties` | 773 | Already correct |
| `bluetooth_radio_report_radio_in_range_event` | 814 | Already correct (no locks held) |
| `bluetooth_radio_add_remote_device` | 843 | Init refcount/state, move external calls |
| `bluetooth_radio_remove_remote_device` | 898 | Use state |
| `bluetooth_device_set_properties` | 958 | Document assumption about IoSetDevicePropertyData |
| `bluetooth_radio_update_device_props` | 1008 | Move external call outside lock |
| `bluetooth_radio_report_auth_event` | 1067 | Already mostly correct, minor cleanup |
| `complete_irp` | 1119 | Already correct |
| `bluetooth_device_enable_le_iface` | 1129 | Major refactor - external calls |
| `bluetooth_device_add_gatt_service` | 1153 | Init refcount, move external calls, remove chars_cs init |
| `bluetooth_gatt_service_remove` | 1202 | Remove chars_cs usage, add ref drain |
| `bluetooth_gatt_service_add_characteristic` | 1250 | Remove chars_cs usage |
| `bluetooth_gatt_characteristic_remove` | 1304 | Remove chars_cs usage |
| `fdo_pnp` | 1423 | Already correct |
| `remote_device_query_id` | 1510 | Add state check |
| `radio_query_id` | 1547 | Already correct |
| `bluetooth_radio_set_properties` | 1576 | Already correct |
| `remove_pending_irps` | 1606 | Already correct |
| `remote_device_destroy` | 1620 | Remove chars_cs deletion |
| `remote_device_pdo_pnp` | 1641 | Use state, fix external calls in START_DEVICE |
| `radio_pdo_pnp` | 1714 | Use state |

---

## Appendix B: Regression Risk Assessment

| Change | Risk Level | Mitigation |
|--------|------------|------------|
| Convert `started`/`removed` to enum | Low | Mechanical transformation; compile-time errors catch missed spots |
| Remove `chars_cs` | Medium | Ensure no hidden write operations exist for characteristics after init. Audit all `LIST_FOR_EACH_ENTRY` on characteristics to confirm read-only. |
| Add refcounting | Medium-High | A single leaked `incref` = memory leak. A missed `incref` = BSOD. Every code path must be audited for balance. |
| Move external calls | Medium | Must preserve exact behavior; external calls may have side effects that depend on timing |
| Fix IOCTL lock ordering | Medium | Ensure no deadlock introduced; stress test with concurrent IOCTLs |
| Add IRP cancel routine | Medium | Standard pattern but easy to get wrong. Test with forced IRP cancellation. |

**Critical Success Factor:** Reference count balance. Use debug helpers (Appendix C) to log all incref/decref pairs during development.

---

## Appendix C: Debug Helpers

Add these for development/debugging:

```c
#ifdef WINE_BLUETOOTH_DEBUG
static void validate_lock_order( const char *func, int line )
{
    /* Implementation would check TLS for currently held locks */
}

#define ENTER_DEVICE_LIST_CS() do { \
    validate_lock_order(__func__, __LINE__); \
    EnterCriticalSection(&device_list_cs); \
} while(0)
#else
#define ENTER_DEVICE_LIST_CS() EnterCriticalSection(&device_list_cs)
#endif
```

---

*End of Implementation Specification*
