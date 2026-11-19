def can_build(env, platform):
    # The Haxe module only needs a C++ compiler for the engine side. The Haxe
    # toolchain (haxe + hxcpp) is an external dependency of the projects that
    # use the language, not of the engine build.
    return True


def configure(env):
    # The Haxe library of a web game is a WebAssembly side module loaded by
    # dlopen. hxcpp maps Haxe `throw`/`try` to C++ exceptions, and a side module
    # that throws needs exception support in the page runtime: Emscripten's
    # dynamic linker resolves the libc++abi entry points (`__cxa_throw`,
    # `__cxa_begin_catch`) and the `invoke_*` wrappers against the main module.
    # Godot itself compiles with `-fno-exceptions`, so this module enables the
    # runtime support at link time (`-fexceptions` adds Emscripten's exception
    # library, `EMCC_FORCE_STDLIBS` makes sure libc++abi is part of the main
    # module and therefore visible to dynamic libraries). The engine code is
    # still compiled without exceptions; only the support code is linked in.
    if env["platform"] == "web":
        # WebAssembly exceptions (not Emscripten's JS-based scheme): the latter
        # is incompatible with the `SUPPORT_LONGJMP=wasm` setting the web
        # platform forces, and wasm exceptions are the only mode that works with
        # dynamic linking in current Emscripten.
        #
        # `-fwasm-exceptions` makes the runtime create and export the
        # `__cpp_exception` tag that a throwing side module imports. It also
        # switches the JS stack allocator to a wasm helper, which has to be
        # exported explicitly because the main module's symbol list would
        # otherwise hide it.
        env.Append(LINKFLAGS=["-fwasm-exceptions", "-Wl,--export=__emscripten_stack_alloc"])


def is_enabled():
    # Built by default: a Haxe project has to run on a stock engine build, the
    # same way GDScript and C# projects do on the builds that include their
    # modules. Pass module_haxe_enabled=no for a build without the language.
    return True


def get_doc_classes():
    return []


def get_doc_path():
    return "doc_classes"