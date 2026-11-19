"""Functions used to generate source files during build time"""

import os.path

import methods


def make_haxe_support(target, source, env):
    """Packs the Haxe support files into a C++ header.

    Projects need the generated Godot bindings, the Haxe runtime classes and
    the native glue compiled into every project library. Instead of copying
    them next to the executable, they are deflate compressed into the engine
    binary (like the documentation and translation data) so that a Haxe
    project only requires the executable itself.
    """
    dist_root = env.Dir("#modules/haxe/dist").abspath
    entries = []
    for node in source:
        path = node.abspath
        relative = os.path.relpath(path, dist_root).replace(os.sep, "/")
        entries.append((relative, path))
    entries.sort()

    arrays = []
    rows = []
    for index, (relative, path) in enumerate(entries):
        buffer = methods.get_buffer(path)
        uncompressed_size = len(buffer)
        compressed = methods.compress_buffer(buffer)
        arrays.append(
            f"inline constexpr const unsigned char _haxe_support_{index}_compressed[] = {{\n"
            f"{methods.format_buffer(compressed, 1)}\n"
            f"}};"
        )
        rows.append(f'\t{{ "{relative}", _haxe_support_{index}_compressed, {uncompressed_size}, {len(compressed)} }},')

    with methods.generated_wrapper(str(target[0])) as file:
        file.write(
            "/* Haxe support files embedded into the engine binary. */\n\n"
            "struct HaxeSupportFile {\n"
            "\tconst char *path;\n"
            "\tconst unsigned char *data;\n"
            "\tint size;\n"
            "\tint compressed_size;\n"
            "};\n\n"
        )
        file.write("\n".join(arrays))
        file.write("\n\n")
        file.write("inline constexpr HaxeSupportFile _haxe_support_files[] = {\n")
        file.write("\n".join(rows))
        file.write("\n};\n\n")
        file.write(f"inline constexpr const int _haxe_support_file_count = {len(entries)};\n")
