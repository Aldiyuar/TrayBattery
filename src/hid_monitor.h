#pragma once
#include "common.h"

void HidMonitor_Start(HWND notifyWnd);
void HidMonitor_Stop();
void HidMonitor_TriggerRescan();
void HidMonitor_CollectDevices(DeviceSnapshot* outSnapshot);
