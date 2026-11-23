# Metadata lives here (not [project]) because the oldest supported
# setuptools must still build this. Platform wheel despite no extension
# module: the package ships a prebuilt shared library.
import pathlib
import re

from setuptools import setup
from setuptools.dist import Distribution

HERE = pathlib.Path(__file__).parent

# Read, not import: importing the package pulls in numpy, which need not be
# present to build a wheel.
VERSION = re.search(r'^__version__ = "([^"]+)"',
                    (HERE / "stanli" / "__init__.py").read_text(),
                    re.M).group(1)


class BinaryDistribution(Distribution):
    def has_ext_modules(self):
        return True


try:
    from wheel.bdist_wheel import bdist_wheel as _bdist_wheel

    class bdist_wheel(_bdist_wheel):
        """Platform-specific, but not Python-specific.

        The package ships a shared library loaded through ctypes, so the
        wheel must name the platform it was built for, yet it works on any
        CPython 3: tagging it cp39 would hide it from every other version.
        """

        def finalize_options(self):
            _bdist_wheel.finalize_options(self)
            self.root_is_pure = False

        def get_tag(self):
            _, _, plat = _bdist_wheel.get_tag(self)
            return "py3", "none", plat

    CMDCLASS = {"bdist_wheel": bdist_wheel}
except ImportError:  # building without the wheel package
    CMDCLASS = {}


setup(
    name="stanli",
    version=VERSION,
    description=("Stan Language Interpreter: compile and sample Stan "
                 "models with no C++ toolchain"),
    long_description=(HERE / "README.md").read_text(encoding="utf-8"),
    long_description_content_type="text/markdown",
    author="Sean Talts",
    license="BSD-3-Clause",
    # Copied in by tools/build_wheel.sh; this puts it in .dist-info too.
    license_files=["stanli/LICENSE"],
    url="https://github.com/seantalts/stanli",
    project_urls={
        "Source": "https://github.com/seantalts/stanli",
        "Issues": "https://github.com/seantalts/stanli/issues",
        "Changelog": ("https://github.com/seantalts/stanli/blob/main/"
                      "CHANGELOG.md"),
        "Benchmarks": ("https://github.com/seantalts/stanli/blob/main/"
                       "docs/benchmarks.md"),
        "Model coverage": ("https://github.com/seantalts/stanli/blob/main/"
                           "TESTING.md#every-model"),
    },
    python_requires=">=3.9",
    install_requires=["numpy>=1.22"],
    packages=["stanli"],
    package_data={"stanli": ["_bin/*", "LICENSE",
                         "THIRD_PARTY_LICENSES.md"]},
    include_package_data=True,
    distclass=BinaryDistribution,
    cmdclass=CMDCLASS,
    keywords=["stan", "bayesian", "mcmc", "nuts", "hmc", "statistics",
              "probabilistic-programming", "inference", "autodiff"],
    classifiers=[
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Science/Research",
        "License :: OSI Approved :: BSD License",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3 :: Only",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
        "Programming Language :: Python :: 3.14",
        "Programming Language :: C++",
        "Topic :: Scientific/Engineering :: Mathematics",
        "Topic :: Scientific/Engineering :: Artificial Intelligence",
        "Operating System :: MacOS :: MacOS X",
        "Operating System :: POSIX :: Linux",
    ],
)
