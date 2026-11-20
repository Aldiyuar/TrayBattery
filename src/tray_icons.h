#pragma once
#include "common.h"

void TrayIcons_Init(HWND hwnd);
void TrayIcons_Update(const DeviceSnapshot* snapshot);
void TrayIcons_ReAddAll();
void TrayIcons_Shutdown();
int  TrayIcons_VisibleLimit();
HICON TrayIcons_CreateAppIcon();
