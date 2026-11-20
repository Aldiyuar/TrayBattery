#pragma once
#include "common.h"

struct FlyoutCallbacks {
    void (*onRefresh)();
    void (*onToggleAutoStart)(bool enabled);
    void (*onQuit)();
};

void Flyout_Init(HINSTANCE hInst, HWND ownerWnd, const FlyoutCallbacks& cb);
void Flyout_Toggle(const DeviceSnapshot* snapshot, POINT anchorPt);
void Flyout_UpdateIfVisible(const DeviceSnapshot* snapshot);
void Flyout_Hide();
bool Flyout_IsVisible();
bool Flyout_WillOpenOnToggle();
void Flyout_Shutdown();
