"""The Online switch: what the game gets, and PLAY's way there."""
import queue
import unittest
from unittest import mock

from test_launcher_controls import launcher

oh = launcher.online_host


def env(**settings):
    return launcher.game_environment({**launcher.APP_DEFAULTS, 'game_dir': 'fixture', **settings})


class Environment(unittest.TestCase):
    def test_off_is_offline_whatever_the_rest_says(self):
        e = env(online_role='join', online_code=oh.encode_code('1.1.1.1'), player_name='Bob Smith')
        for key in ('WIPEOUT_NET_SERVER', 'WIPEOUT_NET_HOST', 'WIPEOUT_NET_PORT'):
            self.assertNotIn(key, e)
        self.assertEqual(e['WIPEOUT_USER_NAME'], 'Bob Smith')

    def test_host(self):
        e = env(online_enabled=True, online_role='host', online_host_port=7790, player_name='Bob Smith')
        self.assertEqual((e['WIPEOUT_NET_HOST'], e['WIPEOUT_NET_PORT'], e['WIPEOUT_USER_NAME']), ('1', '7790', 'Bob_Smith'))
        self.assertNotIn('WIPEOUT_NET_SERVER', e)

    def test_join(self):
        e = env(online_enabled=True, online_role='join', online_address='192.168.1.2:7788')
        self.assertEqual(e['WIPEOUT_NET_SERVER'], '192.168.1.2:7788')
        self.assertNotIn('WIPEOUT_NET_HOST', e)
        self.assertRegex(e['WIPEOUT_USER_NAME'], r'^Player-[0-9a-f]{4}$')

    def test_join_without_probe_uses_the_public_address(self):
        code = oh.encode_code('1.1.1.1', 7801, '192.168.1.2')
        e = env(online_enabled=True, online_role='join', online_code=code)
        self.assertEqual(e['WIPEOUT_NET_SERVER'], '1.1.1.1:7801')

    def test_bad_code_connects_nowhere(self):
        self.assertNotIn('WIPEOUT_NET_SERVER', env(online_enabled=True, online_role='join', online_code='nope'))


class Play(unittest.TestCase):
    def owner(self, **settings):
        owner = launcher.Launcher.__new__(launcher.Launcher)
        owner.root, owner.status, owner.play_button = mock.Mock(), mock.Mock(), mock.Mock()
        owner.messagebox = mock.Mock()
        owner.ui_calls = queue.Queue()
        owner.vars = {}
        owner.refresh_status, owner.launch_game, owner.show = mock.Mock(), mock.Mock(), mock.Mock()
        owner.app = {**launcher.APP_DEFAULTS, 'online_enabled': True, **settings}
        return owner

    def run_work(self, owner):
        with mock.patch.object(launcher.threading, 'Thread') as thread:
            owner.prepare_online()
        if thread.called:
            thread.call_args.kwargs['target']()
            while not owner.ui_calls.empty():
                owner.ui_calls.get_nowait()()

    def test_bad_code_goes_back_to_the_page(self):
        owner = self.owner(online_role='join', online_code='oops')
        self.run_work(owner)
        owner.messagebox.showerror.assert_called_once()
        owner.show.assert_called_once_with('online')
        owner.launch_game.assert_not_called()

    def test_join_uses_what_answers(self):
        owner = self.owner(online_role='join', online_code=oh.encode_code('1.1.1.1', 7788, '192.168.1.2'))
        with mock.patch.object(oh, 'reachable_address', return_value='192.168.1.2:7788'):
            self.run_work(owner)
        self.assertEqual(owner.launch_game.call_args.args[0]['online_address'], '192.168.1.2:7788')
        owner.messagebox.askyesno.assert_not_called()

    def test_silent_host_asks_first(self):
        owner = self.owner(online_role='join', online_code=oh.encode_code('1.1.1.1'))
        owner.messagebox.askyesno.return_value = False
        with mock.patch.object(oh, 'reachable_address', return_value=None):
            self.run_work(owner)
        owner.launch_game.assert_not_called()
        owner.messagebox.askyesno.return_value = True
        with mock.patch.object(oh, 'reachable_address', return_value=None):
            self.run_work(owner)
        self.assertEqual(owner.launch_game.call_args.args[0]['online_address'], '1.1.1.1:7788')

    def test_host_passes_the_port_it_got(self):
        owner = self.owner(online_role='host', online_port='7788')
        session = mock.Mock(port=7790)
        session.start.return_value = session
        with mock.patch.object(oh, 'HostSession', return_value=session):
            self.run_work(owner)
        settings, passed = owner.launch_game.call_args.args
        self.assertEqual((settings['online_host_port'], passed), (7790, session))

    def test_router_failure_is_reported(self):
        owner = self.owner(online_role='host')
        session = mock.Mock()
        session.start.side_effect = OSError('ports in use')
        with mock.patch.object(oh, 'HostSession', return_value=session):
            self.run_work(owner)
        owner.messagebox.showerror.assert_called_once()
        owner.launch_game.assert_not_called()
        self.assertFalse(owner.online_busy)

    def test_game_exit_closes_the_port(self):
        owner = self.owner()
        session = mock.Mock()
        owner.host_session = session
        with mock.patch.object(launcher.threading, 'Thread') as thread:
            owner.stop_hosting()
        self.assertIs(thread.call_args.kwargs['target'], session.close)
        self.assertIsNone(owner.host_session)

    def test_summary(self):
        owner = self.owner()
        self.assertEqual(owner.online_summary(), '')
        owner.vars = {'online_enabled': mock.Mock(get=lambda: False)}
        self.assertEqual(owner.online_summary(), '')
        owner.vars = {'online_enabled': mock.Mock(get=lambda: True), 'online_role': mock.Mock(get=lambda: 'host')}
        self.assertIn('hosting', owner.online_summary())


class Updater(unittest.TestCase):
    """This fork's builds never ask the original project for updates: one would remove online play."""

    def test_it_is_off(self):
        self.assertIs(launcher.UPDATES_ENABLED, False)

    def test_no_check_and_no_install(self):
        owner = launcher.Launcher.__new__(launcher.Launcher)
        with mock.patch.object(launcher, 'latest_release') as release, mock.patch.object(launcher.subprocess, 'Popen') as popen:
            owner.check_update()
            owner.check_update(manual=True)
            owner.install_update('9.9.9', 'http://example.invalid/x.zip', 'page')
        release.assert_not_called()
        popen.assert_not_called()


if __name__ == '__main__':
    unittest.main()
