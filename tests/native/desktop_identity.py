"""仅供原创 Python 子进程自证 Desktop/Job 身份；没有窗口操作。"""
import ctypes
from ctypes import wintypes


def capture(stage, job_name):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    user = ctypes.WinDLL("user32", use_last_error=True)
    signatures = (
        (kernel.GetCurrentProcess, [], wintypes.HANDLE),
        (kernel.GetCurrentProcessId, [], wintypes.DWORD),
        (kernel.GetCurrentThreadId, [], wintypes.DWORD),
        (kernel.GetProcessTimes, [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4, wintypes.BOOL),
        (kernel.OpenJobObjectW, [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR], wintypes.HANDLE),
        (kernel.IsProcessInJob, [wintypes.HANDLE, wintypes.HANDLE, ctypes.POINTER(wintypes.BOOL)], wintypes.BOOL),
        (kernel.CloseHandle, [wintypes.HANDLE], wintypes.BOOL),
        (user.GetThreadDesktop, [wintypes.DWORD], wintypes.HANDLE),
        (user.OpenInputDesktop, [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD], wintypes.HANDLE),
        (user.CloseDesktop, [wintypes.HANDLE], wintypes.BOOL),
        (user.GetUserObjectInformationW, [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                        wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)], wintypes.BOOL),
    )
    for function, arguments, result in signatures:
        function.argtypes, function.restype = arguments, result

    def check(result, operation):
        if not result:
            raise OSError(ctypes.get_last_error(), operation)

    def desktop_name(desktop):
        check(desktop, "query Desktop handle")
        text, needed = ctypes.create_unicode_buffer(256), wintypes.DWORD()
        check(user.GetUserObjectInformationW(desktop, 2, text, ctypes.sizeof(text), ctypes.byref(needed)),
              "query Desktop name")
        check(text.value, "empty Desktop name")
        return text.value

    process, thread_id = kernel.GetCurrentProcess(), kernel.GetCurrentThreadId()
    times = [wintypes.FILETIME() for _ in range(4)]
    check(kernel.GetProcessTimes(process, *(ctypes.byref(value) for value in times)), "query process creation time")
    thread_desktop = desktop_name(user.GetThreadDesktop(thread_id))
    desktop = user.OpenInputDesktop(0, False, 1)
    check(desktop, "open input Desktop for query")
    try:
        input_desktop = desktop_name(desktop)
    finally:
        check(user.CloseDesktop(desktop), "close input Desktop query handle")
    job = kernel.OpenJobObjectW(4, False, job_name)
    check(job, "open expected Job for query")
    member = wintypes.BOOL()
    try:
        check(kernel.IsProcessInJob(process, job, ctypes.byref(member)), "query expected Job membership")
    finally:
        check(kernel.CloseHandle(job), "close Job query handle")
    return {"Stage": stage, "ProcessId": kernel.GetCurrentProcessId(), "ThreadId": thread_id,
            "ProcessCreated": (times[0].dwHighDateTime << 32) | times[0].dwLowDateTime,
            "ThreadDesktop": thread_desktop, "InputDesktop": input_desktop,
            "JobName": job_name, "InExpectedJob": bool(member.value)}


def require_private_desktop(row, expected_desktop):
    if (not expected_desktop or not row["InExpectedJob"] or not row["ProcessCreated"]
            or row["ThreadDesktop"].casefold() != expected_desktop.casefold()
            or row["ThreadDesktop"].casefold() == row["InputDesktop"].casefold()):
        raise RuntimeError("test process Desktop or Job identity differs from isolated host")
