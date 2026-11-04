"""The project's earlier name, for people who update: this is the one place that still uses it."""
import os
from pathlib import Path

OLD_PREFIX, NEW_PREFIX = 'GOW3_', 'WIPEOUT_'
OLD_SETTINGS, NEW_SETTINGS = 'gow3.ini', 'wipeout.ini'


def adopt_environment(env=os.environ):
    """Variables set under the earlier name still count; the new name wins when both are set."""
    for name, value in list(env.items()):
        if name.startswith(OLD_PREFIX):
            env.setdefault(NEW_PREFIX + name[len(OLD_PREFIX):], value)


def adopt_settings(folder):
    """The earlier settings file becomes the new one, when there is no new one yet."""
    old, new = Path(folder) / OLD_SETTINGS, Path(folder) / NEW_SETTINGS
    try:
        if old.is_file() and not new.exists():
            old.rename(new)
    except OSError:
        pass  # a read-only folder: the defaults apply, as on a first start
