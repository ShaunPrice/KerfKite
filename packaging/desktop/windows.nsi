Unicode True
!include "MUI2.nsh"
!include "FileFunc.nsh"
!ifndef STUDIO_VERSION
  !error "Pass the packaged Studio wheel version as -DSTUDIO_VERSION"
!endif
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
; Start always gets KerfKite Studio; the desktop icon is the user's choice, ticked to begin with.
!define MUI_COMPONENTSPAGE_SMALLDESC
!define MUI_PAGE_HEADER_TEXT "Choose where KerfKite Studio appears"
!define MUI_PAGE_HEADER_SUBTEXT "It is always added to the Start menu. Untick the desktop shortcut if you do not want one."
!insertmacro MUI_PAGE_COMPONENTS
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

Section "KerfKite Studio and its Start menu shortcut" SecStudio
  SectionIn RO
  SetOutPath "$INSTDIR"
  Delete "$INSTDIR\kerfkite_studio-*.whl"
  File /r "payload\*"
  File "app.ico"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\KerfKite Studio.lnk" "$INSTDIR\KerfKite.exe" "" "$INSTDIR\app.ico"
  ; A reinstall without the desktop shortcut removes the one an earlier install made.
  Delete "$DESKTOP\KerfKite Studio.lnk"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KerfKiteStudio" "DisplayName" "KerfKite Studio"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KerfKiteStudio" "DisplayVersion" "${STUDIO_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KerfKiteStudio" "DisplayIcon" "$INSTDIR\app.ico"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KerfKiteStudio" "UninstallString" '"$INSTDIR\Uninstall.exe"'
SectionEnd
Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\KerfKite Studio.lnk" "$INSTDIR\KerfKite.exe" "" "$INSTDIR\app.ico"
SectionEnd
; The workstation server without Python can carry STEP exchange: its Open CASCADE add-on and libraries (LGPL 2.1),
; a component of its own. A reinstall without it removes it.
!if /FileExists "step\kerfkite-step.exe"
Section "STEP import and export (Open CASCADE)" SecStep
  SetOutPath "$INSTDIR"
  File /r "step\*"
SectionEnd
Section "-Remove STEP when not chosen"
  SectionGetFlags ${SecStep} $0
  IntOp $0 $0 & ${SF_SELECTED}
  StrCmp $0 ${SF_SELECTED} keep
  Delete "$INSTDIR\kerfkite-step.exe"
  Delete "$INSTDIR\TK*.dll"
  RMDir /r "$INSTDIR\open-cascade"
  keep:
SectionEnd
!endif
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStudio} "The workshop for your account, with KerfKite Studio in the Start menu."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A KerfKite Studio icon on your desktop. Double-click it to start the workshop."
!if /FileExists "step\kerfkite-step.exe"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecStep} "Read and write STEP files of parts and surfaces, with Open CASCADE Technology (LGPL 2.1), kept as separate files."
!endif
!insertmacro MUI_FUNCTION_DESCRIPTION_END
Section "Uninstall"
  Delete "$SMPROGRAMS\KerfKite Studio.lnk"
  Delete "$DESKTOP\KerfKite Studio.lnk"
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
  Delete "$INSTDIR\kerfkite-studio.exe"
  Delete "$INSTDIR\kerfkite-pendant-tls.exe"
  Delete "$INSTDIR\kerfkite-cam.exe"
  Delete "$INSTDIR\kerfkite-step.exe"
  Delete "$INSTDIR\TK*.dll"
  RMDir /r "$INSTDIR\static"
  RMDir /r "$INSTDIR\open-cascade"
  Delete "$INSTDIR\LICENSE"
  RMDir /r "$INSTDIR\runtime-notices"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\KerfKiteStudio"
SectionEnd
