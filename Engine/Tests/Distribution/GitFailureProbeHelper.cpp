#include <cstdlib>
#include <iostream>
#include <string_view>

/// @brief Version診断だけに成功し、Dependency Restore用Git操作を失敗させるTest Helper
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    if (a_argumentCount == 2 && std::wstring_view(a_arguments[1]) == L"--version")
    {
        std::size_t required = 0U;
        static_cast<void>(_wgetenv_s(&required, nullptr, 0U, L"CUE_TEST_OUTDATED_GIT"));
        std::cout << (required > 0U ? "git version 2.43.9.windows.1\n" : "git version 2.44.0.windows.1\n");
        return 0;
    }
    return 87;
}
