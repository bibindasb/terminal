// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"

#include "WslDistroGenerator.h"
#include "LegacyProfileGeneratorNamespaces.h"

#include "../../types/inc/utils.hpp"
#include "../../inc/DefaultSettings.h"
#include "Utils.h"
#include <io.h>
#include <fcntl.h>
#include "DefaultProfileUtils.h"

static constexpr std::wstring_view DockerDistributionPrefix{ L"docker-desktop" };

using namespace ::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::Settings::Model;

// Legacy GUIDs:
//   - Debian       58ad8b0c-3ef8-5f4d-bc6f-13e4c00f2530
//   - Ubuntu       2c4de342-38b7-51cf-b940-2309a097f518
//   - Alpine       1777cdf0-b2c4-5a63-a204-eb60f349ea7c
//   - Ubuntu-18.04 c6eaf9f4-32a7-5fdc-b5cf-066e8a4b1e40

std::wstring_view WslDistroGenerator::GetNamespace()
{
    return WslGeneratorNamespace;
}

// Method Description:
// -  Enumerates all the installed WSL distros to create profiles for them.
// Arguments:
// - <none>
// Return Value:
// - a vector with all distros for all the installed WSL distros
std::vector<Profile> _legacyGenerate()
{
    std::vector<Profile> profiles;

    wil::unique_handle readPipe;
    wil::unique_handle writePipe;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, true };
    THROW_IF_WIN32_BOOL_FALSE(CreatePipe(&readPipe, &writePipe, &sa, 0));
    STARTUPINFO si{ 0 };
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe.get();
    si.hStdError = writePipe.get();
    wil::unique_process_information pi;
    wil::unique_cotaskmem_string systemPath;
    THROW_IF_FAILED(wil::GetSystemDirectoryW(systemPath));
    std::wstring command(systemPath.get());
    command += L"\\wsl.exe --list";

    THROW_IF_WIN32_BOOL_FALSE(CreateProcessW(nullptr,
                                             const_cast<LPWSTR>(command.c_str()),
                                             nullptr,
                                             nullptr,
                                             TRUE,
                                             CREATE_NO_WINDOW,
                                             nullptr,
                                             nullptr,
                                             &si,
                                             &pi));
    switch (WaitForSingleObject(pi.hProcess, 2000))
    {
    case WAIT_OBJECT_0:
        break;
    case WAIT_ABANDONED:
    case WAIT_TIMEOUT:
        return profiles;
    case WAIT_FAILED:
        THROW_LAST_ERROR();
    default:
        THROW_HR(ERROR_UNHANDLED_EXCEPTION);
    }
    DWORD exitCode;
    if (!GetExitCodeProcess(pi.hProcess, &exitCode))
    {
        THROW_HR(E_INVALIDARG);
    }
    else if (exitCode != 0)
    {
        return profiles;
    }
    DWORD bytesAvailable;
    THROW_IF_WIN32_BOOL_FALSE(PeekNamedPipe(readPipe.get(), nullptr, NULL, nullptr, &bytesAvailable, nullptr));
    // "The _open_osfhandle call transfers ownership of the Win32 file handle to the file descriptor."
    // (https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/open-osfhandle?view=vs-2019)
    // so, we detach_from_smart_pointer it -- but...
    // "File descriptors passed into _fdopen are owned by the returned FILE * stream.
    // If _fdopen is successful, do not call _close on the file descriptor.
    // Calling fclose on the returned FILE * also closes the file descriptor."
    // https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/fdopen-wfdopen?view=vs-2019
    FILE* stdioPipeHandle = _wfdopen(_open_osfhandle((intptr_t)wil::detach_from_smart_pointer(readPipe), _O_WTEXT | _O_RDONLY), L"r");
    auto closeFile = wil::scope_exit([&]() { fclose(stdioPipeHandle); });

    std::wfstream pipe{ stdioPipeHandle };

    std::wstring wline;
    std::getline(pipe, wline); // remove the header from the output.
    while (pipe.tellp() < bytesAvailable)
    {
        std::getline(pipe, wline);
        std::wstringstream wlinestream(wline);
        if (wlinestream)
        {
            std::wstring distName;
            std::getline(wlinestream, distName, L'\r');

            if (distName.substr(0, std::min(distName.size(), DockerDistributionPrefix.size())) == DockerDistributionPrefix)
            {
                // Docker for Windows creates some utility distributions to handle Docker commands.
                // Pursuant to GH#3556, because they are _not_ user-facing we want to hide them.
                continue;
            }

            const size_t firstChar = distName.find_first_of(L"( ");
            // Some localizations don't have a space between the name and "(Default)"
            // https://github.com/microsoft/terminal/issues/1168#issuecomment-500187109
            if (firstChar < distName.size())
            {
                distName.resize(firstChar);
            }
            auto WSLDistro{ CreateDefaultProfile(distName) };

            WSLDistro.Commandline(L"wsl.exe -d " + distName);
            WSLDistro.DefaultAppearance().ColorSchemeName(L"Campbell");
            WSLDistro.StartingDirectory(DEFAULT_STARTING_DIRECTORY);
            WSLDistro.Icon(L"ms-appx:///ProfileIcons/{9acb9455-ca41-5af7-950f-6bca1bc9722f}.png");
            profiles.emplace_back(WSLDistro);
        }
    }

    return profiles;
}

const wchar_t RegKeyLxss[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss";

HKEY _openWslRegKey()
{
    HKEY hKey{ nullptr };
    if (RegOpenKeyEx(HKEY_CURRENT_USER, RegKeyLxss, 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS)
    {
        return hKey;
    }
    return nullptr;
}

bool _getWslGuids(std::vector<std::wstring>& guidStrings)
{
    static wil::unique_hkey wslRootKey{ _openWslRegKey() };

    if (!wslRootKey)
    {
        return false;
    }

    DWORD dwNumSubKeys = 0;
    DWORD maxSubKeyLen = 0;
    if (RegQueryInfoKey(wslRootKey.get(), // hKey,
                        nullptr, // lpClass,
                        nullptr, // lpcchClass,
                        nullptr, // lpReserved,
                        &dwNumSubKeys, // lpcSubKeys,
                        &maxSubKeyLen, // lpcbMaxSubKeyLen,
                        nullptr, // lpcbMaxClassLen,
                        nullptr, // lpcValues,
                        nullptr, // lpcbMaxValueNameLen,
                        nullptr, // lpcbMaxValueLen,
                        nullptr, // lpcbSecurityDescriptor,
                        nullptr // lpftLastWriteTime
                        ) != ERROR_SUCCESS)
    {
        return false;
    }

    // lpcbMaxSubKeyLen does not include trailing space
    std::unique_ptr<wchar_t[]> buffer = std::make_unique<wchar_t[]>(maxSubKeyLen + 1);

    guidStrings.reserve(dwNumSubKeys);
    for (DWORD i = 0; i < dwNumSubKeys; i++)
    {
        auto cbName = maxSubKeyLen+1;
        auto result = RegEnumKeyEx(wslRootKey.get(), i, buffer.get(), &cbName, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_SUCCESS)
        {
            guidStrings.emplace_back(buffer.get(), cbName);
        }

    }
    return true;
}

std::vector<Profile> WslDistroGenerator::GenerateProfiles()
{
    std::vector<std::wstring> guidStrings{};
    if (_getWslGuids(guidStrings))
    {
        for (auto& guid : guidStrings)
        {
            // wil::unique_cotaskmem_string guidString;
            // if (SUCCEEDED(StringFromCLSID(guid, &guidString)))
            // {
            //     ids.push_back(guidString.get());
            // }
            guid;
        }
    }

    return _legacyGenerate();
}
