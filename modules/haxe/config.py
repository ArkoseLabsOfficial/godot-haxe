def can_build(env, platform):
    # The Haxe module only needs a C++ compiler for the engine side. The Haxe
    # toolchain (haxe + hxcpp) is an external dependency of the projects that
    # use the language, not of the engine build.
    #
    # The Android editor is the one exception: it *is* the editor that has to
    # compile a project, and the Haxe compiler cannot be installed or run on
    # Android. Shipping the language there would only offer a tool that can
    # never work, so the module is left out of that target. Android templates
    # keep it: a game is compiled on a desktop and only run on the device.
    if platform == "android" and env["target"] == "editor":
        return False
    return True


def configure(env):
    # Dynamic linking is what lets a web page load the compiled Haxe library at
    # all: without it the engine is a single module with no dynamic linker, and
    # `HaxeRuntime` cannot resolve the library's C entry points. The extra link
    # flags below only make sense for such a template, and the plain web
    # template (web_builds.yml) would reject them.
    if env["platform"] == "web" and env["dlink_enabled"]:
        # WebAssembly exceptions (not Emscripten's JS-based scheme): the latter
        # is incompatible with the `SUPPORT_LONGJMP=wasm` setting the web
        # platform forces, and wasm exceptions are the only mode that works with
        # dynamic linking in current Emscripten.
        #
        # `-fwasm-exceptions` makes the runtime create the `__cpp_exception` tag
        # that a throwing side module imports. It also switches the stack
        # allocator to a wasm helper (`emscripten_stack_alloc`), which the main
        # module already defines *and* exports under this mode; the dlink
        # template links with `MAIN_MODULE=1 EXPORT_ALL=1`, so every defined
        # symbol (including that helper) is exported without naming it here.
        env.Append(LINKFLAGS=["-fwasm-exceptions"])


def is_enabled():
    # Built by default: a Haxe project has to run on a stock engine build, the
    # same way GDScript and C# projects do on the builds that include their
    # modules. Pass module_haxe_enabled=no for a build without the language.
    return True


def get_doc_classes():
    # The classes the module registers with ClassDB. `--doctool` documents every
    # exposed class, so their XML has to live somewhere; without this list the
    # generator would create them under `doc/classes/` and the class-reference
    # check would fail on files the module, not the engine, owns.
    return [
        "@Haxe",
        "HaxeLanguage",
        "HaxeScript",
        "HaxeSyntaxHighlighter",
    ]


def get_doc_path():
    return "doc_classes"
