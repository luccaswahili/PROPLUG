// Windows file dialogs for "Load Reference..." and "Export .cube...".
#include <windows.h>
#include <commdlg.h>
#include "LookMatch.h"

static bool toUtf8(const wchar_t* w, char* out, int capacity) {
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, capacity, nullptr, nullptr) > 0;
}

bool LM_OpenImageDialog(char* pathOut, int capacity) {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.tga;*.bmp)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"LookMatch: choose the reference still";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) && toUtf8(file, pathOut, capacity);
}

bool LM_SaveCubeDialog(char* pathOut, int capacity) {
    wchar_t file[MAX_PATH] = L"LookMatch.cube";
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Cube LUT (*.cube)\0*.cube\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"cube";
    ofn.lpstrTitle = L"LookMatch: export LUT for Lumetri";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameW(&ofn) && toUtf8(file, pathOut, capacity);
}
