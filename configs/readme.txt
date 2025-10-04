All targets in this project use an xcconfig file.  Most targets use a config file with the same name as the target, eg, dyld_info uses dyld_info.xcconfig.

There are a number of config files imported by others, eg, most configs include base.xcconfig or unittests_base.xcconfig.  Over time we should have as many options as possible in these base config files, to avoid duplication in every client.

The following is the tree of all config files.  Please keep it up to date!

base
    dyld
    libKernelCollectionBuilder
    dyld_tests
    libdsc
    dsc_extractor
    libdyld
    libdyld_introspection_static
    libmach_o
        libmach_o_writer
    framework
    shared_cache_runtime
    test_support
    tools_base
        dyld_app_cache_util
        dyld_closure_util
        dyld_info
        dyld_inspect
        dyld_nm
        dyld_shared_cache_util
        dyld_symbols_cache
        dyld_usage
        ld_base
            shared_cache_linker
            ld
            ld_unittests
                kernel_linker_unittests
            libcctoolshelper
        machocheck
        nocr
        shared_cache_builder_base
            dyld_shared_cache_builder
            libslc_builder
        update_dyld_shared_cache_tool
        update_dyld_sim_shared_cache
    update_dyld_shared_cache-target
    unittests_base
        unittests
            allocator_unittests
            common_unittests
            dyld_unittests
            framework_testlib_unittests
            framework_unitttests
            libdyld_unittests
            lsl_unittests
            mach_o_unittests
            other_tools_unittests
            shared_cache_runtime_unittests
            symbols_cache_unittests
        cache_builder_unittests
        ld_unittests
            kernel_linker_unittests

