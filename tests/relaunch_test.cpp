// Checks what a relaunch drops from this run's arguments (src/relaunch.h):
// --launcher in every spelling, and nothing that only looks like it.

#include <doctest/doctest.h>
#include <string>
#include "src/relaunch.h"

using band3::relaunch::EraseFlag;
using band3::relaunch::IsFlag;

namespace {

std::wstring Erased(std::wstring command, std::wstring_view name = L"launcher") {
    EraseFlag(command, name);
    return command;
}

}

TEST_CASE("a Windows command line loses --launcher in every spelling") {
    CHECK(Erased(L"band3.exe --launcher") == L"band3.exe");
    CHECK(Erased(L"band3.exe --launcher --fullscreen=false") == L"band3.exe --fullscreen=false");
    CHECK(Erased(L"band3.exe --launcher=true --x") == L"band3.exe --x");
    CHECK(Erased(L"band3.exe --launcher=false") == L"band3.exe");
    CHECK(Erased(L"band3.exe --launcher --launcher=true") == L"band3.exe");
    CHECK(Erased(L"band3.exe\t--launcher\t--x") == L"band3.exe\t--x");
    // a quoted value goes whole
    CHECK(Erased(L"band3.exe --launcher=\"a b\" --x") == L"band3.exe --x");
    CHECK(Erased(L"\"C:\\Games\\band3\\band3.exe\" --relaunch_wait_pid=42 --x",
                 L"relaunch_wait_pid") == L"\"C:\\Games\\band3\\band3.exe\" --x");
}

TEST_CASE("a Windows command line keeps what only looks like --launcher") {
    // another setting
    CHECK(Erased(L"band3.exe --launcher_x") == L"band3.exe --launcher_x");
    CHECK(Erased(L"band3.exe --launcher_x=1 --launcher") == L"band3.exe --launcher_x=1");
    // inside quotes: the program's folder, or a setting's value
    CHECK(Erased(L"\"C:\\my --launcher dir\\band3.exe\" --launcher") ==
          L"\"C:\\my --launcher dir\\band3.exe\"");
    CHECK(Erased(L"band3.exe --content_folders=\"D:\\a --launcher b\"") ==
          L"band3.exe --content_folders=\"D:\\a --launcher b\"");
    // an escaped quote doesn't end the quoted part
    CHECK(Erased(L"band3.exe \"x\\\" --launcher\"") == L"band3.exe \"x\\\" --launcher\"");
    // the program itself is never a setting
    CHECK(Erased(L"--launcher") == L"--launcher");
    CHECK(Erased(L"band3.exe") == L"band3.exe");
}

TEST_CASE("a Linux argument is --launcher in every spelling, and nothing else") {
    CHECK(IsFlag("--launcher", "launcher"));
    CHECK(IsFlag("--launcher=true", "launcher"));
    CHECK(IsFlag("--launcher=false", "launcher"));
    CHECK(IsFlag("--relaunch_wait_pid=42", "relaunch_wait_pid"));
    CHECK_FALSE(IsFlag("--launcher_x", "launcher"));
    CHECK_FALSE(IsFlag("--launche", "launcher"));
    CHECK_FALSE(IsFlag("-launcher", "launcher"));
    CHECK_FALSE(IsFlag("launcher", "launcher"));
    // one argv entry is one argument, so a folder with it inside is just a folder
    CHECK_FALSE(IsFlag("/home/deck/my --launcher dir/band3", "launcher"));
    CHECK_FALSE(IsFlag("--content_folders=/a --launcher b", "launcher"));
}
