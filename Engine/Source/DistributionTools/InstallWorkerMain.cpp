#include <string_view>

/// @brief #377でOperation Journal Workerへ接続する前のPayload起動Probeを提供する
int wmain(int a_argumentCount, wchar_t **a_arguments)
{
    return a_argumentCount == 2 && std::wstring_view(a_arguments[1]) == L"--install-probe" ? 0 : 2;
}
