"""Route the `ar` archiving command through a response file.

`env.Library()` on the generated godot-cpp bindings builds a command line whose
single argument is larger than Linux's `MAX_ARG_STRLEN` (128 KiB), so the
archive step dies with `sh: Argument list too long`. SCons already handles this
for the final link: its `LINKCOM` wraps the command in `$TEMPFILE`, which writes
the argument list to a file and passes `@file`. Its `ar` tool does not, so the
binding archive was the one command that could not fit.

`site_init.py` is loaded before the SConstruct applies its tools, so patching
`SCons.Tool.ar.generate` here reaches every environment the build creates. Only
`ARCOM` changes, and SCons only switches to the response file when the command
line is actually over the limit, so shorter archives keep the plain command.
"""

import SCons.Tool.ar

_original_generate = SCons.Tool.ar.generate


def generate(env) -> None:
    _original_generate(env)
    env["ARCOM"] = "${TEMPFILE('$AR $ARFLAGS $TARGET $SOURCES', '$ARCOMSTR')}"


SCons.Tool.ar.generate = generate
