// Copyright 2023 Niels Martignène <niels.martignene@protonmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the “Software”), to deal in 
// the Software without restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
// Software, and to permit persons to whom the Software is furnished to do so,
// subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

#pragma once

#include "src/core/libcc/libcc.hh"
#include "src/core/libwrap/json.hh"

namespace RG {

struct http_RequestInfo;
class http_IO;

struct http_ByteRange {
    Size start;
    Size end;
};

const char *http_GetMimeType(Span<const char> extension,
                             const char *default_type = "application/octet-stream");

uint32_t http_ParseAcceptableEncodings(Span<const char> encodings);
bool http_ParseRange(Span<const char> str, Size len, LocalArray<http_ByteRange, 16> *out_ranges);

void http_EncodeUrlSafe(Span<const char> str, const char *passthrough, HeapArray<char> *out_buf);
static inline void http_EncodeUrlSafe(Span<const char> str, HeapArray<char> *out_buf)
    { return http_EncodeUrlSafe(str, nullptr, out_buf); }

bool http_PreventCSRF(const http_RequestInfo &request, http_IO *io);

class http_JsonPageBuilder: public json_Writer {
    http_IO *io = nullptr;

    HeapArray<uint8_t> buf;
    StreamWriter st;

public:
    http_JsonPageBuilder(): json_Writer(&st) {}

    bool Init(http_IO *io);
    void Finish();
};

}
