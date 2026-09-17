"""
This script processes the output from the C preprocessor and extracts all
qstr. Each qstr is transformed into a qstr definition of the form 'Q(...)'.

This script works with Python 3.3+.
"""

import io
import os
import re
import subprocess
import tempfile
import sys
import multiprocessing, multiprocessing.dummy


# Extract MP_QSTR_FOO macros.
_MODE_QSTR = "qstr"

# Extract MP_COMPRESSED_ROM_TEXT("") macros.  (Which come from MP_ERROR_TEXT)
_MODE_COMPRESS = "compress"

# Extract MP_REGISTER_(EXTENSIBLE_)MODULE(...) macros.
_MODE_MODULE = "module"

# Extract MP_REGISTER_ROOT_POINTER(...) macros.
_MODE_ROOT_POINTER = "root_pointer"


def expand_response_files(arguments):
    # An argument of the form "@file" stands for the lines of that file.  Windows
    # caps a command line at 8191 characters and the QSTR source list plus the
    # preprocessor flags run well past that; the excess is dropped silently rather
    # than reported, which then surfaces as an empty output file much later on.
    expanded = []
    for argument in arguments:
        if argument.startswith("@"):
            with io.open(argument[1:], encoding="utf-8") as f:
                expanded.extend(line.strip() for line in f if line.strip())
        else:
            expanded.append(argument)
    return expanded


def command_with_response_file(cmd):
    """Return (cmd, rsp_path), routing through a response file when needed.

    Windows caps a command line at 32767 characters and ESP-IDF's include list
    alone can exceed it; CreateProcess then fails with WinError 206, naming
    neither the limit nor the command. gcc reads options from a response file.

    A response file has its own quoting -- backslash escapes, quotes group -- so
    every argument is escaped and wrapped. Both matter: Windows paths are full of
    backslashes, and defines like -DFFCONF_H="..." carry quotes that belong to
    the macro value and are otherwise eaten.
    """
    if sum(len(a) + 1 for a in cmd) <= 30000:
        return cmd, None
    fd, path = tempfile.mkstemp(suffix=".rsp", text=True)
    bs, q = chr(92), chr(34)
    with os.fdopen(fd, "w") as rsp:
        rsp.write(chr(10).join(
            q + a.replace(bs, bs + bs).replace(q, bs + q) + q for a in cmd[1:]))
    return [cmd[0], "@" + path], path


class PreprocessorError(Exception):
    pass


def is_c_source(fname):
    return os.path.splitext(fname)[1] in [".c"]


def is_cxx_source(fname):
    return os.path.splitext(fname)[1] in [".cc", ".cp", ".cxx", ".cpp", ".CPP", ".c++", ".C"]


def preprocess():
    if any(src in args.dependencies for src in args.changed_sources):
        sources = args.sources
    elif any(args.changed_sources):
        sources = args.changed_sources
    else:
        sources = args.sources
    csources = []
    cxxsources = []
    for source in sources:
        if is_cxx_source(source):
            cxxsources.append(source)
        elif is_c_source(source):
            csources.append(source)
    if not csources and not cxxsources:
        # Never write an empty output: it is not a valid build state, and left
        # silent it fails several steps later with nothing pointing back here.
        raise PreprocessorError("no sources to preprocess")

    try:
        os.makedirs(os.path.dirname(args.output[0]))
    except OSError:
        pass

    # These regex's are used to filter the preprocessed data, keeping only those lines
    # that are subsequently needed by the `process_file` step.  The regexs are kept
    # short so they are as efficient as possible. (The stm32 port needs symbols of the
    # form `micropy_hw_xxx` so they are also kept.)
    re_line_file = re.compile(rb"^#(?:line)?\s+\d+\s\"")
    re_mp_info = re.compile(rb"MP_COMP|MP_QSTR|MP_REGI|micropy_hw")

    def pp(flags):
        def run(files):
            rsp_path = None
            try:
                filtered_lines = []
                cmd = args.pp + flags + files
                # Windows caps a command line at 32767 characters, and the
                # ESP-IDF include list on its own is long enough to exceed it.
                # CreateProcess then fails with WinError 206, which names
                # neither the limit nor the command.  gcc reads options from a
                # response file, so hand it one once the command grows.
                cmd, rsp_path = command_with_response_file(cmd)
                with subprocess.Popen(cmd, stdout=subprocess.PIPE) as proc:
                    recent_file = None
                    for line in proc.stdout:
                        if line.isspace():
                            pass
                        elif re_line_file.match(line):
                            recent_file = line
                        elif re_mp_info.search(line):
                            if recent_file:
                                filtered_lines.append(recent_file)
                                recent_file = None
                            filtered_lines.append(line)
                    proc.wait()
                    if proc.returncode:
                        raise PreprocessorError("command failed: " + " ".join(cmd))
                return b"".join(filtered_lines)
            except subprocess.CalledProcessError as er:
                raise PreprocessorError(str(er))
            finally:
                if rsp_path is not None:
                    try:
                        os.unlink(rsp_path)
                    except OSError:
                        pass

        return run

    try:
        cpus = multiprocessing.cpu_count()
    except NotImplementedError:
        cpus = 1
    # Close the pool deterministically.  Left to the garbage collector it is
    # finalised during interpreter shutdown, by which point its notifier handle
    # is already gone -- on Python 3.14 that raises OSError out of Pool.__del__
    # and fails the build, having merely warned on older versions.
    with multiprocessing.dummy.Pool(cpus) as p, open(args.output[0], "wb") as out_file:
        for flags, sources in (
            (args.cflags, csources),
            (args.cxxflags, cxxsources),
        ):
            batch_size = (len(sources) + cpus - 1) // cpus
            chunks = [sources[i : i + batch_size] for i in range(0, len(sources), batch_size or 1)]
            for output in p.imap(pp(flags), chunks):
                out_file.write(output)


def preprocess_qstrdefs():
    r"""Build qstrdefs.preprocessed.h without a POSIX shell.

    Replaces the pipeline

        cat inputs | sed 's/^Q(.*)/"&"/' | cc -E flags - | sed 's/^"\(Q(.*)\)"/\1/'

    The quoting protects Q(...) entries from the preprocessor and is undone
    afterwards.  As a shell pipeline it needs cat, sed and a shell that can parse
    it: fine under make, but CMake's Ninja generator runs custom commands through
    cmd.exe, where it fails with "sed: unterminated `s' command".  Ninja is the
    only generator ESP-IDF accepts, so this had to stop being a pipeline.
    """
    q_line = re.compile(rb"(?m)^(Q\(.*\))")
    data = b"".join(io.open(f, "rb").read() for f in args.input)
    data = q_line.sub(rb'"\1"', data)

    cmd, rsp_path = command_with_response_file(args.pp + args.cflags + ["-"])
    try:
        proc = subprocess.run(cmd, input=data, stdout=subprocess.PIPE)
    finally:
        if rsp_path is not None:
            try:
                os.unlink(rsp_path)
            except OSError:
                pass
    if proc.returncode:
        raise PreprocessorError("preprocessor failed on qstrdefs")

    quoted = re.compile(rb'(?m)^"(Q\(.*\))"')
    with open(args.output[0], "wb") as out_file:
        out_file.write(quoted.sub(rb"\1", proc.stdout))


def write_out(fname, output):
    if output:
        for m, r in [("/", "__"), ("\\", "__"), (":", "@"), ("..", "@@")]:
            fname = fname.replace(m, r)
        with open(args.output_dir + "/" + fname + "." + args.mode, "w") as f:
            f.write("\n".join(output) + "\n")


def process_file(f):
    # match gcc-like output (# n "file") and msvc-like output (#line n "file")
    re_line = re.compile(r"^#(?:line)?\s+\d+\s\"([^\"]+)\"")
    if args.mode == _MODE_QSTR:
        re_match = re.compile(r"MP_QSTR_[_a-zA-Z0-9]+")
    elif args.mode == _MODE_COMPRESS:
        re_match = re.compile(r'MP_COMPRESSED_ROM_TEXT\("([^"]*)"\)')
    elif args.mode == _MODE_MODULE:
        re_match = re.compile(
            r"(?:MP_REGISTER_MODULE|MP_REGISTER_EXTENSIBLE_MODULE|MP_REGISTER_MODULE_DELEGATION)\(.*?,\s*.*?\);"
        )
    elif args.mode == _MODE_ROOT_POINTER:
        re_match = re.compile(r"MP_REGISTER_ROOT_POINTER\(.*?\);")
    output = []
    last_fname = None
    for line in f:
        if line.isspace():
            continue
        m = re_line.match(line)
        if m:
            fname = m.group(1)
            if not is_c_source(fname) and not is_cxx_source(fname):
                continue
            if fname != last_fname:
                write_out(last_fname, output)
                output = []
                last_fname = fname
            continue
        for match in re_match.findall(line):
            if args.mode == _MODE_QSTR:
                name = match.replace("MP_QSTR_", "")
                output.append("Q(" + name + ")")
            elif args.mode in (_MODE_COMPRESS, _MODE_MODULE, _MODE_ROOT_POINTER):
                output.append(match)

    if last_fname:
        write_out(last_fname, output)
    return ""


def cat_together():
    import glob
    import hashlib

    hasher = hashlib.md5()
    all_lines = []
    for fname in glob.glob(args.output_dir + "/*." + args.mode):
        with open(fname, "rb") as f:
            lines = f.readlines()
            all_lines += lines
    all_lines.sort()
    all_lines = b"\n".join(all_lines)
    hasher.update(all_lines)
    new_hash = hasher.hexdigest()
    # print(new_hash)
    old_hash = None
    try:
        with open(args.output_file + ".hash") as f:
            old_hash = f.read()
    except IOError:
        pass
    mode_full = "QSTR"
    if args.mode == _MODE_COMPRESS:
        mode_full = "Compressed data"
    elif args.mode == _MODE_MODULE:
        mode_full = "Module registrations"
    elif args.mode == _MODE_ROOT_POINTER:
        mode_full = "Root pointer registrations"
    if old_hash != new_hash or not os.path.exists(args.output_file):
        print(mode_full, "updated")

        with open(args.output_file, "wb") as outf:
            outf.write(all_lines)
        with open(args.output_file + ".hash", "w") as f:
            f.write(new_hash)
    else:
        print(mode_full, "not updated")


if __name__ == "__main__":
    if len(sys.argv) < 6:
        print("usage: %s command mode input_filename output_dir output_file" % sys.argv[0])
        sys.exit(2)

    class Args:
        pass

    args = Args()
    args.command = sys.argv[1]

    if args.command == "qstrdefs":
        named = {s: [] for s in ["qstrdefs", "pp", "output", "cflags", "input"]}
        current_tok = "qstrdefs"
        for arg in sys.argv[1:]:
            if arg in named:
                current_tok = arg
            else:
                named[current_tok].append(arg)
        for k, v in named.items():
            setattr(args, k, expand_response_files(v))
        try:
            preprocess_qstrdefs()
        except PreprocessorError as er:
            print(er)
            sys.exit(1)
        sys.exit(0)

    if args.command == "pp":
        named_args = {
            s: []
            for s in [
                "pp",
                "output",
                "cflags",
                "cxxflags",
                "sources",
                "changed_sources",
                "dependencies",
            ]
        }

        for arg in sys.argv[1:]:
            if arg in named_args:
                current_tok = arg
            else:
                named_args[current_tok].append(arg)

        if not named_args["pp"] or len(named_args["output"]) != 1:
            print("usage: %s %s ..." % (sys.argv[0], " ... ".join(named_args)))
            sys.exit(2)

        for k, v in named_args.items():
            setattr(args, k, expand_response_files(v))

        try:
            preprocess()
        except PreprocessorError as er:
            print(er)
            sys.exit(1)

        sys.exit(0)

    args.mode = sys.argv[2]
    args.input_filename = sys.argv[3]  # Unused for command=cat
    args.output_dir = sys.argv[4]
    args.output_file = None if len(sys.argv) == 5 else sys.argv[5]  # Unused for command=split

    if args.mode not in (_MODE_QSTR, _MODE_COMPRESS, _MODE_MODULE, _MODE_ROOT_POINTER):
        print("error: mode %s unrecognised" % sys.argv[2])
        sys.exit(2)

    try:
        os.makedirs(args.output_dir)
    except OSError:
        pass

    if args.command == "split":
        with io.open(args.input_filename, encoding="utf-8") as infile:
            process_file(infile)

    if args.command == "cat":
        cat_together()
