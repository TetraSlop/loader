#include "app/runtime.h"

namespace tmloader
{

Config options;
BYTE*  gameBase   = nullptr;
HWND   gameWindow = nullptr;
DWORD  frames     = 0;
char   runtimeRoot[MAX_PATH]{};

} // namespace tmloader
