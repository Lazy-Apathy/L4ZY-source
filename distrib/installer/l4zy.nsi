; Installateur L4ZY (NSIS 3, Unicode). Construit par tools/sauerrt_build.py.
; Installation par utilisateur (pas de droits administrateur), dans
; %LOCALAPPDATA%\Programs\L4ZY par defaut : les mises a jour se font
; ensuite sans demande d'elevation.
;
; Options (mode silencieux /S pour les essais) :
;   /D=<dossier>     dossier d'installation (dernier argument, sans guillemets)
;   /PORTABLE        donnees du joueur dans <dossier>\userdata (essais isoles)
;   /NOSTARTER       sans effet (aucun modele n est telecharge a l installation)
;   /NOSHORTCUTS     ni raccourcis, ni entree "Programmes et fonctionnalites"

Unicode true
ManifestDPIAware true
RequestExecutionLevel user
SetCompressor zlib
SetDatablockOptimize on
Name "L4ZY ${VERSION}"
OutFile "${OUTFILE}"
Icon "${ICON}"
UninstallIcon "${ICON}"
InstallDir "$LOCALAPPDATA\Programs\L4ZY"
BrandingText "L4ZY ${VERSION}"
VIProductVersion "1.0.0.0"
VIAddVersionKey /LANG=0 "ProductName" "L4ZY"
VIAddVersionKey /LANG=0 "FileDescription" "L4ZY ${VERSION} setup"
VIAddVersionKey /LANG=0 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "LegalCopyright" "Sauerbraten engine zlib licence; see docs"

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\L4ZY.exe"
!define MUI_FINISHPAGE_RUN_TEXT "$(RunNow)"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "French"
!insertmacro MUI_LANGUAGE "English"

LangString RunNow ${LANG_FRENCH} "Lancer L4ZY"
LangString RunNow ${LANG_ENGLISH} "Start L4ZY"
LangString SecGame ${LANG_FRENCH} "L4ZY (jeu, traduction, mises à jour)"
LangString SecGame ${LANG_ENGLISH} "L4ZY (game, translation, updates)"
LangString SecDesk ${LANG_FRENCH} "Raccourci sur le bureau"
LangString SecDesk ${LANG_ENGLISH} "Desktop shortcut"
LangString Running ${LANG_FRENCH} "L4ZY est ouvert depuis ce dossier. Ferme le jeu puis réessaie."
LangString Running ${LANG_ENGLISH} "L4ZY is running from this folder. Close the game and try again."
LangString Junction ${LANG_FRENCH} "Ce dossier contient une jonction ou un lien ($R9). Choisis un autre dossier."
LangString Junction ${LANG_ENGLISH} "This folder contains a junction or link ($R9). Choose another folder."
LangString Rollback ${LANG_FRENCH} "L4ZY - revenir à la version précédente"
LangString Rollback ${LANG_ENGLISH} "L4ZY - go back to the previous version"
LangString KeepData ${LANG_FRENCH} "Tes réglages, corrections de traduction et modèles sont conservés (Documents\My Games\L4ZY et %LOCALAPPDATA%\L4ZY)."
LangString KeepData ${LANG_ENGLISH} "Your settings, translation corrections and models are kept (Documents\My Games\L4ZY and %LOCALAPPDATA%\L4ZY)."

Var Portable
Var NoStarter
Var NoShortcuts

Function .onInit
    ${GetParameters} $R0
    StrCpy $Portable "0"
    StrCpy $NoStarter "0"
    StrCpy $NoShortcuts "0"
    ClearErrors
    ${GetOptions} $R0 "/PORTABLE" $R1
    ${IfNot} ${Errors}
        StrCpy $Portable "1"
    ${EndIf}
    ClearErrors
    ${GetOptions} $R0 "/NOSTARTER" $R1
    ${IfNot} ${Errors}
        StrCpy $NoStarter "1"
    ${EndIf}
    ClearErrors
    ${GetOptions} $R0 "/NOSHORTCUTS" $R1
    ${IfNot} ${Errors}
        StrCpy $NoShortcuts "1"
        SectionSetFlags 1 0
    ${EndIf}
FunctionEnd

; Un dossier dont un element est une jonction ferait ecrire ailleurs : refuse.
Function CheckReparse
    StrCpy $R8 "$INSTDIR"
    loop:
        ${If} ${FileExists} "$R8\*.*"
            System::Call 'kernel32::GetFileAttributesW(w "$R8") i .r1'
            IntOp $2 $1 & 0x400
            ${If} $1 != -1
            ${AndIf} $2 != 0
                StrCpy $R9 "$R8"
                MessageBox MB_ICONSTOP "$(Junction)" /SD IDOK
                Abort
            ${EndIf}
        ${EndIf}
        ${GetParent} "$R8" $R7
        StrCmp $R7 "" done
        StrCmp $R7 $R8 done
        StrCpy $R8 $R7
        Goto loop
    done:
    ; Les dossiers remplaces ne doivent pas etre des jonctions non plus :
    ; RMDir /r viderait leur cible.
    !macro _NoJunction SUB
        System::Call 'kernel32::GetFileAttributesW(w "$INSTDIR\${SUB}") i .r1'
        IntOp $2 $1 & 0x400
        ${If} $1 != -1
        ${AndIf} $2 != 0
            StrCpy $R9 "$INSTDIR\${SUB}"
            MessageBox MB_ICONSTOP "$(Junction)" /SD IDOK
            Abort
        ${EndIf}
    !macroend
    !insertmacro _NoJunction "bin64"
    !insertmacro _NoJunction "data"
    !insertmacro _NoJunction "packages"
    !insertmacro _NoJunction "runtime"
    !insertmacro _NoJunction "service"
    !insertmacro _NoJunction "docs"
    !insertmacro _NoJunction "state"
FunctionEnd

Section "!$(SecGame)" SecMain
    SectionIn RO
    ; Verrou de l'installation (tenu par L4ZY.exe pendant le jeu).
    ${If} ${FileExists} "$INSTDIR\state\install.lock"
        System::Call 'kernel32::CreateFileW(w "$INSTDIR\state\install.lock", i 0xC0000000, i 0, p 0, i 3, i 0x80, p 0) p .r1'
        ${If} $1 == -1
            MessageBox MB_ICONSTOP "$(Running)" /SD IDOK
            Abort
        ${EndIf}
        System::Call 'kernel32::CloseHandle(p r1)'
    ${EndIf}
    Call CheckReparse

    ; Reinstallation : on remplace les fichiers distribues, pas les donnees.
    RMDir /r "$INSTDIR\bin64"
    RMDir /r "$INSTDIR\data"
    RMDir /r "$INSTDIR\packages"
    RMDir /r "$INSTDIR\runtime"
    RMDir /r "$INSTDIR\service"
    RMDir /r "$INSTDIR\docs"
    RMDir /r "$INSTDIR\state\update"
    Delete "$INSTDIR\L4ZY.exe"
    Delete "$INSTDIR\autoexec.cfg"
    Delete "$INSTDIR\traduction.cfg"

    SetOutPath "$INSTDIR"
    File /r "${TREE}\*.*"
    SetOutPath "$INSTDIR\state"
    File "${STATE}\installed.json"
    File "${STATE}\installed.ini"

    ; Reglages d'installation : ecrits une fois, jamais remplaces ensuite.
    ${IfNot} ${FileExists} "$INSTDIR\l4zy.ini"
        FileOpen $0 "$INSTDIR\l4zy.ini" w
        FileWrite $0 "; Reglages de cette installation L4ZY (conserves par les mises a jour).$\r$\n"
        FileWrite $0 "[install]$\r$\nchannel = ${CHANNEL}$\r$\n; channel_url = (vide = adresse du canal fournie par la version)$\r$\n"
        FileWrite $0 "[paths]$\r$\nportable = $Portable$\r$\n; profile = (vide = Documents\My Games\L4ZY)$\r$\n; userdata = (vide = %LOCALAPPDATA%\L4ZY)$\r$\n"
        ; Aucun modele telecharge automatiquement : le joueur choisit dans le
        ; jeu (Chat Translation -> Model), avec taille et progression.
        FileWrite $0 "[translation]$\r$\nstarter_model =$\r$\n"
        FileClose $0
    ${EndIf}

    WriteUninstaller "$INSTDIR\Uninstall-L4ZY.exe"
    ${If} $NoShortcuts == "0"
        CreateDirectory "$SMPROGRAMS\L4ZY"
        CreateShortcut "$SMPROGRAMS\L4ZY\L4ZY.lnk" "$INSTDIR\L4ZY.exe"
        CreateShortcut "$SMPROGRAMS\L4ZY\$(Rollback).lnk" "$INSTDIR\L4ZY.exe" "--rollback"
        CreateShortcut "$SMPROGRAMS\L4ZY\Uninstall L4ZY.lnk" "$INSTDIR\Uninstall-L4ZY.exe"
        WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\L4ZY" "DisplayName" "L4ZY"
        WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\L4ZY" "DisplayVersion" "${VERSION}"
        WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\L4ZY" "DisplayIcon" "$INSTDIR\L4ZY.exe"
        WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\L4ZY" "InstallLocation" "$INSTDIR"
        WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\L4ZY" "UninstallString" '"$INSTDIR\Uninstall-L4ZY.exe"'
        WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\L4ZY" "NoModify" 1
        WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\L4ZY" "NoRepair" 1
    ${EndIf}
SectionEnd

Section "$(SecDesk)" SecDesk
    ${If} $NoShortcuts == "0"
        CreateShortcut "$DESKTOP\L4ZY.lnk" "$INSTDIR\L4ZY.exe"
    ${EndIf}
SectionEnd

Section "Uninstall"
    ${If} ${FileExists} "$INSTDIR\state\install.lock"
        System::Call 'kernel32::CreateFileW(w "$INSTDIR\state\install.lock", i 0xC0000000, i 0, p 0, i 3, i 0x80, p 0) p .r1'
        ${If} $1 == -1
            MessageBox MB_ICONSTOP "$(Running)" /SD IDOK
            Abort
        ${EndIf}
        System::Call 'kernel32::CloseHandle(p r1)'
    ${EndIf}
    !insertmacro _NoJunction "bin64"
    !insertmacro _NoJunction "data"
    !insertmacro _NoJunction "packages"
    !insertmacro _NoJunction "runtime"
    !insertmacro _NoJunction "service"
    !insertmacro _NoJunction "docs"
    !insertmacro _NoJunction "state"
    RMDir /r "$INSTDIR\bin64"
    RMDir /r "$INSTDIR\data"
    RMDir /r "$INSTDIR\packages"
    RMDir /r "$INSTDIR\runtime"
    RMDir /r "$INSTDIR\service"
    RMDir /r "$INSTDIR\docs"
    RMDir /r "$INSTDIR\state"
    Delete "$INSTDIR\L4ZY.exe"
    Delete "$INSTDIR\autoexec.cfg"
    Delete "$INSTDIR\traduction.cfg"
    Delete "$INSTDIR\l4zy.ini"
    Delete "$INSTDIR\Uninstall-L4ZY.exe"
    RMDir "$INSTDIR"
    Delete "$DESKTOP\L4ZY.lnk"
    RMDir /r "$SMPROGRAMS\L4ZY"
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\L4ZY"
    MessageBox MB_ICONINFORMATION "$(KeepData)" /SD IDOK
SectionEnd
