module;

#include <common.hxx>
#include <psapi.h>

export module extrainfo;

import common;
import comvars;
import settings;

std::wstring GetModuleVersion(HMODULE hModule)
{
    std::wstring versionStr;
    WCHAR filePath[MAX_PATH];

    if (GetModuleFileNameW(hModule, filePath, MAX_PATH))
    {
        DWORD dummy;
        DWORD size = GetFileVersionInfoSizeW(filePath, &dummy);
        if (size > 0)
        {
            std::vector<BYTE> versionInfo(size);
            if (GetFileVersionInfoW(filePath, 0, size, versionInfo.data()))
            {
                VS_FIXEDFILEINFO* fileInfo;
                UINT len;
                if (VerQueryValueW(versionInfo.data(), L"\\", (LPVOID*)&fileInfo, &len))
                {
                    DWORD major = (fileInfo->dwFileVersionMS >> 16) & 0xFFFF;
                    DWORD minor = (fileInfo->dwFileVersionMS) & 0xFFFF;
                    DWORD build = (fileInfo->dwFileVersionLS >> 16) & 0xFFFF;
                    DWORD revision = (fileInfo->dwFileVersionLS) & 0xFFFF;
                    versionStr = std::format(L"{}.{}.{}.{}", major, minor, build, revision);
                }

                wchar_t* gitHash = nullptr;
                UINT gitHashLen = 0;
                if (VerQueryValueW(versionInfo.data(), L"\\StringFileInfo\\040004b0\\GitHash", (LPVOID*)&gitHash, &gitHashLen) && gitHashLen > 1)
                {
                    versionStr += L"-" + std::wstring(gitHash);
                }
            }
        }
    }
    return versionStr;
}

class ExtraInfo
{
public:
    ExtraInfo()
    {
        FusionFix::onInitEventAsync() += []()
        {
            CIniReader iniReader("");

            auto bExtraInfo = iniReader.ReadInteger("MISC", "ExtraInfo", 1) != 0;

            if (bExtraInfo)
            {
                HMODULE hm = NULL;
                GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&GetModuleVersion, &hm);
                static auto asiVer = L"Fusion Fix: v" + GetModuleVersion(hm);
                static auto exeVer = GetExeModuleName().wstring() + L": v" + GetModuleVersion(NULL);
                static std::wstring ualVer = L"";

                ModuleList dlls;
                dlls.Enumerate(ModuleList::SearchLocation::LocalOnly);
                for (auto& e : dlls.m_moduleList)
                {
                    auto m = std::get<HMODULE>(e);
                    if (IsModuleUAL(m))
                    {
                        ualVer = L"ASI Loader: v" + GetModuleVersion(m) + L" (" + GetModulePath(m).filename().wstring() + L")";
                        break;
                    }
                }

                auto pattern = find_pattern("F3 0F 10 44 24 ? 6A FF 6A FF 50 83 EC 08 F3 0F 11 44 24 ? F3 0F 10 44 24 ? F3 0F 11 04 24 E8 ? ? ? ? 83 C4 14", "D9 44 24 0C 6A FF 6A FF 52 83 EC 08 D9 5C 24 04 D9 44 24 1C D9 1C 24 E8 ? ? ? ? 83 C4 14");
                if (!pattern.empty())
                {
                    static auto reg = *pattern.get_first<uint8_t>(10);
                    static auto PauseHook = safetyhook::create_mid(pattern.get_first(0), [](SafetyHookContext& regs)
                    {
                        static std::wstring extra = L"";

                        if (CGameConfigReader::ms_imgFiles && pMenuTab && (*pMenuTab == 49 || *pMenuTab == 0 || *pMenuTab == 7 || *pMenuTab == 8)) // Graphics || Game || Audio || Display
                        {
                            auto s = std::wstring_view((wchar_t*)(reg == 0x50 ? regs.eax : regs.edx));
                            extra = s;
                            extra += L"~n~";
                            extra += L"                        ";

                            if (*pMenuTab == 0)
                            {
                                if (!exeVer.empty())
                                    extra += L"~p~" + exeVer;

                                if (!asiVer.empty())
                                    extra += L" / " + asiVer;

                                if (!ualVer.empty())
                                    extra += L" / " + ualVer;

                                auto ens = CText::getText("FF_WARN2");
                                if (ens[0])
                                {
                                    if (FusionFixSettings.GetRef("PREF_EXTRANIGHTSHADOWS")->get() != 0)
                                    {
                                        extra += L"~n~";
                                        extra += L"                        ";
                                        extra += L"~r~";
                                        extra += ens;
                                    }
                                }
                            }
                            else if (*pMenuTab == 49)
                            {
                                auto imgNum = 0;
                                auto imgArrSize = 0;
                                for (auto& it : *CGameConfigReader::ms_imgFiles)
                                {
                                    if (it.m_hFile != -1)
                                        imgNum++;
                                    imgArrSize++;
                                }

                                auto FF_WARN0 = CText::getText("FF_WARN0");
                                extra += (FF_WARN0[0] ? FF_WARN0 : L"~p~IMG Files:") + std::wstring(L" ") + std::to_wstring(imgNum) + L" / " + std::to_wstring(imgArrSize);

                                ::PROCESS_MEMORY_COUNTERS pmc;
                                if (::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc)))
                                {
                                    extra += L"; RAM: " + std::to_wstring(pmc.WorkingSetSize / 1024 / 1024) + L" MB";
                                }

                                auto FF_WARN1 = CText::getText("FF_WARN1");
                                if (imgNum >= imgArrSize) extra += FF_WARN1[0] ? FF_WARN1 : L"; ~r~WARNING: 255 IMG limit exceeded, will cause streaming issues.";

                                auto FF_RESTART = CText::getText("FF_RESTART");
                                if (FF_RESTART[0])
                                {
                                    static auto api = FusionFixSettings.GetRef("PREF_GRAPHICSAPI");
                                    static auto initialSelectedApi = api->get();

                                    if (initialSelectedApi != api->get())
                                    {
                                        extra += L"~n~";
                                        extra += L"                        ";
                                        extra += L"~r~";
                                        extra += FF_RESTART;
                                    }
                                }

                                if (FusionFixSettings.Get("PREF_SHADOWFILTER") == FusionFixSettings.ShadowFilterText.eCHSS)
                                {
                                    if (FusionFixSettings.Get("PREF_SHADOW_QUALITY") < 4) // Very High
                                    {
                                        extra += L"~n~";
                                        extra += L"                        ";
                                        auto FF_WARN6 = CText::getText("FF_WARN6");
                                        extra += FF_WARN6[0] ? FF_WARN6 : L"~r~WARNING: CHSS only takes effect with Shadow Quality set to Very High.";
                                    }
                                }

                                // Explains the greyed-out Tone Mapping row, but only while it is
                                // the selected one - live index 18 in the Graphics list
                                // (calibrated in-game; the XML element count lands one higher).
                                if (bHdrLockToneMapping && CMenu::getSelectedItem() == 18)
                                {
                                    extra += L"~n~";
                                    extra += L"                        ";
                                    auto HDRWarnTM = CText::getText("HDRWarnTM");
                                    extra += HDRWarnTM[0] ? HDRWarnTM : L"~y~Tone Mapping is managed by HDR while it is enabled.";
                                }
                            }
                            else if (*pMenuTab == 7)
                            {
                                auto FF_WARN7 = CText::getText("FF_WARN7");
                                extra += FF_WARN7[0] ? FF_WARN7 : L"~r~WARNING: Set Cutscene Audio Sync ON if you have audio desynchronization, OFF for animation smoothness. It can be toggled in a cutscene via ~PAD_UP~";
                            }
                            else if (*pMenuTab == 8) // Display: the HDR toggle lives here
                            {
                                // Console Gamma is no longer explained here - it is
                                // fused into the HDR blit and fully usable, so it is
                                // not greyed. The HDR row itself is greyed on outputs
                                // with no ST2084 support - index 22 in the Display list.
                                if (bHdrLockHdrToggle && CMenu::getSelectedItem() == 22)
                                {
                                    auto HDRWarnNA = CText::getText("HDRWarnNA");
                                    extra += HDRWarnNA[0] ? HDRWarnNA : L"~y~HDR is unavailable: this display session has no HDR output.";
                                }
                            }

                            if (reg == 0x50)
                                regs.eax = (uintptr_t)extra.c_str();
                            else
                                regs.edx = (uintptr_t)extra.c_str();
                        }
                    });
                }
            }
        };
    }
} ExtraInfo;