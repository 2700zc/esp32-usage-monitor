' Hidden launcher for the agent bridge (used by the Startup shortcut).
'
' NOTE: this file must stay ASCII-only. Windows Script Host reads .vbs in the
' system ANSI codepage, not UTF-8 -- non-ASCII comments get mis-decoded and can
' break the statements below (symptom: "Object required: 'fso'" on a comment
' line). The .bat next door is fine with UTF-8; this one is not.
Set fso = CreateObject("Scripting.FileSystemObject")
Set sh  = CreateObject("WScript.Shell")
here = fso.GetParentFolderName(WScript.ScriptFullName)
sh.Run "cmd /c """ & here & "\bridge-daemon.bat""", 0, False
