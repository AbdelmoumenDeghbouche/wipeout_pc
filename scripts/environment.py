"""Canonical port environment; legacy inputs remain valid, including empty values."""
import os
from pathlib import Path


def data_directory(env):
    """Packaged Linux data stays in an existing directory, with the new name preferred."""
    explicit = getenv('WIPEOUT_OC_DATA_DIR', env=env)
    if explicit is not None:
        return Path(explicit)
    base = Path(env.get('XDG_DATA_HOME') or Path.home() / '.local/share')
    canonical, legacy = base / 'wipeout_oc', base / 'gow3'
    return canonical if canonical.is_dir() or not legacy.is_dir() else legacy


def _names(name):
    suffix = name.removeprefix('WIPEOUT_OC_').removeprefix('GOW3_')
    return 'WIPEOUT_OC_' + suffix, 'GOW3_' + suffix


def getenv(name, default=None, env=None):
    env = os.environ if env is None else env
    current, legacy = _names(name)
    return env[current] if current in env else env.get(legacy, default)


def normalize(env):
    """Consume legacy names once so a later unset cannot revive an inherited value."""
    for name in tuple(env):
        if name.startswith('GOW3_'):
            current, _ = _names(name)
            env.setdefault(current, env[name])
            del env[name]
    return env


def clear_derived(env):
    """Remove only recorded values; an unlisted alias can be an explicit override."""
    marker = getenv('DERIVED', '', env)
    for name in _names('DERIVED'):
        env.pop(name, None)
    for name in marker.split(','):
        if name.startswith(('GOW3_', 'WIPEOUT_OC_')):
            env.pop(name, None)
