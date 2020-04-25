// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "../core/libcc/libcc.hh"
#include "error.hh"
#include "program.hh"

namespace RG {

class VirtualMachine {
    Span<const Instruction> ir;

public:
    const Program *const program;

    HeapArray<Value> stack;
    Size pc = 0;
    Size bp = 0;

    VirtualMachine(const Program &program) : ir(program.ir), program(&program) {}

    bool Run(int *out_exit_code);

    void DecodeFrames(const VirtualMachine &vm, HeapArray<FrameInfo> *out_frames);

private:
    void DumpInstruction();

    template <typename... Args>
    void FatalError(const char *fmt, Args... args)
    {
        HeapArray<FrameInfo> frames;
        DecodeFrames(*this, &frames);

        ReportRuntimeError(frames, fmt, args...);
    }
};

bool Run(const Program &program, int *out_exit_code);

}
