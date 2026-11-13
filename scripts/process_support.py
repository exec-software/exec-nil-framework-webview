#!/usr/bin/env python3
"""Bounded owned test processes; Windows Jobs or POSIX process groups.

Derived from the matching NIL web server harness (Execute Software, MIT).
"""
from __future__ import annotations
import ctypes
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

def require(good: bool, message: str):
    if not good:
        raise AssertionError(message)

class WindowsJob:
    """An unnamed owned job; descendants inherit it before any command runs."""
    def __init__(self):
        from ctypes import wintypes
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel = self.kernel
        kernel.CreateJobObjectW.argtypes = [wintypes.LPVOID, wintypes.LPCWSTR]
        kernel.CreateJobObjectW.restype = wintypes.HANDLE
        kernel.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
        kernel.SetInformationJobObject.restype = wintypes.BOOL
        kernel.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        kernel.AssignProcessToJobObject.restype = wintypes.BOOL
        kernel.QueryInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID,
                                                     wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
        kernel.QueryInformationJobObject.restype = wintypes.BOOL
        kernel.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        kernel.TerminateJobObject.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL

        class BasicLimits(ctypes.Structure):
            _fields_ = [("ProcessTime", ctypes.c_int64), ("JobTime", ctypes.c_int64),
                        ("Flags", wintypes.DWORD), ("MinWorkingSet", ctypes.c_size_t),
                        ("MaxWorkingSet", ctypes.c_size_t), ("ActiveLimit", wintypes.DWORD),
                        ("Affinity", ctypes.c_size_t), ("Priority", wintypes.DWORD),
                        ("Scheduling", wintypes.DWORD)]

        class IoCounters(ctypes.Structure):
            _fields_ = [(name, ctypes.c_uint64) for name in
                        ("ReadOps", "WriteOps", "OtherOps", "ReadBytes", "WriteBytes", "OtherBytes")]

        class ExtendedLimits(ctypes.Structure):
            _fields_ = [("Basic", BasicLimits), ("IO", IoCounters),
                        ("ProcessMemory", ctypes.c_size_t), ("JobMemory", ctypes.c_size_t),
                        ("PeakProcessMemory", ctypes.c_size_t), ("PeakJobMemory", ctypes.c_size_t)]

        class Accounting(ctypes.Structure):
            _fields_ = [("UserTime", ctypes.c_int64), ("KernelTime", ctypes.c_int64),
                        ("PeriodUser", ctypes.c_int64), ("PeriodKernel", ctypes.c_int64),
                        ("PageFaults", wintypes.DWORD), ("TotalProcesses", wintypes.DWORD),
                        ("ActiveProcesses", wintypes.DWORD), ("TerminatedProcesses", wintypes.DWORD)]

        self.Accounting = Accounting
        self.handle = kernel.CreateJobObjectW(None, None)
        require(bool(self.handle), "cannot create owned Windows process job")
        limits = ExtendedLimits()
        limits.Basic.Flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            self.close()
            raise AssertionError("cannot set owned Windows job limits")

    def assign(self, process):
        # CPython's Popen owns this stable process handle. The bootstrap is
        # blocked on stdin until assignment succeeds; no descendant can race it.
        require(bool(self.kernel.AssignProcessToJobObject(self.handle, int(process._handle))),
                "cannot assign owned Windows bootstrap to its job")

    def active(self):
        info = self.Accounting()
        returned = ctypes.c_ulong()
        require(bool(self.kernel.QueryInformationJobObject(self.handle, 1, ctypes.byref(info),
                    ctypes.sizeof(info), ctypes.byref(returned))) and returned.value == ctypes.sizeof(info),
                "cannot inspect owned Windows job")
        return info.ActiveProcesses != 0

    def terminate(self):
        require(bool(self.kernel.TerminateJobObject(self.handle, 124)), "cannot terminate owned Windows job")

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def group_active(pid: int) -> bool:
    try:
        os.killpg(pid, 0)
        return True
    except ProcessLookupError:
        return False


def drain(check, seconds: float) -> bool:
    until = time.monotonic() + seconds
    while check():
        if time.monotonic() >= until:
            return False
        time.sleep(0.025)
    return True


def run(command: list[str], *, cwd: Path, env: dict[str, str], evidence: Path,
        label: str, timeout: int) -> dict:
    started = time.monotonic()
    row = {"command": command, "timeout_seconds": timeout, "cleanup_verified": False}
    process = None
    job = None
    assigned = False
    problem = None
    with (evidence / (label + ".stdout")).open("wb") as stdout, \
         (evidence / (label + ".stderr")).open("wb") as stderr:
        try:
            if os.name == "nt":
                job = WindowsJob()
                # Isolated bootstrap cannot spawn the actual command until its
                # inherited job has been assigned. It waits for that command so
                # Popen's exit status remains the command's status on Windows.
                bootstrap = ("import subprocess,sys; "
                    "go=sys.stdin.buffer.read(1); "
                    "sys.exit(subprocess.call(sys.argv[1:],stdin=subprocess.DEVNULL) if go==b'G' else 125)")
                process = subprocess.Popen([sys.executable, "-I", "-S", "-c", bootstrap, *command],
                    cwd=cwd, env=env, stdin=subprocess.PIPE, stdout=stdout, stderr=stderr)
                row["pid"] = process.pid
                job.assign(process)
                assigned = True
                process.stdin.write(b"G")
                process.stdin.flush()
                process.stdin.close()
            else:
                process = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                    stdout=stdout, stderr=stderr, start_new_session=True)
                row["pid"] = process.pid
            deadline = started + timeout
            while True:
                if max(os.fstat(stdout.fileno()).st_size, os.fstat(stderr.fileno()).st_size) > 8 * 1024**2:
                    raise AssertionError(label + " exceeded the 8 MiB output limit")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(command, timeout)
                try:
                    row["exit_code"] = process.wait(timeout=min(0.05, remaining))
                    if max(os.fstat(stdout.fileno()).st_size, os.fstat(stderr.fileno()).st_size) > 8 * 1024**2:
                        raise AssertionError(label + " exceeded the 8 MiB output limit")
                    break
                except subprocess.TimeoutExpired:
                    pass
            if job:
                # Job accounting may retain terminated process references;
                # the exit code is cached, so release Popen's owned handle.
                process._handle.Close()
            active = job.active if job else lambda: group_active(process.pid)
            row["unexpected_descendants"] = not drain(active, 5)
            if row["unexpected_descendants"]:
                problem = AssertionError(label + " left a descendant process after exit")
        except subprocess.TimeoutExpired:
            row["timed_out"] = True
            problem = AssertionError(label + " exceeded its outer deadline")
        except Exception as error:
            problem = error
        finally:
            try:
                if process is not None:
                    if job and assigned:
                        if job.active():
                            job.terminate()
                        process.wait(timeout=5)
                        process._handle.Close()
                        row["cleanup_verified"] = drain(job.active, 5)
                    elif job:
                        # Assignment failed while stdin was still a barrier;
                        # the bootstrap has not been allowed to spawn children.
                        if process.poll() is None:
                            process.kill()
                        process.wait(timeout=5)
                        process._handle.Close()
                        row["cleanup_verified"] = True
                    else:
                        if group_active(process.pid):
                            try:
                                os.killpg(process.pid, signal.SIGTERM)
                            except ProcessLookupError:
                                pass
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            pass
                        if not drain(lambda: group_active(process.pid), 1):
                            try:
                                os.killpg(process.pid, signal.SIGKILL)
                            except ProcessLookupError:
                                pass
                        process.wait(timeout=5)
                        row["cleanup_verified"] = drain(lambda: group_active(process.pid), 5)
                else:
                    row["cleanup_verified"] = True
            except Exception as error:
                row["cleanup_error"] = type(error).__name__
                problem = problem or error
            finally:
                if job:
                    job.close()
                row["elapsed_seconds"] = round(time.monotonic() - started, 6)
                (evidence / (label + ".command.json")).write_text(json.dumps(row, indent=2) + "\n")
    if problem:
        raise problem
    require(row["cleanup_verified"], label + " process cleanup could not be verified")
    require(row.get("exit_code") == 0, label + " failed; see preserved logs")
    return row
