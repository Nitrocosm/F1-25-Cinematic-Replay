# Third-party notices

## Swofty SDK

`cinecam.dll` is built against the Swofty Launcher SDK headers (`packages/sdk/include`), parts of which are
compiled into it.

```
MIT License

Copyright (c) 2026 Jacob Nardella (Swofty)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Compiler runtime

`cinecam.dll` is compiled with zig, which links parts of LLVM's libc++, libc++abi and libunwind (Apache License
2.0 with LLVM Exceptions) and the mingw-w64 runtime (Zope Public License 2.1, and public domain) into it.

## ffmpeg (not included)

Rendering runs [ffmpeg](https://ffmpeg.org) as a separate program. It isn't part of this addon: the user
installs it (the Render menu's Install button uses WinGet to install the Gyan.dev build, `Gyan.FFmpeg`, which
is licensed under the GPL version 3).
