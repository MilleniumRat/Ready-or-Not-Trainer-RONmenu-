// Made by MilleniumRat (github.com/MilleniumRat)
#pragma once

namespace overlay {
// Creates dummy D3D11/D3D12 swap chains to find the vtables, then patches Present /
// ResizeBuffers / ExecuteCommandLists. Rendering starts on the game's first Present.
bool Install();
// Undoes every patch, waits for in-flight hook calls and tears down ImGui.
void Uninstall();
}
