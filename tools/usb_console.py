#!/usr/bin/env python3
"""
USB CDC console — splits the USB VCP port into trice output + command input.

Trice TCOBS data FROM the device is piped to the trice tool for decoding.
Commands typed by the user are sent TO the device as plain text.

Usage:
    python3 tools/usb_console.py [/dev/ttyACMx]

    Then type commands at the '> ' prompt (e.g. "help", "peripherals").
    Trice-decoded output appears in real time.
    Ctrl+C to exit.
"""

import serial
import subprocess
import sys
import threading
import os
import tempfile

PORT = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyACM0'
TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
TRICE = os.path.join(TOOLS, 'trice')
TIL = os.path.join(ROOT, 'til.json')
LI = os.path.join(ROOT, 'li.json')


def main():
    # Create named pipe for serial -> trice data flow
    fifo_path = os.path.join(tempfile.gettempdir(), 'periphnet_trice.fifo')
    try:
        os.unlink(fifo_path)
    except FileNotFoundError:
        pass
    os.mkfifo(fifo_path)

    ser = serial.Serial(PORT, timeout=0.1)
    ser.reset_input_buffer()

    # Start trice tool reading from named pipe (FILE mode retries on EOF)
    trice_proc = subprocess.Popen(
        [TRICE, 'log', '-p', 'FILE', '-args', fifo_path,
         '-i', TIL, '-li', LI, '-color', 'off'],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    # Open FIFO for writing (must happen after trice opens it for reading)
    fifo = open(fifo_path, 'wb', buffering=0)

    stop_event = threading.Event()

    def serial_reader():
        """Read from serial port, forward to trice via FIFO."""
        while not stop_event.is_set():
            data = ser.read(256)
            if data:
                try:
                    fifo.write(data)
                except (BrokenPipeError, OSError):
                    break

    def trice_printer():
        """Read decoded trice output, print to terminal."""
        while not stop_event.is_set():
            line = trice_proc.stdout.readline()
            if not line:
                break
            text = line.decode(errors='replace')
            # Strip trice FILE: prefix for cleaner output
            text = text.replace('  FILE: ', '  ')
            sys.stdout.write(text)
            sys.stdout.flush()

    t_reader = threading.Thread(target=serial_reader, daemon=True)
    t_printer = threading.Thread(target=trice_printer, daemon=True)
    t_reader.start()
    t_printer.start()

    print(f"Connected to {PORT}. Type commands, Ctrl+C to exit.\n")

    try:
        while True:
            cmd = input("> ")
            if cmd.strip():
                ser.write((cmd.strip() + '\n').encode())
                ser.flush()
    except (KeyboardInterrupt, EOFError):
        print("\nExiting...")
    finally:
        stop_event.set()
        try:
            fifo.close()
        except OSError:
            pass
        trice_proc.terminate()
        ser.close()
        try:
            os.unlink(fifo_path)
        except FileNotFoundError:
            pass


if __name__ == '__main__':
    main()
