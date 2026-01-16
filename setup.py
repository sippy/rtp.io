from contextlib import suppress
from setuptools import setup, Extension, Command
from setuptools.command.build import build
import subprocess
from os import environ
from sys import path as sys_path
from os.path import realpath, dirname

src_dir = realpath(dirname(__file__))

sys_path.insert(0, src_dir)
from build_tools.CheckVersion import CheckVersion


class CustomCommand(Command):
    def initialize_options(self) -> None:
        self.bdist_dir = None

    def finalize_options(self) -> None:
        with suppress(Exception):
            self.bdist_dir = self.get_finalized_command("bdist_wheel").bdist_dir

    def run(self):
        if self.bdist_dir:
            # Run the external build script
            subprocess.check_call(['sh', 'python/build_rtpproxy.sh'])

class CustomBuild(build):
    sub_commands = [('build_custom', None)] + build.sub_commands

def main():
    link_args = ['-O1', '-g3']
    compile_args = ['-O1', '-g3']
    compile_args.append('-Ibuild/include')
    for x in link_args, compile_args:
        x.append('-flto')
    smap_fname = 'python/symbols.map'
    link_args.append(f'-Wl,--version-script={smap_fname}')
    link_args.extend([
        '-Wl,--whole-archive',
        'rtpproxy/src/.libs/librtpproxy.a',
        '-Wl,--no-whole-archive',
        '-Lbuild/lib',
        '-l:libsrtp2.a', '-l:libssl.a', '-l:libcrypto.a',
    ])
    if 'SRTP_LIBS' in environ:
        link_args.extend(environ['SRTP_LIBS'].split())
    ext_modules = [
        Extension(
            'rtp.io',
            sources=['python/rtpproxy_mod.c'],
            include_dirs=['rtpproxy/src'],  # adjust if headers are elsewhere
            extra_link_args=link_args,
            extra_compile_args=['-std=c99'],
        )
    ]

    with open(src_dir + "/README.md", "r") as fh:
        long_description = fh.read()

    setup(
        name='rtp.io',
        version='1.0.3',
        description='Fast RTPProxy-like RTP handling/processing engine.',
        ext_modules=ext_modules,
        cmdclass={
            'build': CustomBuild,
            'build_custom': CustomCommand,
            'checkversion': CheckVersion,
        },
        classifiers=[
            'Programming Language :: Python :: 3',
            'Programming Language :: C',
        ],
        long_description=long_description,
        long_description_content_type='text/markdown',
    )

if __name__ == '__main__':
    main()
