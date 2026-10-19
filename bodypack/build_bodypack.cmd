@echo off
rem Builds tldcoop_bodies (story characters as walking bodies) from YOUR OWN Episode 5 install.
rem   1. stage the needed Episode 5 bundles   (tools\stage_bundles.py)
rem   2. AssetRipper export                    (done once; rip\ep5_export)
rem   3. copy characters + clips into a Unity project (tools\collect_assets.py)
rem   4. Unity 6000.0.60f1 headless build      (bodypack\Editor\BuildBodies.cs)
rem The result contains Hinterland's art: for your own use and friends who own the game. Never publish it.
setlocal
set ROOT=%~dp0..
set UNITY=D:\Unity\6000.0.60f1\Editor\Unity.exe
set EXPORT=%ROOT%\rip\ep5_export\ExportedProject\Assets
set PROJECT=%ROOT%\bodypack\project

if not exist "%UNITY%" (echo Unity 6000.0.60f1 not found at %UNITY% & exit /b 1)
if not exist "%EXPORT%" (echo AssetRipper export not found at %EXPORT% & exit /b 1)

if not exist "%PROJECT%\Assets" (
  echo Creating Unity project...
  "%UNITY%" -batchmode -nographics -quit -createProject "%PROJECT%" -logFile "%ROOT%\bodypack\create.log" || (echo Project creation failed, see bodypack\create.log & exit /b 1)
)

python -I "%ROOT%\tools\collect_assets.py" "%EXPORT%" "%ROOT%\bodypack\bodies.json" "%PROJECT%\Assets" || exit /b 1
copy /y "%ROOT%\bodypack\bodies.json" "%PROJECT%\bodies.json" >nul
if not exist "%PROJECT%\Assets\Editor" mkdir "%PROJECT%\Assets\Editor"
copy /y "%ROOT%\bodypack\Editor\BuildBodies.cs" "%PROJECT%\Assets\Editor\" >nul
if not exist "%PROJECT%\Assets\BodyPackShaders" mkdir "%PROJECT%\Assets\BodyPackShaders"
copy /y "%ROOT%\bodypack\Shaders\*.shader" "%PROJECT%\Assets\BodyPackShaders\" >nul
rem The remote player's ground struggle while an animal has them ("down"), one per rig.
for %%G in (M F) do (
  if not exist "%PROJECT%\Assets\AnimalStruggle\%%G" mkdir "%PROJECT%\Assets\AnimalStruggle\%%G"
  copy /y "%ROOT%\bodypack\Clips\AnimalStruggle\%%G\down.anim" "%PROJECT%\Assets\AnimalStruggle\%%G\" >nul || exit /b 1
)

echo Building bundle (first run imports textures; can take several minutes)...
"%UNITY%" -batchmode -nographics -quit -projectPath "%PROJECT%" -executeMethod TLDCoop.BuildBodies.Build -logFile "%ROOT%\bodypack\build.log"
if errorlevel 1 (echo Unity build failed, see bodypack\build.log & exit /b 1)
findstr /c:"[BodyPack]" "%ROOT%\bodypack\build.log"
copy /y "%PROJECT%\Build\tldcoop_bodies" "%ROOT%\build\tldcoop_bodies.bundle" >nul || exit /b 1
echo Done: build\tldcoop_bodies.bundle
