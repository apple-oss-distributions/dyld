#!/usr/bin/python3

import string
import os
import json
import sys
import importlib
import importlib.machinery
import os.path
import traceback

sys.dont_write_bytecode = True

import KernelCollection


if __name__ == "__main__":
    test_dir = os.path.realpath(os.path.dirname(__file__))
    sys.path.append(test_dir)
    all_tests = os.listdir(test_dir)
    all_tests.sort()
    test_to_run = ""
    if len(sys.argv) == 2:
        test_to_run = sys.argv[1]
        all_tests = [ test_to_run ]
    for f in all_tests:
        test_case = test_dir + "/" + f + "/test.py"
        if os.path.isfile(test_case):
            py_mod = importlib.machinery.SourceFileLoader(f, test_case).load_module()
            check_func = getattr(py_mod, "check", 0)
            if check_func == 0:
                print("FAIL: " + f + ", missing check() function")
            else:
                try:
                    kernelCollection = KernelCollection.KernelCollection(test_to_run != "")
                    check_func(kernelCollection)
                    print("PASS: " + f)
                except AssertionError:
                    _, _, tb = sys.exc_info()
                    tb_info = traceback.extract_tb(tb)
                    filename, line, func, text = tb_info[-1]
                    print("FAIL: " + f + ", " + text)
                except KeyError:
                    _, _, tb = sys.exc_info()
                    tb_info = traceback.extract_tb(tb)
                    filename, line, func, text = tb_info[-1]
                    print("FAIL: " + f + ", " + text)

