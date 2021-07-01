#!/usr/bin/env python3
#
# This script reads C++ or RST source files and writes all
# multi-line strings into individual files.
# This can be used to extract the Solidity test cases
# into files for e.g. fuzz testing as
# scripts/isolate_tests.py test/libsolidity/*

import sys
import re
import os
import hashlib
from os.path import join, isfile, split

def extract_test_cases(path):
    with open(path, encoding="utf8", errors='ignore', mode='r', newline='') as file:
        lines = file.read().splitlines()

    inside = False
    delimiter = ''
    tests = []

    for l in lines:
        if inside:
            if l.strip().endswith(')' + delimiter + '";'):
                inside = False
            else:
                tests[-1] += l + '\n'
        else:
            m = re.search(r'R"([^(]*)\($', l.strip())
            if m:
                inside = True
                delimiter = m.group(1)
                tests += ['']

    return tests

# Extract code examples based on the 'beginMarker' parameter
# up until we reach EOF or a line that is not empty and doesn't start with 4
# spaces.
def extract_docs_cases(path, beginMarker):
    insideBlock = False
    tests = []

    # Collect all snippets of indented blocks
    with open(path, mode='r', errors='ignore', encoding='utf8', newline='') as f:
        lines = f.read().splitlines()

    for line in lines:
        if insideBlock:
            if line == '' or line.startswith("    "):
                if line != "    :force:": # Ignore instruction for Shpinx
                    tests[-1] += line + "\n"
            else:
                insideBlock = False
        elif line.lower().startswith(beginMarker):
            insideBlock = True
            tests += ['']

    codeStart = "(// SPDX-License-Identifier:|pragma solidity|contract.*{|library.*{|interface.*{)"

    # Filter out tests that are not compilable.
    return [test.lstrip("\n") for test in list(filter(lambda test: re.search(r'^\s{4}' + codeStart, test, re.MULTILINE), tests))]

def write_cases(f, tests):
    cleaned_filename = f.replace(".","_").replace("-","_").replace(" ","_").lower()
    for test in tests:
        # When code examples are extracted they are indented by 8 spaces, which violates the style guide,
        # so before checking remove 4 spaces from each line.
        remainder = re.sub(r'^ {4}', '', test, 0, re.MULTILINE)
        sol_filename = 'test_%s_%s.sol' % (hashlib.sha256(test.encode("utf-8")).hexdigest(), cleaned_filename)
        with open(sol_filename, mode='w', encoding='utf8', newline='') as fi:
            fi.write(remainder)

def extract_and_write(f, path):
    if docs:
        cases = extract_docs_cases(path, ".. code-block:: solidity")
    else:
        if f.endswith('.sol'):
            with open(path, mode='r', encoding='utf8', newline='') as _f:
                cases = [_f.read()]
        else:
            cases = extract_test_cases(path)
    write_cases(f, cases)

if __name__ == '__main__':
    if len(sys.argv) == 1:
        print("Usage: " + sys.argv[0] + " path-to-file-or-folder-to-extract-code-from [docs]")
        exit(1)

    path = sys.argv[1]
    docs = False
    if len(sys.argv) > 2 and sys.argv[2] == 'docs':
        docs = True

    if isfile(path):
        _, tail = split(path)
        extract_and_write(tail, path)
    else:
        for root, subdirs, files in os.walk(path):
            if '_build' in subdirs:
                subdirs.remove('_build')
            if 'compilationTests' in subdirs:
                subdirs.remove('compilationTests')
            for f in files:
                _, tail = split(f)
                if tail == "invalid_utf8_sequence.sol":
                    continue  # ignore the test with broken utf-8 encoding
                path = join(root, f)
                extract_and_write(f, path)
