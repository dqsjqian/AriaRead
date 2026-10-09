#!/usr/bin/env python3
"""Unified build pipeline for AriaRead (Windows/macOS/Linux).

Complete pipeline: deps -> build -> test -> bench -> package.
Powered by aria_deps.build_kit (pip install aria-deps).

Windows defaults to MSVC x64. MinGW is an explicit alternative.
Python 3.10+ required.

Usage:
    python tools/build.py                 # Full pipeline
    python tools/build.py deps            # Only dependencies
    python tools/build.py --toolchain mingw  # MinGW build on Windows
"""
from __future__ import annotations

import argparse
import os
import platform as plat
import sys
from pathlib import Path

try:
    from aria_deps.build_kit import Pipeline
    _HAS_BUILD_KIT = True
except ImportError:
    _HAS_BUILD_KIT = False
    Pipeline = None

ROOT = Path(__file__).resolve().parents[1]


def extra_args(parser: argparse.ArgumentParser):
    parser.add_argument("--toolchain", choices=("auto", "msvc", "mingw"),
                        default="auto", help="Windows toolchain (auto=MSVC)")
    parser.add_argument("--test", action="store_true", help="Build and run tests")
    parser.add_argument("--offline", action="store_true", help="Offline mode")
    parser.add_argument("--tls-backend", choices=("auto", "openssl", "schannel"),
                        default="auto", help="TLS backend")
    parser.add_argument("--deps-prefix", type=Path,
                        default=os.environ.get("ARIAREAD_DEPS_PREFIX"),
                        help="Dependency install prefix")
    parser.add_argument("--profile", choices=("runtime", "tests"),
                        help="Dependency set (default: runtime, tests with --test)")


def validate(args):
    if plat.system() != "Windows" and args.toolchain != "auto":
        raise ValueError("--toolchain only applies on Windows")


def build_dir_fn(args) -> Path:
    if os.environ.get("ARIAREAD_BUILD_DIR"):
        return Path(os.environ["ARIAREAD_BUILD_DIR"])
    toolchain = args.toolchain
    if toolchain == "auto" and plat.system() == "Windows":
        toolchain = "msvc"
    suffix = f"{toolchain}-{args.config.lower()}"
    return (ROOT / "build" / suffix).resolve()


def deps_list(args) -> list:
    # AriaRead uses aria-deps for dependency management
    # The deps stage is handled by the aria-deps driver
    return []


def cmake_flags(args) -> dict:
    flags = {}
    if args.tls_backend != "auto":
        flags["ARIAREAD_TLS_BACKEND"] = args.tls_backend.upper()
    if args.deps_prefix:
        flags["CMAKE_PREFIX_PATH"] = str(args.deps_prefix)
    flags["ARIAREAD_BUILD_TESTS"] = "ON" if args.test else "OFF"
    return flags


def main(argv=None) -> int:
    if not _HAS_BUILD_KIT:
        print("Error: aria-deps is required. Install it with:", file=sys.stderr)
        print("    pip install aria-deps", file=sys.stderr)
        return 1
    pipeline = Pipeline(
        name="aria-read",
        root=ROOT,
        deps=deps_list,
        cmake_flags=cmake_flags,
        extra_args=extra_args,
        build_dir_fn=build_dir_fn,
        validate_fn=validate,
    )
    return pipeline.run(argv)


if __name__ == "__main__":
    sys.exit(main())
