#include "PathUtil.h"

#include <Windows.h>

#include <array>
#include <stdexcept>

namespace KurenaiGo
{
    namespace
    {
        std::filesystem::path GetExecutablePath()
        {
            std::array<wchar_t, MAX_PATH> buffer{};
            const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (length == 0 || length == buffer.size())
            {
                throw std::runtime_error("実行ファイルのパス取得に失敗しました (GetModuleFileNameW)");
            }
            return std::filesystem::path(buffer.data());
        }
    }

    std::filesystem::path ResolveAppDataPath(const std::wstring& relativePath)
    {
        return GetExecutablePath().parent_path() / relativePath;
    }
}
