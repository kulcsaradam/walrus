#!/usr/bin/env python3

# Copyright 2018-present Samsung Electronics Co., Ltd.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from __future__ import print_function

import os
import traceback
import sys
import time
import re
import fnmatch

from argparse import ArgumentParser
from difflib import unified_diff
from glob import glob
from os.path import abspath, basename, dirname, join, relpath
from shutil import copy
from subprocess import PIPE, Popen, run, CalledProcessError


PROJECT_SOURCE_DIR = dirname(dirname(abspath(__file__)))
DEFAULT_WALRUS = join(PROJECT_SOURCE_DIR, 'walrus')


COLOR_RED = '\033[31m'
COLOR_GREEN = '\033[32m'
COLOR_YELLOW = '\033[33m'
COLOR_BLUE = '\033[34m'
COLOR_PURPLE = '\033[35m'
COLOR_RESET = '\033[0m'


RUNNERS = {}
DEFAULT_RUNNERS = []
JIT_EXCLUDE_FILES = []
jit = False
jit_no_reg_alloc = False
multi_memory = False


class runner(object):

    def __init__(self, suite, default=False):
        self.suite = suite
        self.default = default

    def __call__(self, fn):
        RUNNERS[self.suite] = fn
        if self.default:
            DEFAULT_RUNNERS.append(self.suite)
        return fn

def _run_wast_tests(engine, files, is_fail, args=None):
    fails = 0
    for file in files:
        if jit or jit_no_reg_alloc:
            filename = os.path.basename(file)
            if filename in JIT_EXCLUDE_FILES:
                continue
        subprocess_args =  qemu + [engine, "--mapdirs", "./test/wasi", "/var"]
        if jit or jit_no_reg_alloc: subprocess_args.append("--jit")
        if jit_no_reg_alloc: subprocess_args.append("--jit-no-reg-alloc")
        if multi_memory: subprocess_args.append("--enable-multi-memory")
        if args: subprocess_args.append("--args")
        subprocess_args.append(file)
        if args: subprocess_args.extend(args)

        if len(qemu) == 0:
            proc = Popen(subprocess_args, stdout=PIPE, stderr=PIPE)
            out, _ = proc.communicate()
            returncode = proc.returncode
        else:
            try:
                run(subprocess_args, check=True, stdout=PIPE, stderr=PIPE)
                returncode = 0
                out = ""
            except CalledProcessError as e:
                returncode = e.returncode
                out = e.stdout.decode('utf-8') + e.stderr.decode('utf-8')

        if is_fail and returncode or not is_fail and not returncode:
            print('%sOK: %s%s' % (COLOR_GREEN, file, COLOR_RESET))
        else:
            print('%sFAIL(%d): %s%s' % (COLOR_RED, returncode, file, COLOR_RESET))
            print(out)
            fails += 1

    return fails


@runner('basic-tests', default=True)
def run_basic_tests(engine):
    TEST_DIR = join(PROJECT_SOURCE_DIR, 'test', 'basic')

    print('Running basic tests:')
    xpass = glob(join(TEST_DIR, '*.wast'))
    xpass_result = _run_wast_tests(engine, xpass, False)

    tests_total = len(xpass)
    fail_total = xpass_result
    print('TOTAL: %d' % (tests_total))
    print('%sPASS : %d%s' % (COLOR_GREEN, tests_total - fail_total, COLOR_RESET))
    print('%sFAIL : %d%s' % (COLOR_RED, fail_total, COLOR_RESET))

    if fail_total > 0:
        raise Exception("basic tests failed")


@runner('wasm-test-core', default=True)
def run_core_tests(engine):
    TEST_DIR = join(PROJECT_SOURCE_DIR, 'test', 'wasm-spec', 'core')
    global multi_memory

    print('Running wasm-test-core tests:')
    xpass_core = [i for i in glob(join(TEST_DIR, '**/*.wast'), recursive=True) if "multi-memory" not in i]
    xpass_multi_memory = [i for i in glob(join(TEST_DIR, '**/*.wast'), recursive=True) if "multi-memory" in i]

    xpass_core_result = _run_wast_tests(engine, xpass_core, False)

    multi_memory = True
    xpass_multi_memory_result = _run_wast_tests(engine, xpass_multi_memory, False)
    multi_memory = False

    tests_total = len(xpass_core) + len(xpass_multi_memory)
    fail_total = xpass_core_result + xpass_multi_memory_result
    print('TOTAL: %d' % (tests_total))
    print('%sPASS : %d%s' % (COLOR_GREEN, tests_total - fail_total, COLOR_RESET))
    print('%sFAIL : %d%s' % (COLOR_RED, fail_total, COLOR_RESET))

    if fail_total > 0:
        raise Exception("wasm-test-core failed")


@runner('wasi', default=True)
def run_wasi_tests(engine):
    TEST_DIR = join(PROJECT_SOURCE_DIR, 'test', 'wasi')

    print('Running wasi tests:')
    xpass = glob(join(TEST_DIR, '*.wast'))
    args_tests = glob(join(TEST_DIR, 'args.wast'))
    for item in args_tests:
        xpass.remove(item)

    xpass_result = _run_wast_tests(engine, xpass, False)
    xpass_result += _run_wast_tests(engine, args_tests, False,
                                    args=["Hello", "World!", "Lorem ipsum dolor sit amet, consectetur adipiscing elit"])

    tests_total = len(xpass) + len(args_tests)
    fail_total = xpass_result
    print('TOTAL: %d' % (tests_total))
    print('%sPASS : %d%s' % (COLOR_GREEN, tests_total - fail_total, COLOR_RESET))
    print('%sFAIL : %d%s' % (COLOR_RED, fail_total, COLOR_RESET))

    if fail_total > 0:
        raise Exception("basic wasi tests failed")

@runner('jit', default=True)
def run_jit_tests(engine):
    TEST_DIR = join(PROJECT_SOURCE_DIR, 'test', 'jit')

    print('Running jit tests:')
    xpass = glob(join(TEST_DIR, '*.wast'))
    xpass_result = _run_wast_tests(engine, xpass, False)

    tests_total = len(xpass)
    fail_total = xpass_result
    print('TOTAL: %d' % (tests_total))
    print('%sPASS : %d%s' % (COLOR_GREEN, tests_total - fail_total, COLOR_RESET))
    print('%sFAIL : %d%s' % (COLOR_RED, fail_total, COLOR_RESET))

    if fail_total > 0:
        raise Exception("basic wasm-test-core failed")

@runner('wasm-test-extended', default=True)
def run_extended_tests(engine):
    TEST_DIR = join(PROJECT_SOURCE_DIR, 'test', 'extended')

    print('Running wasm-extended tests:')
    xpass = glob(join(TEST_DIR, '**/*.wast'), recursive=True)
    xpass_result = _run_wast_tests(engine, xpass, False)

    tests_total = len(xpass)
    fail_total = xpass_result
    print('TOTAL: %d' % (tests_total))
    print('%sPASS : %d%s' % (COLOR_GREEN, tests_total - fail_total, COLOR_RESET))
    print('%sFAIL : %d%s' % (COLOR_RED, fail_total, COLOR_RESET))

    if fail_total > 0:
        raise Exception("wasm-test-extended failed")

def main():
    parser = ArgumentParser(description='Walrus Test Suite Runner')
    parser.add_argument('--engine', metavar='PATH', default=DEFAULT_WALRUS,
                        help='path to the engine to be tested (default: %(default)s)')
    parser.add_argument('--qemu', metavar='PATH', default=None, help='path to qemu')
    parser.add_argument('suite', metavar='SUITE', nargs='*', default=sorted(DEFAULT_RUNNERS),
                        help='test suite to run (%s; default: %s)' % (', '.join(sorted(RUNNERS.keys())), ' '.join(sorted(DEFAULT_RUNNERS))))
    parser.add_argument('--jit', action='store_true', help='test with JIT')
    parser.add_argument('--jit-no-reg-alloc', action='store_true', help='test with JIT without register allocation')
    args = parser.parse_args()
    global jit
    jit = args.jit

    global jit_no_reg_alloc
    jit_no_reg_alloc = args.jit_no_reg_alloc

    global qemu
    qemu = [args.qemu] if args.qemu else []

    if jit and jit_no_reg_alloc:
        parser.error('jit and jit-no-reg-alloc cannot be used together')

    if jit or jit_no_reg_alloc:
        exclude_list_file = join(PROJECT_SOURCE_DIR, 'tools', 'jit_exclude_list.txt')
        with open(exclude_list_file) as f:
            global JIT_EXCLUDE_FILES
            JIT_EXCLUDE_FILES = f.read().replace('\n', ' ').split()


    for suite in args.suite:
        if suite not in RUNNERS:
            parser.error('invalid test suite: %s' % suite)

    success, fail = [], []

    for suite in args.suite:
        text = ""
        if jit:
            text = " with jit"
        elif jit_no_reg_alloc:
            text = " with jit without register allocation"
        print(COLOR_PURPLE + f'running test suite{text}: ' + suite + COLOR_RESET)
        try:
            RUNNERS[suite](args.engine)
            success += [suite]
        except Exception as e:
            print('\n'.join(COLOR_YELLOW + line + COLOR_RESET for line in traceback.format_exc().splitlines()))
            fail += [suite]

    if success:
        print(COLOR_GREEN + sys.argv[0] + ': success: ' + ', '.join(success) + COLOR_RESET)
    sys.exit(COLOR_RED + sys.argv[0] + ': fail: ' + ', '.join(fail) + COLOR_RESET if fail else None)


if __name__ == '__main__':
    main()
