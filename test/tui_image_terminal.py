#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Run the image probe in two real terminals on a private Xvfb display."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from PIL import Image, ImageChops


RELAY = r'''
import fcntl, os, pty, select, signal, sys, termios
for name in ('TMUX','STY','KITTY_WINDOW_ID','GHOSTTY_RESOURCES_DIR',
             'WEZTERM_EXECUTABLE','KONSOLE_VERSION','QSOC_NO_IMAGE_GRAPHICS'):
    os.environ.pop(name, None)
os.environ['TERM_PROGRAM'] = 'ghostty' if sys.argv[1] == 'Kitty' else 'iTerm.app'
os.environ['TERM'] = 'xterm-256color'
pid, fd = pty.fork()
if pid == 0:
    os.execv(sys.argv[2], [sys.argv[2], sys.argv[3]])
def resize(*_):
    geometry = fcntl.ioctl(1, termios.TIOCGWINSZ, b'\0' * 8)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, geometry)
signal.signal(signal.SIGWINCH, resize)
resize()
try:
    with open(sys.argv[4], 'wb', buffering=0) as wire:
        while True:
            if not select.select([fd], [], [], 1)[0]:
                continue
            try:
                data = os.read(fd, 65536)
            except OSError:
                break
            if not data:
                break
            wire.write(data)
            sys.stdout.buffer.write(data)
            sys.stdout.buffer.flush()
finally:
    os.close(fd)
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    os.waitpid(pid, 0)
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--expect-fail', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve()
    if not probe.is_file():
        parser.error('probe executable does not exist')
    root = Path(tempfile.mkdtemp(prefix='test_qsoc_image_terminal_'))
    processes = []
    env = {key: value for key, value in os.environ.items()
           if key in ('PATH', 'LANG', 'LC_ALL', 'HOME', 'USER', 'LOGNAME', 'SHELL', 'XDG_DATA_DIRS')}
    for directory in ('config', 'cache', 'runtime'):
        (root / directory).mkdir(mode=0o700)
    env.update(XDG_CONFIG_HOME=str(root / 'config'), XDG_CACHE_HOME=str(root / 'cache'),
               XDG_RUNTIME_DIR=str(root / 'runtime'), GDK_BACKEND='x11', LIBGL_ALWAYS_SOFTWARE='1')
    (root / 'relay.py').write_text(RELAY)
    (root / 'openbox.xml').write_text(
        '<?xml version="1.0"?><openbox_config xmlns="http://openbox.org/3.4/rc"></openbox_config>')

    def spawn(command, name):
        with (root / (name + '.log')).open('wb') as log:
            process = subprocess.Popen(command, env=env, cwd=root, stdout=log,
                                       stderr=log, start_new_session=True)
        processes.append(process)
        return process

    def run(command):
        return subprocess.run(command, env=env, cwd=root, check=True,
                              capture_output=True, text=True, timeout=15).stdout

    results = {}
    try:
        read_fd, write_fd = os.pipe()
        with (root / 'xvfb.log').open('wb') as log:
            display_process = subprocess.Popen(
                ['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1280x1024x24',
                 '-nolisten', 'tcp', '-ac'], pass_fds=(write_fd,), stdout=log,
                stderr=log, start_new_session=True)
        processes.append(display_process)
        os.close(write_fd)
        if not select.select([read_fd], [], [], 10)[0]:
            raise RuntimeError('Xvfb startup timed out')
        env['DISPLAY'] = ':' + os.read(read_fd, 128).decode().strip()
        os.close(read_fd)
        spawn(['openbox', '--sm-disable', '--config-file', str(root / 'openbox.xml')], 'openbox')

        for protocol in ('Kitty', 'OSC1337'):
            control = root / protocol
            control.mkdir()
            wire = control / 'wire.bin'
            command = [sys.executable, str(root / 'relay.py'), protocol, str(probe),
                       str(control), str(wire)]
            if protocol == 'Kitty':
                launch = ['ghostty', '--config-default-files=false', '--gtk-single-instance=false',
                          '--linux-cgroup=never', '--shell-integration=none',
                          '--title=QSOC-P1-Probe', '-e']
            else:
                launch = ['wezterm', '--skip-config', '--config', 'enable_wayland=false',
                          '--config', 'front_end="Software"', '--config', 'check_for_updates=false',
                          'start', '--always-new-process', '--no-auto-connect', '--cwd', str(control), '--']
            terminal = spawn(['dbus-run-session', '--'] + launch + command, protocol)
            deadline = time.monotonic() + 15
            window = None
            while time.monotonic() < deadline:
                try:
                    window = run(['xdotool', 'search', '--onlyvisible', '--name',
                                  '^QSOC-P1-Probe$']).strip().splitlines()[-1]
                    break
                except (subprocess.CalledProcessError, IndexError):
                    time.sleep(.1)
            if window is None:
                raise RuntimeError(protocol + ' window did not appear')
            sequence = 0
            stages = []

            def stage(action, green, blue=0, height=None, upload=None):
                nonlocal sequence
                before = len(wire.read_bytes()) if wire.exists() else 0
                if height is not None:
                    run(['xdotool', 'windowsize', window, '800', str(height)])
                    time.sleep(.25)
                sequence += 1
                temporary = control / 'command.tmp'
                temporary.write_text(json.dumps({'sequence': sequence, 'action': action}))
                temporary.replace(control / 'command.json')
                deadline = time.monotonic() + 10
                response = {}
                while time.monotonic() < deadline:
                    response_path = control / 'response.json'
                    if response_path.exists():
                        response = json.loads(response_path.read_text())
                        if response.get('sequence') == sequence:
                            break
                    time.sleep(.025)
                else:
                    raise RuntimeError(protocol + ' probe response timed out: ' + action)
                time.sleep(.25)
                screenshot = control / (str(sequence) + '.png')
                for attempt in range(6):
                    run(['import', '-window', window, str(screenshot)])
                    image = Image.open(screenshot).convert('RGB')
                    counts = []
                    for target in ((37, 221, 113), (49, 101, 229)):
                        masks = [channel.point([255 if abs(value - expected) <= 3 else 0
                                                for value in range(256)])
                                 for channel, expected in zip(image.split(), target)]
                        mask = ImageChops.multiply(ImageChops.multiply(masks[0], masks[1]), masks[2])
                        counts.append(mask.histogram()[255])
                    if (counts[0] > 1000) == bool(green) and (counts[1] > 1000) == bool(blue):
                        break
                    time.sleep(.1)
                payload = wire.read_bytes()[before:]
                uploads = payload.count(b'a=t') if protocol == 'Kitty' else payload.count(b'\x1b]1337;File=')
                checks = {'green': counts[0] > 1000 if green else counts[0] == 0,
                          'blue': counts[1] > 1000 if blue else counts[1] == 0}
                if upload is not None:
                    checks['uploads'] = uploads >= 1 if upload else uploads == 0
                if height == 270:
                    checks['clipped'] = response['second']['visibleRows'] < response['second']['rows']
                stages.append({'action': action, 'height': height, 'pixels': counts,
                               'uploads': uploads, 'checks': checks, 'geometry': response,
                               'screenshot_sha256': hashlib.sha256(screenshot.read_bytes()).hexdigest()})

            stage('single', True, height=740)
            stage('hold', True, upload=False)
            stage('input', True, upload=False)
            stage('frame', False, height=270)
            stage('hold', False, upload=False)
            stage('bottom', True, height=740)
            stage('both', True, blue=1)
            stage('single', True)
            stage('fold', False)
            stage('single', True)
            stage('top', False, height=270)
            stage('bottom', True, height=740)
            stage('off', False)
            stage('top', True)
            stage('invalidate', True, upload=protocol == 'OSC1337')
            stage('resume', True, upload=True)
            results[protocol] = stages
            sequence += 1
            (control / 'command.json').write_text(json.dumps({'sequence': sequence, 'action': 'quit'}))
            try:
                terminal.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(terminal.pid, signal.SIGTERM)
        report = {'results': results}
        failures = sum(not passed for stages in results.values() for stage_result in stages
                       for passed in stage_result['checks'].values())
        report['failed_checks'] = failures
        output = json.dumps(report, indent=2)
        if args.output:
            args.output.write_text(output + '\n')
        print(output)
        return 0 if (failures > 0) == args.expect_fail else 1
    finally:
        for process in reversed(processes):
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        for process in processes:
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
        shutil.rmtree(root)


if __name__ == '__main__':
    sys.exit(main())
