# MDKVR

A small VR viewer for Windows, built with OpenXR and OpenGL.
It loads an OBJ model with a TGA texture and shows it in front of the HMD.

The OpenXR flow follows the official sample [hello_xr](https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/main/src/tests/hello_xr), but uses only the Win32 API and WGL, without the sample's platform/graphics plugin layers.

## Features

- Stereo rendering (4x MSAA) submitted to the OpenXR runtime as a projection layer
- View updated from the per-eye poses returned by `xrLocateViews` every frame
- Companion window on the desktop showing the left and right eye images side by side
- OBJ model loading (triangle and quad faces in `v/vt/vn`, `v//vn` and `v/vt` formats)
- 32-bit TGA texture loading (uncompressed and RLE)
- Shaders, model and texture embedded in the executable as resources

Controller rendering and input handling are not implemented yet.

## Requirements

- Windows 10 / 11 (x64)
- A GPU supporting OpenGL 3.3 or later
- An OpenXR runtime that supports `XR_KHR_opengl_enable` (for example SteamVR), set as the active runtime, and a compatible HMD

## Building

### Prerequisites

- Visual Studio 2022 (v143 toolset, "Desktop development with C++" workload)
- [GLEW](https://github.com/nigels-com/glew) (Windows binaries)
- [GLM](https://github.com/g-truc/glm)
- [OpenXR SDK Source](https://github.com/KhronosGroup/OpenXR-SDK-Source), built with CMake into `OpenXR/build` with the default static loader (`DYNAMIC_LOADER=OFF`)

To build the loader for both configurations:

```
cmake --build OpenXR/build --config Debug --target openxr_loader
cmake --build OpenXR/build --config Release --target openxr_loader
```

### Directory layout

Place the dependencies next to this repository:

```
projects/
├─ mdkvr/                     <- this repository
│   ├─ mdkvr.sln
│   └─ mdkvr/
├─ OpenXR/
│   └─ build/
│       ├─ include/openxr/    (openxr.h, openxr_platform.h)
│       └─ src/loader/
│           ├─ Debug/         (openxr_loaderd.lib)
│           └─ Release/       (openxr_loader.lib)
└─ thirdparty/
    ├─ glm/                   (glm/glm.hpp)
    └─ glew/
        ├─ include/           (GL/glew.h)
        ├─ lib/Release/x64/   (glew32.lib)
        └─ bin/Release/x64/   (glew32.dll)
```

### Steps

1. Open `mdkvr.sln` in Visual Studio.
2. Select `Debug | x64` or `Release | x64` and build.
3. A post-build event copies `glew32.dll` next to the executable (`x64\Debug` or `x64\Release`). The OpenXR loader is linked statically, so no extra DLL is needed.

From the command line:

```
msbuild mdkvr.sln /p:Configuration=Release /p:Platform=x64
```

## Running

1. Start your OpenXR runtime and make sure the HMD is detected.
2. Run `mdkvr.exe`.

A cube appears about 1.5 m in front of you at a height of 1.2 m.
The app uses the `STAGE` reference space (floor at y = 0). If the runtime does not provide it, it falls back to `LOCAL`, shifted down by 1.6 m.
If no runtime or HMD is available, the app shows an "OpenXR Error" message and exits.

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
| `mdkvview.cpp` | Window, OpenGL context, OpenXR session, swapchains and stereo rendering |
| `mdkvscene.cpp` | Loading and drawing the model, texture and shaders |

## License

[MIT License](LICENSE)

The dependencies (GLEW, GLM, OpenXR SDK) are not included in this repository.
Obtain and use them under their own licenses.
