#!/usr/bin/env python3

# Copyright 2023 Niels Martignène <niels.martignene@protonmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the “Software”), to deal in 
# the Software without restriction, including without limitation the rights to use,
# copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
# Software, and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
# OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
# HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
# WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import sys
import re
import argparse

def load_words(filename):
    with open(filename, 'r', encoding = 'utf-8') as f:
        lines = (line.strip() for line in f.readlines())
        words = [line for line in lines if len(line) > 0 and not line.startswith('#')]

    return words

def simplify(word):
    word = word.lower()
    parts = re.split('[ ,;:]+', word)

    if len(parts) > 2:
        return None

    if len(parts) == 2:
        word = parts[0]
        frequency = int(parts[1])
    else:
        frequency = 100

    if frequency < 10:
        return None
    if re.match('[0-9]', word):
        return None
    if word.endswith('\'s'):
        return None

    word = re.sub('[ç]', 'c', word)
    word = re.sub('[èéêë]', 'e', word)
    word = re.sub('[àâäå]', 'a', word)
    word = re.sub('[îï]', 'i', word)
    word = re.sub('[ùüûú]', 'u', word)
    word = re.sub('[ñ]', 'y', word)
    word = re.sub('[œ]', 'oe', word)
    word = re.sub('[ôóö]', 'o', word)
    word = re.sub('[ÿ]', 'y', word)
    word = re.sub('[—–\\-]', '', word)

    return word

def write_dict_header(words, f):
    f.write("""// Copyright 2023 Niels Martignène <niels.martignene@protonmail.com>
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

// This file is autogenerated by password_dict_gen.py

namespace RG {

static const char pwd_DictRaw[] = {""")
    offsets = []
    offset = 0
    for i, word in enumerate(words):
        if i % 2048 == 0: f.write('\n   ')
        for c in word:
            if ord(c) >= 128:
                print('Complex character in', word, file = sys.stderr)
            f.write(f' 0x{ord(c):02X},')
        f.write(' 0x00,')
        offsets.append(offset)
        offset += len(word) + 1
    f.write("""
};

static const uint32_t pwd_DictWords[] = {""")
    for i, offset in enumerate(offsets):
        if i % 2048 == 0: f.write('\n   ')
        f.write(f' 0x{offset:06X},')
    f.write("""
};

}
""")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description = 'Create password_dict.hh (libpasswd) from dictionaries')
    parser.add_argument('filenames', metavar = 'dictionaries', type = str, nargs = '+',
                        help = 'path to dictionaries')
    args = parser.parse_args()

    raw_words = []
    for filename in args.filenames:
        file_words = load_words(filename)
        raw_words.extend(file_words)

    simplified_words = set((simplify(word) for word in raw_words if len(word) > 3))
    simplified_words = [word for word in simplified_words if word is not None]
    simplified_words.sort()

    write_dict_header(simplified_words, sys.stdout)
