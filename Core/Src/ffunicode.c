#include "ff.h"

WCHAR ff_convert(WCHAR chr, UINT dir)
{
    (void)dir;

    /*
     * This product uses ASCII-only SD paths such as 2026/08.json.
     * For those names, OEM and Unicode code points are identical.
     */
    if (chr < 0x80U)
    {
        return chr;
    }

    return 0U;
}

WCHAR ff_wtoupper(WCHAR chr)
{
    if (chr >= (WCHAR)'a' && chr <= (WCHAR)'z')
    {
        return (WCHAR)(chr - ((WCHAR)'a' - (WCHAR)'A'));
    }

    return chr;
}
