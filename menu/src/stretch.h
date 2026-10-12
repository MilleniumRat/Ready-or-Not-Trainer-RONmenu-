// Made by MilleniumRat (github.com/MilleniumRat)
#pragma once
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11RenderTargetView;

namespace stretch {
// Stretch the finished frame horizontally by `factor` (1 = off). Call before drawing the overlay.
void Apply11(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* bb, ID3D11RenderTargetView* rtv, float factor);
void Release11();
void SetUnsupported(const char* why);
float Applied();                 // factor actually applied last frame (ESP uses it)
const std::string& Status();     // "ok: ..." / "none: ..." / "off", shown on the menu dot
}
