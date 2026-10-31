/*
 * Endfield parent-process helper for Wine.
 *
 * Endfield.exe validates that its direct parent is a signed official Games.exe.
 * This helper starts the official Games.exe suspended (it never executes) and
 * creates Endfield.exe with PROC_THREAD_ATTRIBUTE_PARENT_PROCESS pointing at
 * that suspended process. No game files are modified.
 *
 * Build from Linux with:
 *   zig cc -target x86_64-windows-gnu -O2 -municode \
 *     -o endfield-parent-spoof.exe tools/endfield-parent-spoof.cpp
 *
 * This is a compatibility helper, not a general-purpose launcher. It requires
 * the official launcher installation and may be rejected by anti-cheat.
 */

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static void fail(const char *what) {
    const DWORD error = GetLastError();
    fwprintf(stderr, L"%hs failed: Win32 %lu\n", what, (unsigned long)error);
    fflush(stderr);
}

static void to_windows_path(const wchar_t *input, wchar_t *output, size_t capacity) {
    if (input[0] == L'/') {
        _snwprintf(output, capacity, L"Z:");
        size_t used = 2;
        for (const wchar_t *cursor = input; *cursor != L'\0' && used + 1 < capacity; ++cursor) {
            output[used++] = (*cursor == L'/') ? L'\\' : *cursor;
        }
        output[used] = L'\0';
        return;
    }
    wcsncpy(output, input, capacity - 1);
    output[capacity - 1] = L'\0';
}

static void quote_path(const wchar_t *path, wchar_t *output, size_t capacity) {
    _snwprintf(output, capacity, L"\"%ls\"", path);
}

static void directory_of(const wchar_t *path, wchar_t *output, size_t capacity) {
    wcsncpy(output, path, capacity - 1);
    output[capacity - 1] = L'\0';
    wchar_t *slash = wcsrchr(output, L'\\');
    if (slash != NULL) {
        *slash = L'\0';
    } else {
        output[0] = L'\0';
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line, int show) {
    (void)instance;
    (void)previous;
    (void)command_line;
    (void)show;

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == NULL) {
        return 2;
    }
    if (argc < 3) {
        fwprintf(stderr, L"usage: %ls <Games.exe> <Endfield.exe> [args...]\n", argv[0]);
        LocalFree(argv);
        return 2;
    }

    wchar_t parent_path[32'768];
    wchar_t game_path[32'768];
    wchar_t parent_command[32'768];
    wchar_t game_command[32'768];
    wchar_t parent_directory[32'768];
    wchar_t game_directory[32'768];

    to_windows_path(argv[1], parent_path, 32'768);
    to_windows_path(argv[2], game_path, 32'768);
    quote_path(parent_path, parent_command, 32'768);
    quote_path(game_path, game_command, 32'768);
    for (int index = 3; index < argc; ++index) {
        const size_t used = wcslen(game_command);
        _snwprintf(game_command + used, 32'768 - used, L" \"%ls\"", argv[index]);
    }
    directory_of(parent_path, parent_directory, 32'768);
    directory_of(game_path, game_directory, 32'768);

    STARTUPINFOW parent_startup;
    PROCESS_INFORMATION parent_process;
    ZeroMemory(&parent_startup, sizeof(parent_startup));
    ZeroMemory(&parent_process, sizeof(parent_process));
    parent_startup.cb = sizeof(parent_startup);
    parent_startup.dwFlags = STARTF_USESHOWWINDOW;
    parent_startup.wShowWindow = SW_HIDE;

    if (!CreateProcessW(parent_path, parent_command, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL,
            parent_directory, &parent_startup, &parent_process)) {
        fail("CreateProcessW(Games.exe)");
        LocalFree(argv);
        return 4;
    }
    fwprintf(stderr, L"parent created pid=%lu, suspended\n", (unsigned long)parent_process.dwProcessId);
    fflush(stderr);
    CloseHandle(parent_process.hThread);

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
    if (attribute_size == 0) {
        fail("InitializeProcThreadAttributeList(size)");
        TerminateProcess(parent_process.hProcess, 0);
        CloseHandle(parent_process.hProcess);
        LocalFree(argv);
        return 4;
    }

    LPPROC_THREAD_ATTRIBUTE_LIST attributes = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_size);
    if (attributes == NULL) {
        fwprintf(stderr, L"malloc failed\n");
        TerminateProcess(parent_process.hProcess, 0);
        CloseHandle(parent_process.hProcess);
        LocalFree(argv);
        return 4;
    }
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) {
        fail("InitializeProcThreadAttributeList");
        free(attributes);
        TerminateProcess(parent_process.hProcess, 0);
        CloseHandle(parent_process.hProcess);
        LocalFree(argv);
        return 4;
    }
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent_process.hProcess,
            sizeof(parent_process.hProcess), NULL, NULL)) {
        fail("UpdateProcThreadAttribute(PARENT_PROCESS)");
        DeleteProcThreadAttributeList(attributes);
        free(attributes);
        TerminateProcess(parent_process.hProcess, 0);
        CloseHandle(parent_process.hProcess);
        LocalFree(argv);
        return 4;
    }

    STARTUPINFOEXW game_startup;
    PROCESS_INFORMATION game_process;
    ZeroMemory(&game_startup, sizeof(game_startup));
    ZeroMemory(&game_process, sizeof(game_process));
    game_startup.StartupInfo.cb = sizeof(game_startup);
    game_startup.lpAttributeList = attributes;

    const BOOL created =
        CreateProcessW(game_path, game_command, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
            NULL, game_directory, &game_startup.StartupInfo, &game_process);
    const DWORD create_error = GetLastError();

    DeleteProcThreadAttributeList(attributes);
    free(attributes);

    if (!created) {
        SetLastError(create_error);
        fail("CreateProcessW(Endfield.exe)");
        TerminateProcess(parent_process.hProcess, 0);
        WaitForSingleObject(parent_process.hProcess, 5'000);
        CloseHandle(parent_process.hProcess);
        LocalFree(argv);
        return 4;
    }

    fwprintf(stderr, L"game created pid=%lu, parent pid=%lu\n", (unsigned long)game_process.dwProcessId,
        (unsigned long)parent_process.dwProcessId);
    fflush(stderr);
    CloseHandle(game_process.hThread);

    WaitForSingleObject(game_process.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(game_process.hProcess, &exit_code);

    if (WaitForSingleObject(parent_process.hProcess, 0) == WAIT_TIMEOUT) {
        TerminateProcess(parent_process.hProcess, 0);
    }
    WaitForSingleObject(parent_process.hProcess, 5'000);
    CloseHandle(game_process.hProcess);
    CloseHandle(parent_process.hProcess);
    LocalFree(argv);
    return (int)exit_code;
}
