// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "../../common/kutil.hh"
#include "core.hh"
#include "opengl.hh"
#include "runner.hh"
#include "render.hh"

bool Step()
{
    if (!StartRender())
        return false;

    ImGui::ShowTestWindow();

    Render();
    SwapGLBuffers();

    if (!sys_main->run) {
        ReleaseRender();
    }

    return true;
}
