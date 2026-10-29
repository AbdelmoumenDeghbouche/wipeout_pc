# SPDX-License-Identifier: GPL-2.0-or-later
"""Select the settings file and copy legacy bytes once, without replacing a newer file."""
import os
from pathlib import Path
import shutil
import tempfile

from environment import getenv


def absolute(path):
    # Preserve symlinks and '..': collapsing either can select a different file.
    return Path(path).absolute()


def resolve_config(data_dir, env=None):
    override = getenv('WIPEOUT_OC_CONFIG', env=env)
    if override:
        return absolute(override)
    data = absolute(data_dir)
    destination, source = data / 'wipeout_oc.ini', data / 'gow3.ini'
    try:
        destination.stat()
        return destination
    except FileNotFoundError:
        pass
    temporary = None
    try:
        try:
            old = source.open('rb')
        except FileNotFoundError:
            return destination
        # tempfile normalizes '..'; anchor only its directory to the physical parent.
        with old, tempfile.NamedTemporaryFile(dir=os.path.realpath(data), prefix='.wipeout_oc-', suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            shutil.copyfileobj(old, stream)
            stream.flush()
            os.fsync(stream.fileno())
        try:
            if os.name == 'nt':
                os.rename(temporary, destination)  # Windows refuses an existing destination.
            else:
                os.link(temporary, destination)  # Publish the complete temp inode, never the old INI.
        except FileExistsError:
            pass  # A separately created new file wins, including during migration.
        return destination
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
