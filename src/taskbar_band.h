#pragma once
#include "common.h"

void TaskbarBand_Init(HINSTANCE hInst, HWND mainWnd);
void TaskbarBand_Update(const DeviceSnapshot* snapshot);
void TaskbarBand_SyncPosition();
void TaskbarBand_Shutdown();
void TaskbarBand_RenderSampleToDc(HDC hdc, int w, int h, const DeviceInfo* devs, int count, bool lightTheme, COLORREF bgColor);
void TaskbarBand_DrawDeviceIconGdip(void* gpGraphics, float x, float y, float sz, DeviceType type, uint32_t argbColor);
