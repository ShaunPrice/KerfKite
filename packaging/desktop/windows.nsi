Unicode True
!include "MUI2.nsh"
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
Section "KerfKite Studio"
  SetOutPath "$INSTDIR"
  Delete "$INSTDIR\kerfkite_studio-*-py3-none-any.whl"
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
  Delete "$INSTDIR\kerfkite_studio-*-py3-none-any.whl"
  Delete "$INSTDIR\LICENSE"
  RMDir /r "$INSTDIR\runtime-notices"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KerfKiteStudio"
SectionEnd
