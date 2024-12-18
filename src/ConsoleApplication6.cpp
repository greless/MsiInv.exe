/*---------------------------------------------------------------------------
Feature list:
    Command line parameters are case insensitive

    Shows all MSI based products on machine.
        registered product properties for each product
        patches and transforms for each product
        all features and install states for each product.
            includes feature usage and last used date (when set.)
            summary of install states for features of this product
        all components and install states for each product.
            calculates shared/permanent based on clients for each component
            all keypaths for each product
                registry key path:
                    checks for key existence, and last write date (NT)
                file key path:
                    checks for file existence, owner (NT), attributes,
                    application marking, file size, create and modify dates
            summary for component states of this product
    Component evaluation
        Shows all shared components (any product.)  Shows all products
            that share the component.
        Shows all components enumerated with product clients that do
            not show up via enumerating products, or are permanent with no
            current product installed. (orphaned)
    Shows location and file names of all logs.
        NT:  machine temp, user temp
        9x:  only one temp.
    Dumps event log
        NT:  All events with source = MsiInstaller
        9x:  from temp\msievent.log


TODO:
    Re-do fixed size buffers
    Make the date formatting support locale
    Consider breaking it into more function

---------------------------------------------------------------------------*/

#pragma warning(disable:4996) 
#include <windows.h>
//#define _WIN32_MSI 110
#include "msi.h"
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <userenv.h>

#pragma comment (lib, "Msi.lib")
#pragma comment (lib, "Userenv.lib")

#define Pluralize(X) ((1 == X) ? TEXT("") : TEXT("s"))

#define MinimumPlatform(fWin9X, minMajor, minMinor) ((g_fWin9X == fWin9X) && ((minMajor < g_osviVersion.dwMajorVersion) || ((minMajor == g_osviVersion.dwMajorVersion) && (minMinor <= g_osviVersion.dwMinorVersion))))

// make sure that the help file gets update as new platform values are added.
#define MinimumPlatformWindowsNT51() MinimumPlatform(false, 5, 1)
#define MinimumPlatformWindows2000() MinimumPlatform(false, 5, 0)
#define MinimumPlatformWindowsNT4()  MinimumPlatform(false, 4, 0)

#define MinimumPlatformMillennium()  MinimumPlatform(true,  4, 90)
#define MinimumPlatformWindows98()   MinimumPlatform(true,  4, 10)
#define MinimumPlatformWindows95()   MinimumPlatform(true,  4, 0)

const int CCHProductInfo = 1024;
const int CCHFeatureName = 256;
const int SD_SIZE = 1024;
const int NAME_SIZE = 256;
const int COUNTAllowedInstallStates = (int)INSTALLSTATE_DEFAULT - (int)INSTALLSTATE_NOTUSED; // the feature states are an enum with no size entry.
const int AllowedInstallStatesOffset = -(int)INSTALLSTATE_NOTUSED;
const int CCHGuid = 39;  // GUID + NULL
const TCHAR SZPermanentProduct[CCHGuid] = TEXT("{00000000-0000-0000-0000-000000000000}");

OSVERSIONINFO   g_osviVersion;
bool            g_fWin9X = false;

struct INSTALLSTATENAMES {
    INSTALLSTATE IS;
    const TCHAR* szState;
    const TCHAR* szStateShort;
} InstallStateNames[] =
{    // INSTALLSTATE_BROKEN, TEXT("are broken"), TEXT("broken"), 
    (INSTALLSTATE)-999, TEXT("error"), TEXT("error"),
    INSTALLSTATE_NOTUSED, TEXT("are not used"), TEXT("not used"),
    INSTALLSTATE_ADVERTISED, TEXT("are advertised"), TEXT("advertised"),
    INSTALLSTATE_ABSENT, TEXT("are absent"), TEXT("absent"),
    INSTALLSTATE_LOCAL, TEXT("installed to run local"), TEXT("local"),
    INSTALLSTATE_SOURCE, TEXT("installed to run from source"), TEXT("source"),
    INSTALLSTATE_DEFAULT, TEXT("installed for default"), TEXT("default"),
};

struct INSTALLPROPERTIES {
    const TCHAR* szProperty;
    const TCHAR* szTitle;
    bool fAdvertised;
} InstallProperties[] =
{
    INSTALLPROPERTY_PACKAGECODE,         TEXT("\tPackage code:\t"), true,
    INSTALLPROPERTY_VERSIONSTRING,       TEXT("\tVersion:\t"), false,
    // queried elsewhere    INSTALLPROPERTY_ASSIGNMENTTYPE,      TEXT("\tAssignment Type:\t"), true,
        INSTALLPROPERTY_PUBLISHER,           TEXT("\tPublisher:\t"), false,
        INSTALLPROPERTY_LANGUAGE,            TEXT("\tLanguage:\t"), true,
        // queried elsewhere    INSTALLPROPERTY_PRODUCTID,           TEXT("\tProduct ID: "), false,
            INSTALLPROPERTY_INSTALLLOCATION,     TEXT("\tSuggested installation location: "), false,
            INSTALLPROPERTY_INSTALLSOURCE,       TEXT("\tInstalled from: "), false,
            INSTALLPROPERTY_PACKAGENAME,         TEXT("\t    Package:\t"), true,
            INSTALLPROPERTY_PRODUCTICON,         TEXT("\tProduct Icon:\t"), true,
            //    INSTALLPROPERTY_ADVTFLAGS,           TEXT("\tAdvertisement Flags: "), ?
            // queried elsewhere    INSTALLPROPERTY_INSTALLDATE,         TEXT("\tInstalled:\t"), false,
                INSTALLPROPERTY_URLINFOABOUT,        TEXT("\tAbout link:\t"), false,
                INSTALLPROPERTY_HELPLINK,            TEXT("\tHelp link:\t"), false,
                INSTALLPROPERTY_HELPTELEPHONE,       TEXT("\tHelp telephone:\t"), false,
                INSTALLPROPERTY_URLUPDATEINFO,       TEXT("\tUpdate link:\t"), false,
                L"InstanceType",                      TEXT("\tInstance type:\t"),true,
                INSTALLPROPERTY_TRANSFORMS,          TEXT("\tTransforms:\t"), true,
};

enum EOutputLevel {
    olProducts = 1 << 0,
    olFeatureStates = 1 << 1,
    olFeatureList = 1 << 2,
    olComponentCount = 1 << 3,
    olComponentList = 1 << 4,

    olOrphanedComponents = 1 << 5,
    olSharedComponents = 1 << 6,
    olTimeElapsed = 1 << 7,
    olUserInfo = 1 << 8,
    olLoggingInfo = 1 << 9,
    olComponentEvaluation = olOrphanedComponents | olSharedComponents,

    olVerbose = ~0 & ~(olTimeElapsed),
    olNone = 0,
    olNormal = olVerbose & ~(olComponentList | olFeatureList | olTimeElapsed | olComponentEvaluation | olLoggingInfo),
    olReduced = olProducts | olFeatureStates,

    olModifiers = olTimeElapsed,

};

void ErrorUINT(UINT uiValue, TCHAR* szMessage)
{
    fwprintf(stderr, TEXT("Unexpected error: %d (%s)\n"), uiValue, (szMessage) ? szMessage : TEXT(""));
}

inline void CheckError(UINT uiValue)
{
    if (uiValue != ERROR_SUCCESS)
        ErrorUINT(uiValue, 0);
}

void PrintEventLogTimeGenerated(EVENTLOGRECORD *pevlr)
{
    // from MSDN
    FILETIME FileTime, LocalFileTime;
    SYSTEMTIME SysTime;
    __int64 lgTemp;
    __int64 SecsTo1970 = 116444736000000000;

    lgTemp = Int32x32To64(pevlr->TimeGenerated, 10000000) + SecsTo1970;

    FileTime.dwLowDateTime = (DWORD)lgTemp;
    FileTime.dwHighDateTime = (DWORD)(lgTemp >> 32);

    FileTimeToLocalFileTime(&FileTime, &LocalFileTime);
    FileTimeToSystemTime(&LocalFileTime, &SysTime);

    wprintf(L"%02d/%02d/%02d %02d:%02d:%02d",
        SysTime.wYear,
        SysTime.wMonth,
        SysTime.wDay,

        SysTime.wHour,
        SysTime.wMinute,
        SysTime.wSecond);
}

void PrintLocalFileTime(FILETIME& ft, bool fTime)
{
    FILETIME LocalTime;
    SYSTEMTIME SystemTime;

    if (FileTimeToLocalFileTime(&ft, &LocalTime))
    {
        if (FileTimeToSystemTime(&LocalTime, &SystemTime))
        {

            wprintf(TEXT("%02d\\%02d\\%02d"),
                SystemTime.wYear, SystemTime.wMonth, SystemTime.wDay);

            if (fTime)
            {
                wprintf(TEXT("  %02d:%02d:%02d"),
                    SystemTime.wHour, SystemTime.wMinute, SystemTime.wSecond);
            }
        }
    }
}

void SetPlatformInfo(void)
{
    g_osviVersion.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx(&g_osviVersion);

    if (g_osviVersion.dwPlatformId == VER_PLATFORM_WIN32_WINDOWS)
        g_fWin9X = true;
}

int GetInstallStateStringIndex(INSTALLSTATE IS)
{
    for (int cStates = 1; cStates < (sizeof(InstallStateNames) / sizeof(INSTALLSTATENAMES)); cStates++)
    {
        if (InstallStateNames[cStates].IS == IS)
        {
            return cStates;
            break;
        }
    }
    return 0;
}

void OwnerPrint(PSECURITY_DESCRIPTOR pSD)
{
    // prints the owner of the security descriptor - can be from any secured object,
    // file, registry key, directory, et cetera.

    byte pbSID[SD_SIZE];
    PSID psid = pbSID;
    BOOL fOwnerDefaulted = FALSE;

    if ((!GetSecurityDescriptorOwner(pSD, ((PSID*)&psid), &fOwnerDefaulted)) || (!IsValidSid(psid)))
    {
        if (NULL == psid)
        {
            wprintf(L"No owner");
        }
        else
        {
            wprintf(L"Cannot retrieve Owner (%d)", GetLastError());
        }
        return;
    }

    if (fOwnerDefaulted)
    {
        wprintf(L"Owner Defaulted");
    }
    else
    {
        TCHAR szName[NAME_SIZE] = L"";
        DWORD cbName = NAME_SIZE;
        TCHAR szDomain[NAME_SIZE] = L"";
        DWORD cbDomain = NAME_SIZE;
        SID_NAME_USE snu;
        if (!LookupAccountSid(NULL, psid, szName, &cbName, szDomain, &cbDomain, &snu))
        {

            wprintf(L"Cannot lookup owner (%d)", GetLastError());
            return;
        }

        wprintf(L"%s\\%s", szDomain, szName);
    }
}

void PrintVersionInfo(TCHAR* szFilePath)
{
    // accepts either Registry key (form:  01:path\path\path  (number is root.))
    // or file path.

    byte pbSD[SD_SIZE];
    DWORD cbSD = SD_SIZE;

    if ((NULL != szFilePath) && *szFilePath)
    {
        if (*szFilePath >= '0' && *szFilePath <= '9')
        {
            HKEY hRoot = 0;
            HKEY hKey = 0;
            if (szFilePath[1] >= '0' && szFilePath[1] <= '3')
            {
                HKEY hMsiRoots[] = { HKEY_CLASSES_ROOT, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, HKEY_USERS };
                hRoot = hMsiRoots[szFilePath[1] - '0'];

                if (ERROR_SUCCESS == RegOpenKeyEx(hRoot, NULL, 0, KEY_READ, &hKey))
                {
                    wprintf(TEXT("\t\tKey exists  "));
                    // security
                    if (ERROR_SUCCESS == RegGetKeySecurity(hKey, OWNER_SECURITY_INFORMATION, pbSD, &cbSD))
                    {
                        wprintf(L"Owner: ");
                        OwnerPrint(pbSD);
                    }

                    FILETIME ftLastWriteTime;
                    if (!g_fWin9X && (ERROR_SUCCESS == RegQueryInfoKey(hKey, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &ftLastWriteTime)))
                    {

                        wprintf(TEXT("\n\t\tLast write time: "));
                        PrintLocalFileTime(ftLastWriteTime, true);
                    }
                    wprintf(L"\n");

                }
                else
                {
                    DWORD dwLastErr = GetLastError();

                    wprintf(TEXT("\t\tError checking for key: "));
                    switch (dwLastErr)
                    {
                    case ERROR_ACCESS_DENIED:
                        wprintf(TEXT("access denied"));
                        break;
                    case ERROR_FILE_NOT_FOUND:
                        wprintf(TEXT("key not found"));
                        break;
                    default:
                        wprintf(TEXT("unknown error: %d"), dwLastErr);

                    }
                    wprintf(TEXT("\n"));
                }
            }
        }
        else
        {
            DWORD dwLastErr = 0;
            TCHAR szVersion[CCHProductInfo] = TEXT("");
            DWORD cchVersion = CCHProductInfo;
            TCHAR szLanguage[CCHProductInfo] = TEXT("");
            DWORD cchLanguage = CCHProductInfo;
            DWORD dwAttrib = 0;
            bool  fExtendedAttribs = false;
            WIN32_FILE_ATTRIBUTE_DATA FileInformation;

            if (!g_fWin9X || MinimumPlatformWindows98())
            {
                if (GetFileAttributesEx(szFilePath, GetFileExInfoStandard, &FileInformation))
                {
                    dwAttrib = FileInformation.dwFileAttributes;
                    fExtendedAttribs = true;
                }
            }
            else
            {
                dwAttrib = GetFileAttributes(szFilePath);
            }

            if (ERROR_SUCCESS == (dwLastErr = MsiGetFileVersion(szFilePath, szVersion, &cchVersion, szLanguage, &cchLanguage)))
            {
                wprintf(TEXT("\t\tVersion: %s"), szVersion);
                if (*szLanguage)
                    wprintf(TEXT(",\tLanguage: %s  "), szLanguage);

                wprintf(TEXT("\n"));
            }
            else
            {
                switch (dwLastErr)
                {
                case ERROR_FILE_NOT_FOUND:
                    if ((0xFFFFFFFF != dwAttrib) && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY))
                        wprintf(TEXT("\t\tDirectory exists.\n"));
                    else
                        wprintf(TEXT("\t\tFile or directory not found.\n"));
                    break;
                case ERROR_ACCESS_DENIED:
                    wprintf(TEXT("\t\tAccess denied for version information.\n"));
                    break;
                case ERROR_FILE_INVALID:
                    wprintf(TEXT("\t\tNo version information.\n"));
                    break;
                case ERROR_INVALID_DATA:
                    wprintf(TEXT("\t\tVersion information invalid.\n"));
                    break;
                default:
                    wprintf(TEXT("\t\tUnexpected error reading version information.\n"));
                }
            }

            if (!g_fWin9X && GetFileSecurity(szFilePath, OWNER_SECURITY_INFORMATION, pbSD, SD_SIZE, &cbSD))
            {
                wprintf(TEXT("\t\tOwner: "));
                OwnerPrint(pbSD);
                wprintf(L"\n");
            }

            if (0xFFFFFFFF != dwAttrib)
            {
                DWORD dwBinaryType = 0;
                wprintf(TEXT("\t\tAttributes: "));

                if (!g_fWin9X && GetBinaryType(szFilePath, &dwBinaryType))
                {
                    switch (dwBinaryType)
                    {
                    case SCS_32BIT_BINARY:
                        wprintf(TEXT("WIN32-APP "));
                        break;
                    case SCS_64BIT_BINARY:
                        wprintf(TEXT("WIN64-APP "));
                        break;
                    case SCS_DOS_BINARY:
                        wprintf(TEXT("DOS-APP "));
                        break;
                    case SCS_OS216_BINARY:
                        wprintf(TEXT("OS2-16BIT-APP "));
                        break;
                    case SCS_PIF_BINARY:
                        wprintf(TEXT("PIF "));
                        break;
                    case SCS_POSIX_BINARY:
                        wprintf(TEXT("POSIX-APP "));
                        break;
                    case SCS_WOW_BINARY:
                        wprintf(TEXT("WIN16-APP "));
                        break;
                    default:
                        wprintf(TEXT("Binary type(%d) "), dwBinaryType);
                        break;
                    }
                }

                if (dwAttrib & FILE_ATTRIBUTE_ARCHIVE) wprintf(TEXT("ARCHIVE "));
                if (dwAttrib & FILE_ATTRIBUTE_SYSTEM) wprintf(TEXT("SYSTEM "));
                if (dwAttrib & FILE_ATTRIBUTE_HIDDEN) wprintf(TEXT("HIDDEN "));
                if (dwAttrib & FILE_ATTRIBUTE_NORMAL) wprintf(TEXT("NORMAL "));
                if (dwAttrib & FILE_ATTRIBUTE_READONLY) wprintf(TEXT("READONLY "));
                if (dwAttrib & FILE_ATTRIBUTE_COMPRESSED) wprintf(TEXT("COMPRESSED "));
                if (dwAttrib & FILE_ATTRIBUTE_DIRECTORY) wprintf(TEXT("DIRECTORY "));
                if (dwAttrib & FILE_ATTRIBUTE_TEMPORARY) wprintf(TEXT("TEMPORARY "));
                if (dwAttrib & FILE_ATTRIBUTE_ENCRYPTED) wprintf(TEXT("ENCRYPTED "));
                if (dwAttrib & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) wprintf(TEXT("NOT_CONTENT_INDEXED "));
                if (dwAttrib & FILE_ATTRIBUTE_OFFLINE) wprintf(TEXT("OFFLINE "));
                if (dwAttrib & FILE_ATTRIBUTE_REPARSE_POINT) wprintf(TEXT("REPARSE_POINT "));
                if (dwAttrib & FILE_ATTRIBUTE_SPARSE_FILE) wprintf(TEXT("SPARSE_FILE "));
                wprintf(TEXT("\n"));
                if (fExtendedAttribs)
                {
                    wprintf(TEXT("\t\t"));
                    if (!(dwAttrib & FILE_ATTRIBUTE_DIRECTORY))
                    {
                        if (FileInformation.nFileSizeHigh)
                        {
                            wprintf(TEXT("Size: %u%010u"), FileInformation.nFileSizeHigh, FileInformation.nFileSizeLow);
                        }
                        else
                        {
                            wprintf(TEXT("Size: %u"), FileInformation.nFileSizeLow);
                        }
                    }
                    wprintf(TEXT("  Created: ")); PrintLocalFileTime(FileInformation.ftCreationTime, true);
                    wprintf(TEXT("\n\t\tChanged: "));  PrintLocalFileTime(FileInformation.ftLastWriteTime, true);
                    // accessed is useless - it already has been modified by the tool - always shows today.
                    wprintf(L"\n");
                }
            }
        }
    }
}

void  main(int argc, char* argv[])
{
    EOutputLevel eOutput = olNone;

    DWORD iProductIndex = 0;
    TCHAR szProductCode[CCHGuid];
    TCHAR szProductInfo[CCHProductInfo] = TEXT("");
    DWORD cchProductInfo = CCHProductInfo;
    TCHAR szLocalCache[CCHProductInfo] = TEXT("");
    INSTALLSTATE isProductState = INSTALLSTATE_UNKNOWN;

    UINT cTotalComponents = 0;
    UINT cAccountedForComponents = 0;
    UINT cTotalQualifiedComponents = 0;

    UINT uiEnumerateReturn = ERROR_SUCCESS;
    UINT uiReturn = ERROR_SUCCESS;

    TCHAR *pszLimitProduct = NULL;
    unsigned int cchLimitProduct = 0;

    clock_t clockStart, clockFinish;
    clockStart = clock();

    for (int carg = 1; carg < argc; carg++)
    {
        if (('-' == argv[carg][0]) || ('/' == argv[carg][0]))
        {
            TCHAR chChar = argv[carg][1];
            if (chChar >= 'A' && chChar <= 'Z')
                chChar = chChar - 'A' + 'a';
            switch (chChar)
            {
            case 'p':
                eOutput = EOutputLevel(eOutput | olProducts);
                if ((carg + 1) < argc)
                {
                    if ((*argv[carg + 1] != '-') && (*argv[carg + 1] != '/'))
                    {
                        carg++;
                     
                        size_t cstr_len = strlen(argv[carg]) + 1; // 包含终止符

                        // 计算目标宽字符数组的大小
                        size_t wstr_len = mbstowcs(NULL, argv[carg], 0) + 1;
                        wchar_t* wstr = new wchar_t[wstr_len];

                        // 转换
                        mbstowcs(wstr, argv[carg], cstr_len);
                        pszLimitProduct = wstr;
                        // pszLimitProduct = argv[carg];
                        cchLimitProduct = lstrlen(pszLimitProduct);
                    }
                }
                break;
            case 'f':
                eOutput = EOutputLevel(eOutput | olProducts | olFeatureStates);
                break;
            case 'q':
                eOutput = EOutputLevel(eOutput | olProducts | olComponentCount);
                break;
            case '#':
                eOutput = EOutputLevel(eOutput | olProducts | olComponentCount | olFeatureStates);
                break;
            case 'x':
                eOutput = EOutputLevel(eOutput | olOrphanedComponents);
                break;
            case 'm':
                eOutput = EOutputLevel(eOutput | olSharedComponents);
                break;
            case 'c':
                eOutput = EOutputLevel(eOutput | olComponentEvaluation);
                break;
            case 'v':
                eOutput = EOutputLevel(eOutput | olVerbose);
                break;
            case 't':
                eOutput = EOutputLevel(eOutput | olTimeElapsed);
                break;
            case 'l':
                eOutput = EOutputLevel(eOutput | olLoggingInfo);
                break;
            case 'n':
                eOutput = EOutputLevel(eOutput | olNormal);
                break;
            case 's':
                eOutput = EOutputLevel(eOutput | olReduced);
                break;
            case '?':
            default:
                wprintf(TEXT("Usage: %s [option [option]]\n"), argv[0]);
                wprintf(TEXT("\t-p [product]\tProduct list\n"));
                wprintf(TEXT("\t-f\tFeature state by product. (includes -p)\n"));
                wprintf(TEXT("\t-q\tComponent count by product (includes -p)\n"));
                wprintf(TEXT("\t-#\tComponent count and features states by product (-p -f -q)\n"));
                wprintf(TEXT("\n"));
                wprintf(TEXT("\t-x\tOrphaned components.\n"));
                wprintf(TEXT("\t-m\tShared components.\n"));
                wprintf(TEXT("\t-c\tEvaluate components (-x -m).\n"));
                wprintf(TEXT("\n"));
                wprintf(TEXT("\t-l\tList of log files.\n"));
                wprintf(TEXT("\n"));
                wprintf(TEXT("\t-t\tElapsed time for run. (Benchmarking)\n"));
                wprintf(TEXT("\n"));
                wprintf(TEXT("\t-s\tReduced output.(-p -#)\n"));
                wprintf(TEXT("\t-n\tNormal output. (default)\n"));
                wprintf(TEXT("\t-v\tVerbose output. (default + feature and component lists)\n"));

                return;
            }
        }
    }

    SetPlatformInfo();

    SYSTEMTIME SystemTime;
    FILETIME FileTime;

    GetSystemTime(&SystemTime);
    SystemTimeToFileTime(&SystemTime, &FileTime);
    printf(("%s  "), argv[0]);
    PrintLocalFileTime(FileTime, true);
    wprintf(TEXT("\n\n"));


    if (olNone == (eOutput & ~olModifiers))
        eOutput = EOutputLevel(eOutput | olNormal);


    INSTALLUILEVEL iuiLevel = MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);

    if (olProducts & eOutput)
    {
        while (ERROR_SUCCESS == (uiEnumerateReturn = MsiEnumProducts(iProductIndex++, szProductCode)))
        {
            isProductState = MsiQueryProductState(szProductCode);

            // Product Name
            CheckError(MsiGetProductInfo(szProductCode, INSTALLPROPERTY_PRODUCTNAME, szProductInfo, &cchProductInfo));
            cchProductInfo = CCHProductInfo;

            if (pszLimitProduct)
            {
                if ((0 != _wcsnicmp(szProductCode, pszLimitProduct, cchLimitProduct)) &&
                    (0 != _wcsnicmp(szProductInfo, pszLimitProduct, cchLimitProduct)))
                {
                    continue;
                }
            }

            wprintf(TEXT("%s\n"), szProductInfo);

            // Product Code -- not all products seem to have names, so put the info prominently here if the name failed.
            wprintf(TEXT("%sProduct code:\t%s\n"), (*szProductInfo) ? TEXT("\t") : TEXT(""), szProductCode);

            // Install State
            const TCHAR* pszState = NULL;
            switch (isProductState)
            {

            case INSTALLSTATE_ABSENT:
                pszState = TEXT("The product is installed for a different user.");
                break;
            case INSTALLSTATE_ADVERTISED:
                pszState = TEXT("The product is advertised, but not installed.");
                break;
            case INSTALLSTATE_BADCONFIG:
                pszState = TEXT("The configuration data is corrupt.");
                break;
            case INSTALLSTATE_DEFAULT:
                pszState = TEXT("Installed.");
                break;
            case INSTALLSTATE_INVALIDARG:
                pszState = TEXT("An internal error has occurred.");
                break;
            case INSTALLSTATE_UNKNOWN:
                pszState = TEXT("The product is neither advertised or installed.");
                break;
            default:
                wprintf(TEXT("Internal error querying product state (%d)\n"), isProductState);
                return;
            }

            wprintf(TEXT("\tProduct state:\t(%d) %s\n"), isProductState, pszState);

            CheckError(MsiGetProductInfo(szProductCode, INSTALLPROPERTY_ASSIGNMENTTYPE, szProductInfo, &cchProductInfo));
            cchProductInfo = CCHProductInfo;
            if (*szProductInfo)
            {

                switch (*szProductInfo)
                {
                case '0':
                    pszState = TEXT("per user");
                    break;
                case '1':
                    pszState = TEXT("per machine");
                    break;
                default:
                    pszState = TEXT("unknown - internal error");
                }
                wprintf(TEXT("\tAssignment:\t%s\n"), pszState);
            }

            {  // install properties

                for (int cPropertyCount = 0; cPropertyCount < (sizeof(InstallProperties) / sizeof(INSTALLPROPERTIES)); cPropertyCount++)
                {
                    if ((INSTALLSTATE_DEFAULT == isProductState) || (InstallProperties[cPropertyCount].fAdvertised))
                    {
                        CheckError(MsiGetProductInfo(szProductCode, InstallProperties[cPropertyCount].szProperty, szProductInfo, &cchProductInfo));
                        cchProductInfo = CCHProductInfo;
                        if (*szProductInfo)
                            wprintf(TEXT("%s%s\n"), InstallProperties[cPropertyCount].szTitle, szProductInfo);
                    }
                }

                if (INSTALLSTATE_DEFAULT == isProductState)
                {
                    // Locally cached package -- useful for pulling out authored information, like friendly names for components.
                    CheckError(MsiGetProductInfo(szProductCode, INSTALLPROPERTY_LOCALPACKAGE, szLocalCache, &cchProductInfo));
                    cchProductInfo = CCHProductInfo;
                    wprintf(TEXT("\tLocal package:\t%s\n"), (0 == lstrlen(szLocalCache)) ? TEXT("<missing>") : szLocalCache);

                    // format the date into familiar form.
                    CheckError(MsiGetProductInfo(szProductCode, INSTALLPROPERTY_INSTALLDATE, szProductInfo, &cchProductInfo));
                    cchProductInfo = CCHProductInfo;

                    TCHAR szDate[20] = TEXT("");
                    swprintf(szDate, TEXT("%s"), szProductInfo);
                    swprintf(szDate + 4, TEXT("\\%s"), szProductInfo + 4);
                    swprintf(szDate + 7, TEXT("\\%s"), szProductInfo + 6);
                    wprintf(TEXT("\tInstall date:\t%s\n"), szDate);
                }

                if (olUserInfo & eOutput)
                {
                    TCHAR szUserInfo[CCHProductInfo] = L"";
                    TCHAR szOrgName[CCHProductInfo] = L"";
                    TCHAR szSerialBuf[CCHProductInfo] = L"";
                    DWORD cchUserInfo, cchOrgName, cchSerialBuf;
                    cchUserInfo = cchOrgName = cchSerialBuf = CCHProductInfo;

                    MsiGetUserInfo(szProductCode, szUserInfo, &cchUserInfo, szOrgName, &cchOrgName, szSerialBuf, &cchSerialBuf);
                    if (*szUserInfo)
                        wprintf(TEXT("\tRegistered to:  %s"), szUserInfo);
                    if (*szOrgName)
                        wprintf(TEXT(", %s"), szOrgName);
                    wprintf(TEXT("\n"));
                    if (*szSerialBuf)
                        wprintf(TEXT("\t\tSerial Code: %s\n"), szSerialBuf);
                }
            }

            UINT InstallStatesIndex = 0;
            UINT isInstallStatesCount[COUNTAllowedInstallStates];

            if (olFeatureStates & eOutput)
            {
                // features
                UINT iFeatureIndex = 0;
                TCHAR szFeatureName[MAX_FEATURE_CHARS] = TEXT("");
                TCHAR szFeatureParent[MAX_FEATURE_CHARS] = TEXT("");
                INSTALLSTATE isFeatureState = INSTALLSTATE_ABSENT;

                for (int cInstallStates = 0; cInstallStates <= COUNTAllowedInstallStates; cInstallStates++)
                {
                    isInstallStatesCount[cInstallStates] = 0;
                }

                if (olFeatureList & eOutput)
                    wprintf(TEXT("\tFeatures for this product:\n"));
                while (ERROR_SUCCESS == MsiEnumFeatures(szProductCode, iFeatureIndex, szFeatureName, szFeatureParent))
                {

                    isFeatureState = MsiQueryFeatureState(szProductCode, szFeatureName);
                    InstallStatesIndex = isFeatureState + AllowedInstallStatesOffset;

                    if (olFeatureList & eOutput)
                        wprintf(TEXT("\t\t%-40s"), szFeatureName);

                    isInstallStatesCount[InstallStatesIndex]++;

                    if (olFeatureList & eOutput)
                    {
                        int iIndex = GetInstallStateStringIndex(isFeatureState);
                        if (iIndex)
                            wprintf(TEXT("(%s)"), InstallStateNames[iIndex].szStateShort);

                        // Feature usage
                        DWORD dwUseCount = 0;
                        WORD wDateUsed = 0;

                        wprintf(TEXT("\n"));

                        if (ERROR_SUCCESS == MsiGetFeatureUsage(szProductCode, szFeatureName, &dwUseCount, &wDateUsed))
                        {
                            wprintf(TEXT("\t\t\tUses: %4u"), dwUseCount);
                            if (wDateUsed)
                            {
                                wprintf(TEXT(",\tLast Used: %04d\\%02d\\%02d"),
                                    /*year*/ ((wDateUsed & 0xFE00) >> 9) + 1980, /*month*/ ((wDateUsed & 0x1E0) >> 5),/*day*/ (wDateUsed & 0x1F));
                            }
                            wprintf(TEXT("\n"));
                        }

                    }
                    iFeatureIndex++;
                }
                wprintf(TEXT("\t%d feature%s.\n"), iFeatureIndex, Pluralize(iFeatureIndex));

                UINT uiFeaturesAccountedFor = 0;
                for (int cStates = 1; cStates < (sizeof(InstallStateNames) / sizeof(INSTALLSTATENAMES)); cStates++)
                {
                    InstallStatesIndex = InstallStateNames[cStates].IS + AllowedInstallStatesOffset;
                    wprintf(TEXT("\t\t%d feature%s %s.\n"), isInstallStatesCount[InstallStatesIndex], Pluralize(isInstallStatesCount[InstallStatesIndex]), InstallStateNames[cStates].szState);
                    uiFeaturesAccountedFor += isInstallStatesCount[InstallStatesIndex];
                }

                UINT uiFeaturesUnaccountedFor = iFeatureIndex - uiFeaturesAccountedFor;
                wprintf(TEXT("\t\t%d feature%s in some other state.\n"), uiFeaturesUnaccountedFor, Pluralize(uiFeaturesUnaccountedFor));
            }

            if (olComponentCount & eOutput)
            {
                // components
                UINT uiComponentIndex = 0;
                UINT cComponentsForThisProduct = 0;
                UINT cQualifiedComponentsForThisProduct = 0;
                UINT cSharedComponentsForThisProduct = 0;
                UINT cPermanentComponentsForThisProduct = 0;
                TCHAR szComponentId[CCHGuid] = TEXT("");
                TCHAR szClientProductCode[CCHGuid] = TEXT("");

                for (int cInstallStates = 0; cInstallStates <= COUNTAllowedInstallStates; cInstallStates++)
                {
                    isInstallStatesCount[cInstallStates] = 0;
                }

                if (olComponentList & eOutput)
                    wprintf(TEXT("\tComponents for this product: \n"));
                while (ERROR_SUCCESS == MsiEnumComponents(uiComponentIndex++, szComponentId))
                {
                    // all components on the entire system are listed, but you have to enumerate
                    // the clients to know if this product uses this component.
                    UINT uiEnumerateClients = 0;
                    bool fProductForThisComponent = false;
                    bool fPermanentComponent = false;
                    bool fSharedComponent = false;

                    while (ERROR_SUCCESS == MsiEnumClients(szComponentId, uiEnumerateClients++, szClientProductCode))
                    {
                        if (0 == _wcsicmp(szClientProductCode, szProductCode))
                        {
                            fProductForThisComponent = true;
                            if (olComponentList & eOutput)
                                wprintf(TEXT("\t%s"), szComponentId);
                        }
                        else if (0 == _wcsicmp(szClientProductCode, SZPermanentProduct))
                        {
                            fPermanentComponent = true;
                        }
                        else
                        {
                            fSharedComponent = true;
                        }
                    }

                    if (fProductForThisComponent)
                    {
                        // at this point, you actually know we're on a component for this product.

                        if (olComponentList & eOutput)
                        {
                            if (fPermanentComponent)
                                wprintf(TEXT(" (permanent)"));
                            if (fSharedComponent)
                                wprintf(TEXT(" (shared)"));

                            *szProductInfo = NULL;

                            INSTALLSTATE isState = MsiGetComponentPath(szProductCode, szComponentId, szProductInfo, &cchProductInfo);
                            InstallStatesIndex = isState + AllowedInstallStatesOffset;
                            isInstallStatesCount[InstallStatesIndex]++;

                            int iIndex = GetInstallStateStringIndex(isState);

                            cchProductInfo = CCHProductInfo;

                            if (iIndex)
                                wprintf(TEXT(" (%s)"), InstallStateNames[iIndex].szStateShort);

                            wprintf(TEXT("\n"));

                            if ((INSTALLSTATE_ABSENT != isState) && *szProductInfo)
                                wprintf(TEXT("\t\tPath: %s\n"), szProductInfo);

                            UINT uiEnumerateQualifiers = 0;

                            TCHAR szQualifierBuf[CCHProductInfo] = TEXT("");
                            DWORD cchQualifierBuf = CCHProductInfo;

                            TCHAR szApplicationDataBuf[CCHProductInfo] = TEXT("");
                            DWORD cchApplicationDataBuf = CCHProductInfo;

                            // File version    
                            if (INSTALLSTATE_ABSENT != isState)
                                PrintVersionInfo(szProductInfo);

                            bool fQualified = false;
                            while (ERROR_SUCCESS == MsiEnumComponentQualifiers(szComponentId, uiEnumerateQualifiers++, szQualifierBuf, &cchQualifierBuf, szApplicationDataBuf, &cchApplicationDataBuf))
                            {
                                cchQualifierBuf = CCHProductInfo;
                                cchApplicationDataBuf = CCHProductInfo;

                                wprintf(TEXT("\t\tQualifier: %s"), szQualifierBuf);
                                if (*szApplicationDataBuf)
                                    wprintf(TEXT(", Application Data: %s"), szApplicationDataBuf);
                                wprintf(TEXT("\n"));
                                fQualified = true;
                            }

                            if (fQualified)
                            {
                                cTotalQualifiedComponents++;
                                cQualifiedComponentsForThisProduct++;
                            }

                        }

                        cComponentsForThisProduct++;
                        if (fPermanentComponent)
                            cPermanentComponentsForThisProduct++;
                        if (fSharedComponent)
                            cSharedComponentsForThisProduct++;
                    }
                }

                cTotalComponents = uiComponentIndex - 1;
                cAccountedForComponents += cComponentsForThisProduct;

                if (olComponentList & eOutput)
                {
                    for (int cStates = 1; cStates < (sizeof(InstallStateNames) / sizeof(INSTALLSTATENAMES)); cStates++)
                    {
                        InstallStatesIndex = InstallStateNames[cStates].IS + AllowedInstallStatesOffset;
                        wprintf(TEXT("\t\t%d component%s %s.\n"), isInstallStatesCount[InstallStatesIndex], Pluralize(isInstallStatesCount[InstallStatesIndex]), InstallStateNames[cStates].szState);
                    }
                }

                wprintf(TEXT("\t%d component%s.\n"), cComponentsForThisProduct, Pluralize(cComponentsForThisProduct));
                wprintf(TEXT("\t\t%d qualified.\n"), cQualifiedComponentsForThisProduct);
                wprintf(TEXT("\t\t%d permanent.\n"), cPermanentComponentsForThisProduct);
                wprintf(TEXT("\t\t%d shared.\n"), cSharedComponentsForThisProduct);
            } // olComponentCount

            // patches
            UINT uiPatchIndex = 0;
            TCHAR szPatchId[CCHGuid] = TEXT("");
            TCHAR szTransformList[CCHProductInfo] = TEXT("");
            while (ERROR_SUCCESS == MsiEnumPatches(szProductCode, uiPatchIndex, szPatchId, szTransformList, &cchProductInfo))
            {
                wprintf(TEXT("\tPatch GUID: %s\n"), szPatchId);
                uiPatchIndex++;
                cchProductInfo = CCHProductInfo;

                if (*szTransformList)
                    wprintf(TEXT("\t\tTransforms: %s\n"), szTransformList);

            }

            wprintf(TEXT("\t%d patch package%s.\n"), uiPatchIndex, Pluralize(uiPatchIndex));

            wprintf(TEXT("\n"));
        }
        assert(ERROR_NO_MORE_ITEMS == uiEnumerateReturn);

        wprintf(TEXT("%d product%s installed.\n"), iProductIndex - 1, Pluralize(iProductIndex - 1));

        if (olComponentCount & eOutput)
            wprintf(TEXT("%d total component%s. \n\n"), cTotalComponents, Pluralize(cTotalComponents));
    }


    if (eOutput & olComponentEvaluation)
    {
        // If there are no shared or permanent components, this should be zero.
        // If there are permanent components, this count will go positive.
        // If there are shared components, this count will go negative.
        UINT cUnaccountedComponents = cTotalComponents - cAccountedForComponents;

        // find orphaned components
        // an orphaned component may have a product listed, but the product wasn't listed
        // in MsiEnumProduct.
        UINT uiComponentIndex = 0;
        TCHAR szOrphanedId[CCHGuid] = TEXT("");
        TCHAR szProductClient[CCHGuid] = TEXT("");
        UINT cSharedComponents = 0;
        UINT cPermanentComponents = 0;
        UINT cPermanentAndParentedComponents = 0;

        cUnaccountedComponents = 0;
        // enumerate every component,
        // then the clients of that component,
        // and check to see if that client is a product of the system.
        while (ERROR_SUCCESS == MsiEnumComponents(uiComponentIndex++, szOrphanedId))
        {
            bool fParentFound = false;
            bool fPermanent = false;
            bool fSharedComponent = false;
            bool fPermanentAndParented = false;
            bool fSpecificProductFound = false;
            UINT uiClientIndex = 0;
            while (ERROR_SUCCESS == MsiEnumClients(szOrphanedId, uiClientIndex++, szProductClient))
            {
                UINT uiProductIndex = 0;
                TCHAR szProduct[CCHGuid] = TEXT("");
                if (0 == _wcsicmp(szProductClient, SZPermanentProduct))
                {
                    fPermanent = true;
                }

                if (pszLimitProduct)
                {
                    if (0 == _wcsicmp(szProductClient, pszLimitProduct))
                    {
                        fSpecificProductFound = true;
                    }
                    else
                    {
                        if (ERROR_SUCCESS == MsiGetProductInfo(szProductClient, INSTALLPROPERTY_PRODUCTNAME, szProductInfo, &cchProductInfo))
                        {
                            if (0 == _wcsicmp(szProductInfo, pszLimitProduct))
                                fSpecificProductFound = true;
                        }
                        cchProductInfo = CCHProductInfo;
                    }
                }

                while (ERROR_SUCCESS == MsiEnumProducts(uiProductIndex++, szProduct))
                {
                    if (0 == _wcsicmp(szProduct, szProductClient))
                    {
                        fParentFound = true;
                    }
                } // products on system
            } // clients of this component

            if (!fParentFound)
                cUnaccountedComponents++;
            else
            {
                if (fPermanent)
                    cPermanentAndParentedComponents++;
            }

            if (fPermanent)
                cPermanentComponents++;

            // A permanent component will have 2 clients, where a normal only has 1.
            if (uiClientIndex > ((fPermanent) ? (UINT)3 : (UINT)2))
            {
                fSharedComponent = true;
                cSharedComponents++;
            }

            if ((!fParentFound || fSharedComponent) && ((NULL == pszLimitProduct) || fSpecificProductFound))
            {
                if (!fParentFound)
                {
                    if (olOrphanedComponents & eOutput)
                    {
                        wprintf(TEXT("Component %s has no parent product"), szOrphanedId);
                    }
                    else
                        continue;
                }
                else if (fSharedComponent)
                {
                    if (olSharedComponents & eOutput)
                    {
                        wprintf(TEXT("Component %s (shared)"), szOrphanedId);
                    }
                    else
                        continue;
                }
                if (fPermanent)
                {
                    wprintf(TEXT(" (permanent)"));
                }
                wprintf(TEXT("\n"));
            }
            else
            {
                continue;
            }

            uiClientIndex = 0;
            while (ERROR_SUCCESS == MsiEnumClients(szOrphanedId, uiClientIndex++, szProductClient))
            {
                wprintf(TEXT("\tProduct Code: %s\n"), szProductClient);
                if (0 == _wcsicmp(SZPermanentProduct, szProductClient))
                {
                    wprintf(TEXT("\t\tPermanent Product placeholder.\n"));
                }
                else if (ERROR_SUCCESS == MsiGetProductInfo(szProductClient, INSTALLPROPERTY_PRODUCTNAME, szProductInfo, &cchProductInfo))
                {
                    wprintf(TEXT("\t\tName: %s\n"), szProductInfo);
                }
                cchProductInfo = CCHProductInfo;

                *szProductInfo = NULL;
                if (!szProductInfo[0])
                {
                    MsiGetComponentPath(szProductClient, szOrphanedId, szProductInfo, &cchProductInfo);
                    cchProductInfo = CCHProductInfo;
                }
            }

            // components on the system                
            if (*szProductInfo)
            {
                wprintf(TEXT("\tComponent path: %s\n"), szProductInfo);
                PrintVersionInfo(szProductInfo);
            }
        }

        wprintf(TEXT("\n"));
        wprintf(TEXT("%d component%s without an installed product.\n"), cUnaccountedComponents, Pluralize(cUnaccountedComponents));
        wprintf(TEXT("%d permanent component%s with a product currently installed.\n"), cPermanentAndParentedComponents, Pluralize(cPermanentAndParentedComponents));
        wprintf(TEXT("%d permanent component%s.\n"), cPermanentComponents, Pluralize(cPermanentComponents));
        if (olComponentList & eOutput)
        {
            wprintf(TEXT("%d qualified component%s.\n"), cTotalQualifiedComponents, Pluralize(cTotalQualifiedComponents));
        }
        wprintf(TEXT("%d shared component%s between currently installed applications.\n"), cSharedComponents, Pluralize(cSharedComponents));

    }

    if (eOutput & olLoggingInfo)
    {
        // need to pull both system and user temp.
        WIN32_FIND_DATA fd;
        TCHAR szSearchFile[MAX_PATH] = TEXT("");
        GetTempPath(MAX_PATH, szSearchFile);
        bool fMachineTempFound = false;

        wprintf(TEXT("\nUser log files in "), szSearchFile);

        for (int iRepeat = 0; iRepeat < ((g_fWin9X) ? 1 : 2); iRepeat++)
        {
            wcscat(szSearchFile, TEXT("msi*.log"));
            wprintf(TEXT("%s:\n"), szSearchFile);

            HANDLE hfff = FindFirstFile(szSearchFile, &fd);
            BOOL fFoundFile = (INVALID_HANDLE_VALUE != hfff);
            while (fFoundFile)
            {
                wprintf(TEXT("\t%-20s "), fd.cFileName);
                PrintLocalFileTime(fd.ftLastWriteTime, true);
                wprintf(TEXT("\n"));
                fFoundFile = FindNextFile(hfff, &fd);
            }
            FindClose(hfff);

            if (!g_fWin9X)
            {
                LPVOID pvoid = NULL;

                if (!fMachineTempFound && CreateEnvironmentBlock(&pvoid, NULL, FALSE))
                {
                    WCHAR* pchEnv = (WCHAR*)pvoid;
                    const WCHAR* const szSearch = L"tmp=";
                    const int cbSearch = lstrlenW(szSearch);

                    while (NULL != pchEnv)
                    {
                        if (0 == _wcsnicmp(szSearch, pchEnv, cbSearch))
                        {
                            TCHAR* pchSearchFile = szSearchFile;
                            pchEnv += cbSearch;
                            while (NULL != *pchEnv)
                            {
                                // purposefully strip out the second half of the unicode character.
                                *pchSearchFile++ = *(TCHAR*)pchEnv++;
                            }
                            *pchSearchFile = '\\';
                            pchSearchFile[1] = NULL;
                            fMachineTempFound = true;
                            break;
                        }
                        else
                        {
                            while (NULL != *pchEnv)
                            {
                                pchEnv = CharNextW(pchEnv);
                            }
                            pchEnv++;
                        }
                    }

                    DestroyEnvironmentBlock(pvoid);
                    if (fMachineTempFound)
                        wprintf(TEXT("\nMachine logs in "), szSearchFile);
                }

            }
        }

        if (eOutput & olLoggingInfo)
        {
            // Event log stuff
            wprintf(TEXT("\nEvent log entries:\n"));
            if (!g_fWin9X)
            {
                //  read recent entries from the event log for MSI
                HANDLE hEvent = OpenEventLog(NULL, TEXT("Application"));

                if (NULL != hEvent)
                {
                    byte bEvents[4096];
                    EVENTLOGRECORD *pevlr = (EVENTLOGRECORD *)&bEvents;
                    DWORD dwRecordOffset = 0; // 1 based
                    DWORD dwBytesRead = 0;
                    DWORD dwMinNumberOfBytesNeeded = 0;
                    BOOL fReadMore = true;
                    LPTSTR lpBuffer = NULL;

                    GetNumberOfEventLogRecords(hEvent, &dwRecordOffset);

                    while (dwRecordOffset && fReadMore)
                    {
                        fReadMore = ReadEventLog(hEvent, EVENTLOG_SEEK_READ | EVENTLOG_BACKWARDS_READ,
                            dwRecordOffset--, bEvents, 4096, &dwBytesRead, &dwMinNumberOfBytesNeeded);

                        if (!fReadMore)
                        {
                            if (ERROR_INSUFFICIENT_BUFFER == GetLastError())
                            {
                                // item too big - not MSI.
                                fReadMore = true;
                                continue;
                            }
                            else
                                break;
                        }
                        else
                        {
                            TCHAR* szSource = (LPWSTR)((LPBYTE)pevlr +
                                sizeof(EVENTLOGRECORD));

                            if (0 == _wcsicmp(szSource, TEXT("MsiInstaller")))
                            {
                                PrintEventLogTimeGenerated(pevlr);

                                wprintf(TEXT(" Type: "));
                                if (0 == pevlr->EventType)
                                    wprintf(TEXT("SUCCESS     "));
                                if (EVENTLOG_ERROR_TYPE & (pevlr->EventType))
                                    wprintf(TEXT("ERROR       "));
                                if (EVENTLOG_INFORMATION_TYPE & (pevlr->EventType))
                                    wprintf(TEXT("INFORMATION "));
                                if (EVENTLOG_WARNING_TYPE & (pevlr->EventType))
                                    wprintf(TEXT("WARNING     "));
                                if (EVENTLOG_AUDIT_SUCCESS & (pevlr->EventType))
                                    wprintf(TEXT("AUDIT_SUCCESS "));
                                if (EVENTLOG_AUDIT_FAILURE & (pevlr->EventType))
                                    wprintf(TEXT("AUDIT_FAILURE "));

                                wprintf(TEXT("Event ID: 0x%08X "), pevlr->EventID);
                                wprintf(TEXT("Source: %s\n"), szSource);

                                wprintf(TEXT("\t%s\n"), ((TCHAR*)pevlr) + (unsigned int)pevlr->UserSidOffset + (unsigned int)pevlr->UserSidLength);
                            }
                        }
                    }
                    CloseEventLog(hEvent);
                }
            }
            else
            {
                TCHAR szSearchFile[MAX_PATH] = TEXT("");
                GetTempPath(MAX_PATH, szSearchFile);
                wcscat(szSearchFile, TEXT("msievent.log"));
                HANDLE hFile = CreateFile(szSearchFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
                if (INVALID_HANDLE_VALUE != hFile)
                {
                    TCHAR szBuf[4096];
                    DWORD dwRead = 0;
                    while (ReadFile(hFile, szBuf, 4096, &dwRead, NULL) && dwRead)
                    {
                        wprintf(TEXT("%s"), szBuf);
                    }
                    CloseHandle(hFile);
                }


            }
        }
    }

    clockFinish = clock();
    float fSeconds = float(clockFinish - clockStart) / float(CLOCKS_PER_SEC);

    if (olTimeElapsed & eOutput)
        wprintf(TEXT("Time: %2.2f seconds\n"), fSeconds);

    MsiSetInternalUI(iuiLevel, NULL);
    return;
}