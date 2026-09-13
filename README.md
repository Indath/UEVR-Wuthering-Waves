# UEVR ![build](https://github.com/praydog/UEVR/actions/workflows/dev-release.yml/badge.svg)

Universal Unreal Engine VR Mod WUWA
## Credits & Acknowledgements

* **Original Project:** Based on the [Universal Unreal Engine VR Mod (UEVR)](https://github.com/praydog/UEVR) by **[praydog](https://github.com/praydog)**.
* **Special Thanks:** Thanks to praydog and the UEVR community for creating the foundational framework that made stereoscopic 3D injection possible.
* **Custom Changes in This Repository:**
  * Added view-offset patches for *Wuthering Waves*.
  * Adjusted rendering pipeline parameters for custom onfigurations of UE4.26.

## Supported Engine Versions
Unkowwn

## Getting Started

Before launching, ensure you have installed .NET 6.0 SDK. It should tell you where to install it upon first open, but if not, you can [download it from here](https://dotnet.microsoft.com/en-us/download/dotnet/6.0). Most people should click x64 in the top left table, under the Installers column, next to windows.

1. Launch UEVRInjector.exe (or custom Injector)
2. Select your desired runtime (OpenVR/OpenXR) and settings
3. Launch the target game


## To-dos before injection

1. Disable HDR (it will still work without it, but the game will be darker than usual if it is)
2. Start as administrator if the game is not visible in the list
3. Pass `-nohmd` to the game's command line and/or delete VR plugins from the game directory if the game contains any existing VR plugins
4. Disable any overlays that may conflict and cause crashes (Rivatuner, ASUS software, Razer software, Overwolf, etc...)
5. Disable graphical options in-game that may cause crashes or severe issues like DLSS Frame Generation
6. Consider disabling `Hardware Accelerated GPU Scheduling` in your Windows `Graphics settings`

## In-Game Menu

Press the **Insert** key or **L3+R3** on an XInput based controller to access the in-game menu, which opens by default at startup. With the menu open, hold **RT** for various shortcuts:

- RT + Left Stick: Move the camera left/right/forward/back
- RT + Right Stick: Move the camera up/down
- RT + B: Reset camera offset
- RT + Y: Recenter view
- RT + X: Reset standing origin


