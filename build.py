
import re
import os
import sys
import glob
import pickle
import hashlib
import subprocess


def run(command):
    if type(command) in (list, tuple):
        command = " ".join([str(i) for i in command])
    assert(type(command) == str)

    proc = subprocess.run(command, stdout=sys.stdout, stderr=sys.stderr, shell=True)
    return proc.returncode != 0


SHADER_TYPE = re.compile(r'^.*\.(?P<type>.+)\.glsl$')
HEADER_TYPE = re.compile(r'^.*\.(h|hpp|inl)$')
SOURCE_TYPE = re.compile(r'^.*\.cpp$')


def file_type(path):
    match = SHADER_TYPE.match(path)
    if match:
        subtype = match.groupdict().get("type")
        return "glsl", subtype

    match = HEADER_TYPE.match(path)
    if match:
        return "header", None

    match = SOURCE_TYPE.match(path)
    if match:
        return "c++", None

    return "unknown", None


BUILD_PREFIX = re.compile(r'^scratch/')
SRC_PREFIX = re.compile(r'^src/')

SPIRV_SUFFIX = re.compile(r'\.spirv$')
GLSL_SUFFIX = re.compile(r'\.glsl$')
CPP_SUFFIX = re.compile(r'\.cpp$')


def graph_search(graph, name):
    name = BUILD_PREFIX.sub('', SPIRV_SUFFIX.sub('.glsl', name))
    query = re.compile(f'^src/(.+/)*{name}$')
    for path, node in graph.items():
        if query.match(path):
            return node
    return None


INCLUDES = re.compile(r'^\s*#include\s*\"(.+)\"\s*$', re.M)

GLSL_COMMON = " ".join([
    "glslangValidator",
    "--target-env spirv1.3",
    "--enhanced-msgs",
    "--nan-clamp",
    "-V",
    "-e main",
    "-x",
])

CPP_COMMON = " ".join([
    "clang++",
    "-c",
    "-std=c++2c",
    "-DGROUP_SIZE=8",
    f"-I{os.path.abspath("scratch/..")}/",
    "-I/usr/include/pipewire-0.3",
    "-I/usr/include/spa-0.2",
])

LINK_COMMON = " ".join([
    "clang++",
    "-std=c++2c",
    "-lvulkan",
    "-lSDL3",
    "-lpipewire-0.3",
    "-flto",
])


class FileInfo:
    def __init__(self, path):
        assert(os.path.isfile(path))

        self.path = path
        self.type = None
        self.subtype = None
        self.modified = None
        self.hash = None
        self.includes = []
        self.partial = True
        self.dirty = False
        self.artifact = None

        folder, name = os.path.split(path)
        self.type, self.subtype = file_type(name)

        self.file_time = os.path.getmtime(path)

        with open(path, "rb") as f:
            self.hash = hashlib.file_digest(f, "md5").digest()

        if self.type in ("glsl", "header", "c++"):
            with open(path, "r") as f:
                text = f.read()
                self.includes = INCLUDES.findall(text)

        if self.type == "glsl":
            self.artifact = SRC_PREFIX.sub('scratch/', GLSL_SUFFIX.sub('.spirv', path))

        elif self.type == "c++":
            self.artifact = SRC_PREFIX.sub('scratch/', CPP_SUFFIX.sub('.o', path))

    def populate(self, graph, journal, sequence):
        if self.partial:
            self.partial = False

            partials = self.includes
            self.includes = []
            for partial in partials:
                if node := graph_search(graph, partial):
                    self.includes.append(node)

            if not self.dirty:
                for node in self.includes:
                    dirty, sequence = node.populate(graph, journal, sequence)
                    self.dirty = self.dirty or dirty

            if not self.dirty:
                if old := journal.get(self.path):
                    self.dirty = (old.modified != self.modified or old.hash != self.hash)
                else:
                    self.dirty = True

            if self.artifact and not os.path.isfile(self.artifact):
                self.dirty = True

            if self.type in ("glsl", "c++"):
                sequence.append(self)

        return self.dirty, sequence


    def build_glsl(self):
        command = GLSL_COMMON

        if self.subtype == "cs":
            command += " -S comp"
            command += " -DGROUP_SIZE=8"
        else:
            print(f"\tUnsupported shader subtype: {self.subtype}")
            return True, None

        command += f" -o {self.artifact} {self.path}"

        return run(command), command


    def build_cpp(self):
        command = CPP_COMMON
        command += f" -o {self.artifact} {self.path}"
        print(command)
        return run(command), command


    def build(self):
        assert(self.dirty)

        print(f"{'_' * 79}")
        print(f"Compiling {self.path}...\n")

        error = None
        command = None
        to_link = []

        if self.type == "glsl":
            error, command = self.build_glsl()

        elif self.type == "c++":
            error, command = self.build_cpp()

        else:
            print(f"\nUnsupported source file type: {self.type}\n")
            return True

        if error and command:
            print(f"\ncommand failed with error:\n\n{command}\n")

        return error


def analyze():
    graph = {}
    journal = {}
    if os.path.isfile(".journal"):
        with open(".journal", "rb") as f:
            journal = pickle.load(f)

    src_root = re.compile(r'^src/')
    cpp_sources = []

    names = set()

    for path in glob.glob("src/**"):
        if os.path.isfile(path):
            folder, name = os.path.split(path)
            assert(name not in names)
            names.add(name)

            info = FileInfo(path)
            graph[path] = info
            if info.type == "c++":
                cpp_sources.append(info)

    glsl_sequence = []
    cpp_sequence = []

    for translation_unit in cpp_sources:
        dirty, sequence = translation_unit.populate(graph, journal, [])
        for node in sequence:
            if node.type == "glsl" and node.dirty:
                glsl_sequence.append(node)
            elif node.type == "c++" and node.dirty:
                cpp_sequence.append(node)

    trim = set()
    for path, node in graph.items():
        if node.partial:
            trim.add(path)
    for path in trim:
        graph.pop(path)

    return graph, glsl_sequence + cpp_sequence


if __name__ == "__main__":
    if not os.path.isdir("scratch"):
        assert(not os.path.exists("scratch"))
        os.mkdir("scratch")

    graph, sequence = analyze()
    error = False

    if not sequence:
        print("everything is normal")

    for node in sequence:
        error = error or node.build()
        if error:
            graph.pop(node.path)

    if error:
        print("build failed :(")
    elif sequence:
        object_files = []
        for node in graph.values():
            if node.type == "c++":
                assert(os.path.isfile(node.artifact))
                object_files.append(node.artifact)
        assert(len(object_files) > 0)
        print(f"{'_' * 79}")
        print(f"Linking convolver...\n")
        command = LINK_COMMON + " " + " ".join(object_files) + " -o convolver"
        if error := run(command):
            print(f"\ncommand failed with error:\n\n{command}\n")
        else:
            print("... done!")

    with open(".journal", "wb") as f:
        pickle.dump(graph, f)
