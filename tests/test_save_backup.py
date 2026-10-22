import datetime
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import paths  # noqa: F401  (puts scripts/ on sys.path)
import save_backup


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding='utf-8')


class SaveBackupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.user = Path(self.temp.name)
        self.save = self.user / 'savedata' / '1' / 'CUSA01623' / 'AUTOSAVE' / 'data.bin'
        self.clock = datetime.datetime(2026, 10, 9, 18, 0, 0)

    def tearDown(self):
        self.temp.cleanup()

    def tick(self):
        self.clock += datetime.timedelta(minutes=1)
        return self.clock

    def test_copies_savedata(self):
        write(self.save, 'v1')
        made = save_backup.backup(self.user, now=self.tick())
        self.assertEqual(made.name, '20261009-180100')
        self.assertEqual((made / self.save.relative_to(self.user / 'savedata')).read_text(), 'v1')

    def test_no_backup_without_saves(self):
        self.assertIsNone(save_backup.backup(self.user, now=self.tick()))
        (self.user / 'savedata').mkdir()
        self.assertIsNone(save_backup.backup(self.user, now=self.tick()))
        self.assertEqual(save_backup.list_backups(self.user), [])

    def test_same_second_gets_unique_name(self):
        write(self.save, 'v1')
        first = save_backup.backup(self.user, now=self.clock)
        second = save_backup.backup(self.user, now=self.clock)
        self.assertNotEqual(first, second)
        self.assertEqual(len(save_backup.list_backups(self.user)), 2)

    def test_same_second_backups_sort_by_creation_sequence(self):
        write(self.save, 'v1')
        made = [save_backup.backup(self.user, now=self.clock, keep=20) for _ in range(12)]
        self.assertEqual(save_backup.list_backups(self.user), [p.name for p in reversed(made)])

    def test_same_second_retention_keeps_latest_contents_and_unique_names(self):
        made = []
        for index in range(14):
            write(self.save, f'version {index}')
            made.append(save_backup.backup(self.user, now=self.clock))
            self.assertTrue(made[-1].is_dir(), 'the backup just created must survive pruning')
        self.assertEqual(len(set(made)), len(made))
        expected = list(reversed(made[-save_backup.KEEP:]))
        self.assertEqual(save_backup.list_backups(self.user), [p.name for p in expected])
        relative = self.save.relative_to(self.user / 'savedata')
        self.assertEqual([(p / relative).read_text() for p in expected],
                         [f'version {i}' for i in range(13, 3, -1)])

    def test_keeps_newest_ten(self):
        write(self.save, 'v')
        made = [save_backup.backup(self.user, now=self.tick()) for _ in range(12)]
        kept = save_backup.list_backups(self.user)
        self.assertEqual(len(kept), save_backup.KEEP)
        self.assertEqual(kept[0], made[-1].name)  # newest first
        self.assertFalse(made[0].exists())
        self.assertFalse(made[1].exists())

    def test_same_second_restore_keeps_the_safety_backup(self):
        for index in range(12):
            write(self.save, f'version {index}')
            save_backup.backup(self.user, now=self.clock)
        oldest = save_backup.list_backups(self.user)[-1]
        write(self.save, 'current progress')
        safety = save_backup.restore(self.user, oldest, now=self.clock)
        relative = self.save.relative_to(self.user / 'savedata')
        self.assertEqual(self.save.read_text(), 'version 2')
        self.assertEqual((safety / relative).read_text(), 'current progress')
        self.assertEqual(save_backup.list_backups(self.user)[0], safety.name)
        self.assertEqual(len(save_backup.list_backups(self.user)), save_backup.KEEP)

    def test_restore_backs_up_current_first(self):
        write(self.save, 'old')
        old = save_backup.backup(self.user, now=self.tick())
        write(self.save, 'new')
        write(self.save.parent / 'extra.bin', 'x')
        safety = save_backup.restore(self.user, old.name, now=self.tick())
        self.assertEqual(self.save.read_text(), 'old')
        self.assertFalse((self.save.parent / 'extra.bin').exists())
        self.assertEqual((safety / self.save.relative_to(self.user / 'savedata')).read_text(), 'new')

    def test_restore_oldest_when_full(self):
        write(self.save, 'oldest')
        oldest = save_backup.backup(self.user, now=self.tick())
        write(self.save, 'later')
        for _ in range(save_backup.KEEP - 1):
            save_backup.backup(self.user, now=self.tick())
        save_backup.restore(self.user, oldest.name, now=self.tick())
        self.assertEqual(self.save.read_text(), 'oldest')
        self.assertEqual(len(save_backup.list_backups(self.user)), save_backup.KEEP)

    def test_restore_rejects_unknown_or_unsafe_names(self):
        write(self.save, 'v')
        for name in ('missing', '..', '../savedata', ''):
            with self.assertRaises(ValueError):
                save_backup.restore(self.user, name, now=self.tick())
        self.assertEqual(self.save.read_text(), 'v')

    def test_failed_copy_does_not_publish_or_prune(self):
        write(self.save, 'old')
        for _ in range(save_backup.KEEP):
            save_backup.backup(self.user, now=self.tick())
        previous = save_backup.list_backups(self.user)
        write(self.save, 'current')
        extra = self.save.parent / 'extra.bin'
        write(extra, 'extra')

        for error in (OSError('disk full'), KeyboardInterrupt()):
            with self.subTest(error=type(error).__name__):
                def interrupted(source, destination, **kwargs):
                    write(Path(destination) / 'partial.bin', 'incomplete')
                    raise error

                with mock.patch.object(save_backup.shutil, 'copytree', side_effect=interrupted):
                    with self.assertRaises(type(error)) as caught:
                        save_backup.backup(self.user, now=self.tick())
                self.assertIs(caught.exception, error)
                self.assertEqual(save_backup.list_backups(self.user), previous)
                self.assertEqual(sorted(p.name for p in save_backup.backups_dir(self.user).iterdir()),
                                 sorted(previous))
                self.assertEqual(self.save.read_text(), 'current')
                self.assertEqual(extra.read_text(), 'extra')

    def test_copy_is_not_listed_until_complete(self):
        write(self.save, 'old')
        old = save_backup.backup(self.user, now=self.tick())
        write(self.save, 'new')
        copytree = save_backup.shutil.copytree

        def copying(source, destination, *args, **kwargs):
            self.assertEqual(save_backup.list_backups(self.user), [old.name])
            result = copytree(source, destination, *args, **kwargs)
            self.assertEqual(save_backup.list_backups(self.user), [old.name])
            return result

        with mock.patch.object(save_backup.shutil, 'copytree', side_effect=copying):
            made = save_backup.backup(self.user, now=self.tick())
        self.assertEqual(save_backup.list_backups(self.user), [made.name, old.name])
        relative = self.save.relative_to(self.user / 'savedata')
        self.assertEqual((made / relative).read_text(), 'new')

    def test_abandoned_copy_is_not_listed_or_restored(self):
        write(self.save, 'current')
        pending = save_backup.backups_dir(self.user) / '.backup-pending-abandoned'
        write(pending / 'partial.bin', 'incomplete')
        self.assertEqual(save_backup.list_backups(self.user), [])
        with self.assertRaises(ValueError):
            save_backup.restore(self.user, pending.name, now=self.tick())
        self.assertEqual(self.save.read_text(), 'current')
        save_backup.backup(self.user, now=self.tick(), keep=1)
        self.assertTrue((pending / 'partial.bin').is_file())

    def test_failed_publication_preserves_previous_backup(self):
        write(self.save, 'old')
        old = save_backup.backup(self.user, now=self.tick())
        write(self.save, 'current')
        error = PermissionError('cannot publish backup')
        with mock.patch.object(Path, 'rename', side_effect=error):
            with self.assertRaises(PermissionError) as caught:
                save_backup.backup(self.user, now=self.tick(), keep=1)
        self.assertIs(caught.exception, error)
        self.assertEqual(save_backup.list_backups(self.user), [old.name])
        self.assertEqual(list(save_backup.backups_dir(self.user).iterdir()), [old])
        self.assertEqual(self.save.read_text(), 'current')

    def test_failed_safety_backup_does_not_restore(self):
        write(self.save, 'old')
        old = save_backup.backup(self.user, now=self.tick())
        write(self.save, 'current')

        def interrupted(source, destination, **kwargs):
            write(Path(destination) / 'partial.bin', 'incomplete')
            raise OSError('disk full')

        with mock.patch.object(save_backup.shutil, 'copytree', side_effect=interrupted):
            with self.assertRaises(OSError):
                save_backup.restore(self.user, old.name, now=self.tick())
        self.assertEqual(self.save.read_text(), 'current')
        self.assertEqual(save_backup.list_backups(self.user), [old.name])

    def test_legacy_backup_without_marker_is_restorable(self):
        write(self.save, 'current')
        legacy = save_backup.backups_dir(self.user) / '20251009-180000'
        relative = self.save.relative_to(self.user / 'savedata')
        write(legacy / relative, 'legacy')
        self.assertEqual(save_backup.list_backups(self.user), [legacy.name])
        safety = save_backup.restore(self.user, legacy.name, now=self.tick())
        self.assertEqual(self.save.read_text(), 'legacy')
        self.assertEqual((safety / relative).read_text(), 'current')


if __name__ == '__main__':
    unittest.main()
