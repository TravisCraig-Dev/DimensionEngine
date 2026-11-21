#!/bin/bash
# build_scripts.sh: compiles Scripts/ into .dimension/ScriptAssemblies/GameScripts.dll (what the editor does on save),
# for running the scene with the Player straight from the command line.
cd "$(dirname "$0")/.." || exit 1
dotnet build Scripts/GameScripts.csproj -c Debug -o .dimension/ScriptAssemblies --nologo -v q -clp:NoSummary
