Unicode True
!include "MUI2.nsh"
!include "FileFunc.nsh"
Name "KerfKite Studio"
OutFile "KerfKite-Windows-x64-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\KerfKite Studio"
RequestExecutionLevel user
Icon "app.ico"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\KerfKite.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Open KerfKite Studio"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "payload\LICENSE"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
; Run the exact eligibility check used by the installed launcher before touching the
; installation directory, shortcuts, registry, or an existing installation.
Function .onInit
  ClearErrors
  InitPluginsDir
  IfErrors unsupported
  StrCmp $PLUGINSDIR "" unsupported
  File /oname=$PLUGINSDIR\KerfKitePlatform.exe "payload\KerfKite.exe"
  IfErrors unsupported
  ExecWait '"$PLUGINSDIR\KerfKitePlatform.exe" --check-windows' $0
  IfErrors unsupported
  StrCmp $0 0 accepted
  unsupported:
    IfSilent done
    MessageBox MB_OK|MB_ICONSTOP "KerfKite Studio requires a native x64 client edition of Windows 10 or Windows 11. Setup could not confirm this computer meets that requirement."
  done:
    SetErrorLevel 1150
    Quit
  accepted:
    ${GetParameters} $1
    ClearErrors
    ${GetOptions} "$1" "/CHECKPLATFORM" $2
    IfErrors continue
    SetErrorLevel 0
    Quit
  continue:
FunctionEnd

Section "KerfKite Studio"
  SetOutPath "$INSTDIR"
  Delete "$INSTDIR\kerfkite_studio-*.whl"
  File /r "payload\*"
  File "app.ico"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\KerfKite Studio.lnk" "$INSTDIR\KerfKite.exe" "" "$INSTDIR\app.ico"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KerfKiteStudio" "DisplayName" "KerfKite Studio"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KerfKiteStudio" "DisplayIcon" "$INSTDIR\app.ico"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KerfKiteStudio" "UninstallString" '"$INSTDIR\Uninstall.exe"'
SectionEnd
Section "Uninstall"
  Delete "$SMPROGRAMS\KerfKite Studio.lnk"
  Delete "$INSTDIR\KerfKite.exe"
  Delete "$INSTDIR\KerfKiteNative.exe"
  Delete "$INSTDIR\Microsoft.Web.WebView2.Core.dll"
  Delete "$INSTDIR\Microsoft.Web.WebView2.WinForms.dll"
  Delete "$INSTDIR\WebView2Loader.dll"
  Delete "$INSTDIR\WebView2-LICENSE.txt"
  Delete "$INSTDIR\WebView2-NOTICE.txt"
  Delete "$INSTDIR\app.ico"
  Delete "$INSTDIR\requirements.lock"
  Delete "$INSTDIR\step.lock"
  Delete "$INSTDIR\kerfkite_studio-*.whl"
  Delete "$INSTDIR\LICENSE"
  RMDir /r "$INSTDIR\runtime-notices"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KerfKiteStudio"
SectionEnd
