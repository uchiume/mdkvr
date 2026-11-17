# MDKVR

A small VR viewer for Windows, built with OpenVR (SteamVR) and OpenGL.
It loads an OBJ model with a TGA texture and shows it in front of the HMD.

The rendering flow follows the official OpenVR sample [hellovr_opengl](https://github.com/ValveSoftware/openvr/tree/master/samples/hellovr_opengl), but uses only the Win32 API and WGL instead of SDL.

## Features

- Stereo rendering (4x MSAA) submitted to the SteamVR compositor
- View updated from the HMD pose every frame
- Companion window on the desktop showing the left and right eye images side by side
- OBJ model loading (triangle and quad faces in `v/vt/vn`, `v//vn` and `v/vt` formats)
- 32-bit TGA texture loading (uncompressed and RLE)
- Shaders, model and texture embedded in the executable as resources

Controller rendering and input handling are not implemented yet.

## Requirements

- Windows 10 / 11 (x64)
- A GPU supporting OpenGL 3.3 or later
- SteamVR and a SteamVR-compatible HMD

## Building

### Prerequisites

- Visual Studio 2022 (v143 toolset, "Desktop development with C++" workload)
- [GLEW](https://github.com/nigels-com/glew) (Windows binaries)
- [GLM](https://github.com/g-truc/glm)
- [OpenVR SDK](https://github.com/ValveSoftware/openvr)

### Directory layout

Place the dependencies next to this repository:

```
projects/
├─ mdkvr/                   <- this repository
│   ├─ mdkvr.sln
│   └─ mdkvr/
├─ openvr/
│   ├─ headers/             (openvr.h)
│   ├─ lib/win64/           (openvr_api.lib)
│   └─ bin/win64/           (openvr_api.dll)
└─ thirdparty/
    ├─ glm/                 (glm/glm.hpp)
    └─ glew/
        ├─ include/         (GL/glew.h)
        ├─ lib/Release/x64/ (glew32.lib)
        └─ bin/Release/x64/ (glew32.dll)
```

### Steps

1. Open `mdkvr.sln` in Visual Studio.
2. Select `Debug | x64` or `Release | x64` and build.
3. A post-build event copies `glew32.dll` and `openvr_api.dll` next to the executable (`x64\Debug` or `x64\Release`).

From the command line:

```
msbuild mdkvr.sln /p:Configuration=Release /p:Platform=x64
```

## Running

1. Start SteamVR and make sure the HMD is detected.
2. Run `mdkvr.exe`.

A cube appears about 1.5 m in front of you at a height of 1.2 m.
If SteamVR is not running, the app shows a "VR_Init Failed" message and exits.

## Changing the model

The model and texture live in `mdkvr/res/` and are embedded via `mdkvr.rc`.

| File | Description |
| --- | --- |
| `res/cube.txt` | Model in OBJ format |
| `res/Cube.tga` | 32-bit TGA texture |
| `res/scene.vert`, `res/scene.frag` | Shaders for the model |
| `res/companion.vert`, `res/companion.frag` | Shaders for the companion window |

Replace the files and rebuild.
The model's position and size are set by `m_Model` in `mdkvscene.cpp`, and the light position by `m_LightPos`.

## Source files

| File | Role |
| --- | --- |
| `mdkvr.cpp` | Entry point and message loop |
| `mdkvview.cpp` | Window, OpenGL context, OpenVR setup and stereo rendering |
| `mdkvscene.cpp` | Loading and drawing the model, texture and shaders |

## License

[MIT License](LICENSE)

The dependencies (GLEW, GLM, OpenVR SDK) are not included in this repository.
Obtain and use them under their own licenses.
