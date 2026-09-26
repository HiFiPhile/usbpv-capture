#pragma once
#include "usbpv_lib.h"

// Windows CH56x/WinUSB backend; preserves the callback ABI used by the writer.
namespace usbpv::native {
const char* UPV_CALL list_devices();
UPV_HANDLE UPV_CALL open_device(const char*, int, void*, pfn_packet_handler);
int UPV_CALL close_device(UPV_HANDLE);
int UPV_CALL get_monitor_speed(UPV_HANDLE);
int UPV_CALL get_last_error();
const char* UPV_CALL get_error_string(int);
bool failed(UPV_HANDLE);
}  // namespace usbpv::native
