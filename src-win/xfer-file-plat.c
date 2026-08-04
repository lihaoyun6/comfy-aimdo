#include "plat.h"
#include "xfer-file.h"

#include <windows.h>

bool xfer_file_read_at(XferFileHandle file_handle, uint64_t offset, void *destination,
                       size_t size, bool mark_cold) {
    HANDLE handle = (HANDLE)(uintptr_t)file_handle;
    size_t done = 0;

    (void)mark_cold;
    while (done < size) {
        DWORD got = 0;
        DWORD requested = (DWORD)MIN((uint64_t)0x7ffff000,
                                     (uint64_t)(size - done));
        HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
        OVERLAPPED overlapped = {
            .Offset = (DWORD)((offset + done) & 0xffffffffu),
            .OffsetHigh = (DWORD)((offset + done) >> 32),
            .hEvent = event,
        };

        if (!event) {
            DWORD err = GetLastError();

            log(AIMDO_LOG_ERROR, "%s: CreateEventW failed error=%lu offset=%llu size=%lu\n",
                __func__, err, (ull)(offset + done), requested);
            return false;
        }
        if (!ReadFile(handle, (char *)destination + done, requested, &got, &overlapped)) {
            DWORD err = GetLastError();

            if (err != ERROR_IO_PENDING) {
                log(AIMDO_LOG_ERROR, "%s: ReadFile failed error=%lu handle=%p offset=%llu size=%lu\n",
                    __func__, err, (void *)handle, (ull)(offset + done), requested);
                CloseHandle(event);
                return false;
            }
            if (!GetOverlappedResult(handle, &overlapped, &got, TRUE)) {
                err = GetLastError();

                log(AIMDO_LOG_ERROR, "%s: GetOverlappedResult failed error=%lu handle=%p offset=%llu size=%lu\n",
                    __func__, err, (void *)handle, (ull)(offset + done), requested);
                CloseHandle(event);
                return false;
            }
        }
        CloseHandle(event);
        if (got == 0) {
            log(AIMDO_LOG_ERROR, "%s: zero-byte read handle=%p offset=%llu size=%lu\n",
                __func__, (void *)handle, (ull)(offset + done), requested);
            return false;
        }
        done += got;
    }

    return true;
}
