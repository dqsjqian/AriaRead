#!/usr/bin/env python3
"""Print a GitHub Actions cache scope for the actual host compiler and SDK.

Dependency versions/recipes are appended by the workflow. A restore key can
therefore reuse unchanged components across dependency updates. The builder
still verifies every restored file and never trusts this key as validation.
"""
import os
from pathlib import Path
import subprocess

import dependency_cache


if __name__ == '__main__':
    context = dependency_cache.build_context(Path.cwd() / 'build/deps')
    context['cmake'] = subprocess.check_output(['cmake', '--version'], text=True)
    context['runner_image'] = os.environ.get('ImageVersion', '')
    print('key=' + dependency_cache.fingerprint(context))
