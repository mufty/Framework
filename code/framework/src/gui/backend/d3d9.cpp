#include "d3d9.h"

namespace Framework::GUI {
    bool D3D9Backend::Init(IDirect3D9 *device, IDirect3D9 *_nothing) {
        _device = device;

        return true;
    }

    bool D3D9Backend::Shutdown() {
        return true;
    }
} // namespace Framework::GUI