#pragma once

// UI language of the PC app (Hungarian or English), switchable at runtime.
namespace lang {

extern bool g_english;

inline const wchar_t* tr(const wchar_t* hu, const wchar_t* en)
{
    return g_english ? en : hu;
}

} // namespace lang
