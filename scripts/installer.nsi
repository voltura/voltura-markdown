Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"

!ifndef VERSION
    !error "VERSION is required"
!endif
!ifndef OUTPUT
    !define OUTPUT "..\dist\VolturaMarkdown-Setup-${VERSION}-win-x64.exe"
!endif
!ifndef PRODUCT_KEY
    !define PRODUCT_KEY "VolturaMarkdown"
!endif
!ifndef SHORTCUT_NAME
    !define SHORTCUT_NAME "Voltura Markdown"
!endif
!ifndef APP_BINARY
    !define APP_BINARY "..\dist\vmd.exe"
!endif
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_KEY}"

Name "Voltura Markdown"
Caption "Voltura Markdown ${VERSION} Setup"
BrandingText "Voltura AB"
OutFile "${OUTPUT}"
RequestExecutionLevel user
ManifestDPIAware true
ManifestSupportedOS all
SetCompressor /SOLID lzma
SetCompressorDictSize 32
SetDatablockOptimize on
Var AutomaticUpdate
Var UpdateCancelName
Var SetupMutex
!ifdef TEST_INSTALL_DIRECTORY
!if "${PRODUCT_KEY}" == "VolturaMarkdown"
!error "Test installation directory requires an isolated PRODUCT_KEY"
!endif
InstallDir "${TEST_INSTALL_DIRECTORY}"
!else
InstallDir "$LOCALAPPDATA\Programs\Voltura Markdown"
!endif
InstallDirRegKey HKCU "${UNINSTALL_KEY}" "InstallLocation"

VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "Voltura Markdown"
VIAddVersionKey /LANG=1033 "CompanyName" "Voltura AB"
VIAddVersionKey /LANG=1033 "FileDescription" "Voltura Markdown Installer"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "OriginalFilename" "VolturaMarkdown-Setup-${VERSION}-win-x64.exe"
VIAddVersionKey /LANG=1033 "LegalCopyright" "${U+00A9} 2026 Voltura AB"
VIAddVersionKey /LANG=1033 "Comments" "Developer: Joakim Skoglund; Website: https://markdown.voltura.se/"

!define MUI_ICON "..\assets\app.ico"
!define MUI_UNICON "..\assets\app.ico"
!define MUI_ABORTWARNING
!define MUI_WELCOMEFINISHPAGE_BITMAP "..\assets\installer-banner.bmp"
!define MUI_WELCOMEFINISHPAGE_BITMAP_STRETCH FitControl
!define MUI_UNWELCOMEFINISHPAGE_BITMAP "..\assets\installer-banner.bmp"
!define MUI_UNWELCOMEFINISHPAGE_BITMAP_STRETCH FitControl
!define MUI_WELCOMEPAGE_TITLE "Install Voltura Markdown ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "A fast Markdown reader and editor.$\r$\n$\r$\nInstalls for your Windows account."
!define MUI_PAGE_CUSTOMFUNCTION_SHOW SmoothWelcomeArtwork
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\vmd.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open Voltura Markdown"
!define MUI_FINISHPAGE_RUN_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\vmd.exe"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Choose as default Markdown reader"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED
!define MUI_FINISHPAGE_SHOWREADME_FUNCTION ConfigureDefaults
!define MUI_FINISHPAGE_TEXT "Voltura Markdown is installed.$\r$\nUse the default reader option to open Windows settings and choose Voltura Markdown for .md files."
!define MUI_PAGE_CUSTOMFUNCTION_SHOW SmoothFinishArtwork
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!define MUI_PAGE_CUSTOMFUNCTION_SHOW un.SmoothFinishArtwork
!insertmacro MUI_UNPAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

!include "installer-artwork.nsh"
!insertmacro DefineSmoothArtwork ""
!insertmacro DefineSmoothArtwork "un."

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "Voltura Markdown requires 64-bit Windows." /SD IDOK
        SetErrorLevel 1
        Abort
    ${EndIf}
    SetRegView 64
    SetShellVarContext current
    ; Serialize manual and automatic Setup processes before either can stage replacement files.
    System::Call 'kernel32::SetLastError(i 0)'
    System::Call 'kernel32::CreateMutexW(p 0, i 0, w "Local\${PRODUCT_KEY}-Setup") p.r0 ?e'
    Pop $1
    StrCpy $SetupMutex $0
    ${If} $0 == 0
    ${OrIf} $1 == 183
        SetErrorLevel 1
        Abort
    ${EndIf}
    StrCpy $AutomaticUpdate 0
    ${GetParameters} $0
    ClearErrors
    ${GetOptions} $0 "/AUTOMATICUPDATE" $1
    ${IfNot} ${Errors}
        StrCpy $AutomaticUpdate 1
        IfSilent +3
            SetErrorLevel 1
            Abort
        ; Automatic updates must remain attached to their existing per-user installation.
        ReadRegStr $0 HKCU "${UNINSTALL_KEY}" "InstallLocation"
        ${If} $0 != $INSTDIR
            SetErrorLevel 1
            Abort
        ${EndIf}
        ${GetParameters} $0
        ClearErrors
        ${GetOptions} $0 "/UPDATECANCEL=" $UpdateCancelName
        ${If} ${Errors}
            SetErrorLevel 1
            Abort
        ${EndIf}
        Call CheckAutomaticUpdateAllowed
    ${EndIf}
FunctionEnd

Function CheckAutomaticUpdateAllowed
    ${If} $AutomaticUpdate == 1
        System::Call 'user32::GetSystemMetrics(i 0x2000) i.r0'
        ${If} $0 != 0
            SetErrorLevel 1
            Abort
        ${EndIf}
        System::Call 'kernel32::OpenEventW(i 0x100000, i 0, w "$UpdateCancelName") p.r1'
        ${If} $1 == 0
            SetErrorLevel 1
            Abort
        ${EndIf}
        System::Call 'kernel32::WaitForSingleObject(p r1, i 0) i.r0'
        System::Call 'kernel32::CloseHandle(p r1)'
        ${If} $0 != 258
            SetErrorLevel 1
            Abort
        ${EndIf}
    ${EndIf}
FunctionEnd

Function un.onInit
    SetRegView 64
    SetShellVarContext current
FunctionEnd

Function ConfigureDefaults
    ClearErrors
    ExecWait '"$INSTDIR\vmd.exe" --configure-defaults' $0
    IfErrors registration_failed
    StrCmp $0 0 registration_done
    registration_failed:
        MessageBox MB_OK|MB_ICONEXCLAMATION "Windows could not register Voltura Markdown. Open the viewer and try Register for .md files from its menu." /SD IDOK
    registration_done:
FunctionEnd

Section "Install"
    SetOutPath "$INSTDIR"
    SetOverwrite on
    ; Stage first: a locked executable or interrupted extraction must preserve the old reader.
    File /oname=vmd.update-new.exe "${APP_BINARY}"
    Call CheckAutomaticUpdateAllowed
    IfFileExists "$INSTDIR\vmd.update-old.exe" 0 +4
        Delete "$INSTDIR\vmd.update-new.exe"
        SetErrorLevel 1
        Abort
    IfFileExists "$INSTDIR\vmd.exe" 0 replace_reader
        ${If} $AutomaticUpdate == 0
            InitPluginsDir
            SetOutPath "$PLUGINSDIR"
            File /oname=manual-update.exe "${APP_BINARY}"
            SetOutPath "$INSTDIR"
            ClearErrors
            ExecWait '"$PLUGINSDIR\manual-update.exe" --manual-update "$INSTDIR\vmd.exe" "$INSTDIR\vmd.update-new.exe"' $0
            ${If} ${Errors}
            ${OrIf} $0 != 0
                Delete "$INSTDIR\vmd.update-new.exe"
                SetErrorLevel 1
                Abort
            ${EndIf}
            IfFileExists "$INSTDIR\vmd.update-new.exe" +2 0
                Goto reader_replaced
        ${EndIf}
        System::Call 'kernel32::ReplaceFileW(w "$INSTDIR\vmd.exe", w "$INSTDIR\vmd.update-new.exe", w "$INSTDIR\vmd.update-old.exe", i 0, p 0, p 0) i.r0'
        ${If} $0 == 0
            IfFileExists "$INSTDIR\vmd.exe" +2 0
                Rename "$INSTDIR\vmd.update-old.exe" "$INSTDIR\vmd.exe"
            Delete "$INSTDIR\vmd.update-new.exe"
            SetErrorLevel 1
            Abort
        ${EndIf}
        Goto reader_replaced
    replace_reader:
    ClearErrors
    Rename "$INSTDIR\vmd.update-new.exe" "$INSTDIR\vmd.exe"
    ${If} ${Errors}
        Rename "$INSTDIR\vmd.update-old.exe" "$INSTDIR\vmd.exe"
        Delete "$INSTDIR\vmd.update-new.exe"
        SetErrorLevel 1
        Abort
    ${EndIf}
    reader_replaced:
    Delete "$INSTDIR\vmd.update-old.exe"
    File "..\LICENSE"
    File "..\THIRD-PARTY-NOTICES.md"
    SetOutPath "$INSTDIR\licenses"
    File /oname=MD4C-LICENSE.md "..\third_party\md4c\LICENSE.md"
    File /oname=NanoSVG-LICENSE.txt "..\third_party\nanosvg\LICENSE.txt"
    SetOutPath "$INSTDIR"
    WriteUninstaller "$INSTDIR\Uninstall.exe"
    CreateShortcut "$SMPROGRAMS\${SHORTCUT_NAME}.lnk" "$INSTDIR\vmd.exe" "" "$INSTDIR\vmd.exe" 0

    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "Voltura Markdown"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "Voltura AB"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "URLInfoAbout" "https://markdown.voltura.se/"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\vmd.exe,0"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegDWORD HKCU "${UNINSTALL_KEY}" "EstimatedSize" $0
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\vmd.exe"
    IfFileExists "$INSTDIR\vmd.exe" 0 executable_removed
        MessageBox MB_OK|MB_ICONSTOP "Close Voltura Markdown, then run Uninstall again." /SD IDOK
        SetErrorLevel 1
        Abort
    executable_removed:
!if "${PRODUCT_KEY}" == "VolturaMarkdown"
    ; Remove optional registration only when it still belongs to this installed copy.
    ReadRegStr $0 HKCU "Software\Classes\Voltura.Markdown\shell\open\command" ""
    ${If} $0 == '"$INSTDIR\vmd.exe" "%1"'
        DeleteRegKey HKCU "Software\Classes\Voltura.Markdown"
        DeleteRegKey HKCU "Software\Voltura\Markdown\Capabilities"
        DeleteRegValue HKCU "Software\RegisteredApplications" "Voltura Markdown"
        DeleteRegValue HKCU "Software\Classes\.md\OpenWithProgids" "Voltura.Markdown"
        System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
    ${EndIf}

!endif
    ReadRegStr $1 HKCU "${UNINSTALL_KEY}" "InstallLocation"
    ${If} $1 == $INSTDIR
        Delete "$SMPROGRAMS\${SHORTCUT_NAME}.lnk"
        DeleteRegKey HKCU "${UNINSTALL_KEY}"
    ${EndIf}
    Delete "$INSTDIR\LICENSE"
    Delete "$INSTDIR\THIRD-PARTY-NOTICES.md"
    Delete "$INSTDIR\licenses\MD4C-LICENSE.md"
    Delete "$INSTDIR\licenses\NanoSVG-LICENSE.txt"
    RMDir "$INSTDIR\licenses"
    Delete "$INSTDIR\Uninstall.exe"
    RMDir "$INSTDIR"
    ; Documents, unrelated files, and per-user reader preferences are retained.
SectionEnd
