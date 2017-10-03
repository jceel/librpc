#!/usr/bin/env python3
#
# Copyright 2017 Two Pore Guys, Inc.
# All rights reserved
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted providing that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
# IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES LOSS OF USE, DATA, OR PROFITS OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
# STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
# IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#

import os
import argparse
import itertools
import shutil
import mako
import mako.template
import mako.lookup
import librpc


curdir = os.path.abspath(os.path.dirname(__file__))
templates_path = os.path.join(curdir, 'templates')
lookup = mako.lookup.TemplateLookup(directories=[templates_path])


CLASS_NAMES = {
    librpc.TypeClass.STRUCT: 'struct',
    librpc.TypeClass.UNION: 'union',
    librpc.TypeClass.ENUM: 'enum',
    librpc.TypeClass.TYPEDEF: 'typedef',
    librpc.TypeClass.BUILTIN: 'builtin'
}


def generate_module(typing, name):
    entries = typing.types
    typedefs = (t for t in entries if t.is_builtin or t.is_typedef)
    structures = (t for t in entries if t.is_struct or t.is_union or t.is_enum)

    t = lookup.get_template('module.mako')
    return t.render(
        typedefs=sorted(typedefs, key=lambda t: t.name),
        structures=sorted(structures, key=lambda t: t.name)
    )


def generate_file(outdir, name, contents):
    with open(os.path.join(outdir, name), 'w') as f:
        f.write(contents)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-f', action='append')
    parser.add_argument('-d', action='append')
    parser.add_argument('-o')
    args = parser.parse_args()

    typing = librpc.Typing()
    outdir = args.o

    for f in args.f or []:
        typing.load_types(f)

    for d in args.d or []:
        for f in os.listdir(d):
            typing.load_types(f)

    if not os.path.exists(args.o):
        os.makedirs(args.o, exist_ok=True)

    # Copy the CSS file
    shutil.copy(os.path.join(curdir, 'assets/main.css'), outdir)

    generate_file(outdir, 'index.html', generate_module(typing, 'foo'))


if __name__ == '__main__':
    main()
