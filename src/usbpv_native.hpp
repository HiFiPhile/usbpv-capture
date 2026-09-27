#pragma once
#include "usbpv_lib.h"

// CH56x backend (WinUSB on Windows, libusb on Linux); preserves the callback ABI used by the writer.
namespace usbpv::native {
const char* UPV_CALL list_devices();
UPV_HANDLE UPV_CALL open_device(const char*, int, void*, pfn_packet_handler);
// Optional hook runs after each parsed USB transfer, including parser errors.
// It flushes callback batches before the reader waits for another transfer.
using BatchEnd = void (*)(void*);
// Return true while a partial batch is pending. Forced service publishes the
// tail on idle waits, stop, and errors. The caller chooses its batching deadline.
using BatchService = bool (*)(void*, bool force);
UPV_HANDLE open_device_batched(const char*, int, void*, pfn_packet_handler, BatchEnd,
                              BatchService = nullptr);
int UPV_CALL close_device(UPV_HANDLE);
int UPV_CALL get_monitor_speed(UPV_HANDLE);
int UPV_CALL get_last_error();
const char* UPV_CALL get_error_string(int);
bool failed(UPV_HANDLE);
}  // namespace usbpv::native
