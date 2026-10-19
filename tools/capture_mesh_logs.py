"""Capture serial logs only: sends no commands and requests DTR/RTS low.

Opening a port still depends on its USB driver. Confirm device assignments;
this tool never flashes firmware or starts a pump. Requires pyserial.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import queue
import threading
import time


def utc():
    return datetime.now(timezone.utc).isoformat()


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", action="append", required=True, help="Repeat for each physical device")
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--output", type=Path, required=True, help="New JSONL file; never overwrites a capture")
    args = parser.parse_args()
    if args.seconds <= 0 or args.baud <= 0 or len({p.casefold() for p in args.port}) != len(args.port):
        parser.error("Use positive duration/baud and distinct ports")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    stop = threading.Event()
    records = queue.Queue(maxsize=4096)
    errors = []

    def reader(port):
        device = serial.Serial(port=None, baudrate=args.baud, timeout=0.2)
        device.dtr = False
        device.rts = False
        device.port = port
        try:
            device.open()
            while not stop.is_set():
                line = device.read_until(b"\n", size=4096)
                if line:
                    try:
                        records.put_nowait({"kind": "serial", "source": port, "received_utc": utc(),
                                            "text": line.decode("utf-8", errors="replace").rstrip("\r\n")})
                    except queue.Full:
                        raise RuntimeError("Capture queue full; record is incomplete")
        except Exception as exc:
            errors.append({"source": port, "error": str(exc)})
            stop.set()
        finally:
            device.close()

    # Open the output first, so an existing file cannot cause device side effects.
    with args.output.open("x", encoding="utf-8", newline="\n") as output:
        output.write(json.dumps({"kind": "capture_start", "utc": utc(), "ports": args.port,
                                 "seconds_requested": args.seconds, "baud": args.baud,
                                 "dtr": False, "rts": False, "commands_sent": 0}) + "\n")
        output.flush()
        workers = [threading.Thread(target=reader, args=(port,), daemon=True) for port in args.port]
        for worker in workers:
            worker.start()
        deadline = time.monotonic() + args.seconds
        try:
            while not stop.is_set() and time.monotonic() < deadline:
                try:
                    output.write(json.dumps(records.get(timeout=0.2), ensure_ascii=False) + "\n")
                    output.flush()
                except queue.Empty:
                    pass
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            for worker in workers:
                worker.join(timeout=2)
            if any(worker.is_alive() for worker in workers):
                errors.append({"error": "Serial reader did not close in time"})
            while not records.empty():
                output.write(json.dumps(records.get_nowait(), ensure_ascii=False) + "\n")
            output.write(json.dumps({"kind": "capture_end", "utc": utc(), "errors": errors,
                                     "commands_sent": 0}, ensure_ascii=False) + "\n")
    print(str(args.output.resolve()))
    if errors:
        raise SystemExit(json.dumps(errors, ensure_ascii=False))


if __name__ == "__main__":
    main()
