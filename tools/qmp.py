#!/usr/bin/env python3
"""Drive a headless inFlux VM over QMP (QEMU started with -qmp unix:SOCK,server,nowait and a usb-tablet).

usage: tools/qmp.py SOCK command...
  shot FILE.png      screenshot
  click X Y          left click at screen coordinates (1280x800 by default; QMP_SCREEN=WxH to change)
  dclick X Y         double click
  move X Y           move the pointer
  key K1+K2          press keys together (QEMU qcodes; ctrl acts as Cmd)
  type TEXT          type text (US layout)
  sleep SECONDS
  reset | quit       reset or stop the VM
"""
import json
import os
import socket
import sys
import time

SW, SH = (int(v) for v in os.environ.get("QMP_SCREEN", "1280x800").split("x"))
SHIFTED = {'_': 'minus', ':': 'semicolon', '>': 'dot', '<': 'comma', '|': 'backslash', '&': '7', '"': 'apostrophe',
           '*': '8', '~': 'grave_accent', '$': '4', '!': '1', '@': '2', '#': '3', '(': '9', ')': '0', '+': 'equal',
           '?': 'slash', '%': '5', '^': '6', '{': 'bracket_left', '}': 'bracket_right'}
PLAIN = {' ': 'spc', '\n': 'ret', '-': 'minus', '.': 'dot', '/': 'slash', '=': 'equal', ',': 'comma', ';': 'semicolon',
         "'": 'apostrophe', '[': 'bracket_left', ']': 'bracket_right', '\\': 'backslash', '`': 'grave_accent', '\t': 'tab'}


def main():
    s = socket.socket(socket.AF_UNIX)
    s.connect(sys.argv[1])
    f = s.makefile('rw')

    def reply():
        while True:
            m = json.loads(f.readline())
            if 'event' not in m:
                return m

    def cmd(name, **args):
        f.write(json.dumps({'execute': name, 'arguments': args} if args else {'execute': name}) + '\n')
        f.flush()
        return reply()

    reply()
    cmd('qmp_capabilities')

    def pos(x, y):
        return [{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (SW - 1))}},
                {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (SH - 1))}}]

    def button(down):
        cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': 'left'}}])

    a, i = sys.argv[2:], 0
    while i < len(a):
        c = a[i]
        i += 1
        if c == 'shot':
            r = cmd('screendump', filename=os.path.abspath(a[i]), format='png')
            if 'error' in r:
                print(r)
            i += 1
        elif c == 'sleep':
            time.sleep(float(a[i]))
            i += 1
        elif c in ('click', 'dclick', 'move'):
            cmd('input-send-event', events=pos(float(a[i]), float(a[i + 1])))
            i += 2
            time.sleep(0.05)
            for _ in range({'click': 1, 'dclick': 2, 'move': 0}[c]):
                button(True)
                time.sleep(0.05)
                button(False)
                time.sleep(0.08)
        elif c == 'key':
            cmd('send-key', keys=[{'type': 'qcode', 'data': k} for k in a[i].split('+')])
            i += 1
            time.sleep(0.05)
        elif c == 'type':
            for ch in a[i]:
                if ch in PLAIN:
                    keys = [PLAIN[ch]]
                elif ch in SHIFTED:
                    keys = ['shift', SHIFTED[ch]]
                elif ch.isupper():
                    keys = ['shift', ch.lower()]
                else:
                    keys = [ch]
                cmd('send-key', keys=[{'type': 'qcode', 'data': k} for k in keys])
                time.sleep(0.03)
            i += 1
        elif c == 'reset':
            cmd('system_reset')
        elif c == 'quit':
            cmd('quit')
        else:
            sys.exit(f"qmp.py: unknown command {c}")


if __name__ == '__main__':
    main()
