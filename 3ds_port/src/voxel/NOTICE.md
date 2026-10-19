# Attribution

## pokeemerald-multiplatform

The voxel logic in this directory is derived from the SDL2/OpenGL voxel
renderer of `gradenGnostic/pokeemerald-multiplatform`
(`src/platform/voxel/`, commit db1cab3d2dc9f0e0a9f4a3d67983e5acf30a9a46),
whose port modifications are distributed under the MIT License:

    MIT License

    Copyright (c) 2026 pokeemerald-multiplatform contributors

    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the "Software"),
    to deal in the Software without restriction, including without limitation
    the rights to use, copy, modify, merge, publish, distribute, sublicense,
    and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
    THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
    DEALINGS IN THE SOFTWARE.

What was reused is the *logic*: map instance construction, metatile
classification, the 512x512 metatile atlas composition and the follow camera.
Every file here that touches the GPU is written against Citro3D.

## Free-roam camera

`voxel_freecam.{c,h}` and the camera rig in `voxel_camera.c` follow the
behaviour of the FirstPerson, ThirdPerson and FreeMove scripts of the voxel
mod (voxels_files.zip) - boom, look, blend and steering - written anew in C
over the decompilation's grid. `voxel_freemove.{c,h}` is original to this
port. The scripts' own license has not been confirmed: check it with their
author before a release that ships this code.
