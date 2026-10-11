# The native build on Windows

The native build (native/NATIVE.md) for 64 bit Windows: the same game C++ and the same platform layer (`src/platform/native/`),
built with [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw)'s clang (UCRT), SDL3's MinGW development release and an LGPL
shared FFmpeg. Everything Windows needs differently is under `_WIN32` (or in a file of its own), so the macOS and Linux builds stay
as they were.

## What you need

| What | Where from |
| --- | --- |
| LLVM-MinGW (UCRT) | `winget install MartinStorsjo.LLVM-MinGW.UCRT` |
| CMake and Ninja | `winget install Kitware.CMake Ninja-build.Ninja` (requirements.txt's ninja works too) |
| Python 3.10 or later and the split | README_DECOMP.md, "Building on Windows": the virtual environment, `tools/split.py` |
| PS2SDK's toolchain | README_DECOMP.md, "Building on Windows" (its headers lay the retail data out: native/tools/convert_data.py) |
| SDL3 (MinGW) and FFmpeg | `native/tools/build_windows.py` fetches SDL3 into `build/native/deps` and builds our decode-only LGPL FFmpeg (`native/tools/build_ffmpeg.sh` in Git Bash, with an MSYS `make` it fetches from MSYS2's repository) into `build/native/ffmpeg/windows-x86_64`; without Git Bash it falls back to a prebuilt LGPL shared FFmpeg (every decoder: 90 MB more) |

## Building and running

    .venv\Scripts\python.exe native\tools\build_windows.py
    build\native\cmake\CrashTwinsanity.exe --iso "C:\path\to\Crash Twinsanity (Europe).iso"

`build_windows.py` finds LLVM-MinGW (on the PATH, `$LLVM_MINGW`, or where winget puts it), configures `build/native/cmake` the
first time (`--configure` again, `--clean` from nothing) and builds. The program, SDL's and FFmpeg's DLLs and the UI's fonts end
up together in `build/native/cmake`. Without `--iso`, the disc image is `$TWINSANITY_ISO`, local.json's `disc_image` (in the
working folder), the one picked before, or asked for with a file dialog.

It's a windowed program: its log goes to the terminal it was started from, or to whatever its output was given (a pipe, a
file), else to `log.txt` in the settings folder. `-DTWIN_WINDOWS_CONSOLE=ON` makes a console program instead.

The settings, saves (`memorycard/`), bindings and texture packs are in `%APPDATA%\Crash Twinsanity\`.

## What Windows needed

| Where | Why | What |
| --- | --- | --- |
| Bitfields (`-mno-ms-bitfields`, native/CMakeLists.txt and native/tools/host.py) | MinGW lays bitfields out the Microsoft way by default: a field of another type starts a new unit. The game's structs (and the data it reads from its files over them) are GCC's | GCC's layout, as on every other host; the layout tools are given the same flag |
| Narrow return values (gcc2.h's `CallNarrowed`, convert_data.py's `mark_narrow_returns`) | The retail vtables' calls read 32 bits (`CallVirtual<u32>`) of functions the C++ declares returning `bool`, `u8` or `u16`. The PS2's and Apple's arm64 callees extend those to the whole register; x86-64's set the low byte and leave the rest. `Sm2WantPolicy::DoesNotWant` answered true at depth 0 and the beach's loader was freed while still the focus | The data converter marks each vtable entry of such a function in its `index` (only virtual bases use it, none of the game's: 0x1000 u8/bool, 0x2000 s8, 0x3000 u16, 0x4000 s16); `CallVirtual` calls those through a pointer of the narrow type, so the compiler extends it. 88 entries |
| Mixed float and integer arguments (abi.h's `TWIN_EABI_CC`) | The retail code passes integers and floats each in their own registers in their order (GCC's EABI), and the C++ declares the functions it calls through its pointers with the floats wherever the EABI lets them be (`PushNode(f32 strength, ObjectNode* node, ...)` called as `(node, 0.0f, other)`). AAPCS64 and System V do the same, so macOS and Linux need nothing; Windows' x64 convention gives an argument the register of its position whatever its kind | `RETAIL_N32`'s functions (the 434 the PS2 build thunks) take the System V convention (`__attribute__((sysv_abi))`). A call through the retail vtables and pointers (`Abi::CallEabi`) goes the way of the function behind it: a vtable's slot can hold one taking the float and one leaving it out (`Agent::Launch`'s, called with the gravity, the velocity and the target: `BasicAgent::Launch` takes only the velocity, and crashed the first boss), which the EABI passes alike. native/tools/sysv_functions.py lists `RETAIL_N32`'s functions for a table of their addresses (src/platform/native/abi_win32.cpp); one of them is called the System V way, any other Windows' own with a mixed call's floats left out. native/tools/check_conventions.py (in the build) checks every function taking floats with its integers is `RETAIL_N32`'s; `--selftest-abi` tries the three cases, `$TWIN_ABI_TRACE` logs each function called across |
| Data alignment (convert_data.py) | x86-64's ABI gives a global array of 16 bytes or more 16 byte alignment and clang uses aligned SSE stores on one the C++ declares (`g_MsStreamStates`: `movdqa`); the converter's `alignas` of less lowered it | Objects of 16 bytes or more are 16 byte aligned |
| `long` (convert_data.py) | 32 bits on Windows (LLP64): pointer-sized integers are `long long` there | The converter's table knows it, and takes a scalar's written type (`uiptr`) before its canonical one |
| The retail code's null pointers (`src/platform/native/lowmemory_win32.cpp`) | Windows never maps the first 64 KB | A vectored exception handler runs a copy of the faulting load or store with its displacement moved by `g_NativeLowMemory`'s address, then jumps back past it (no single step: debuggers see nothing). The executable is linked below 2 GB at a fixed base so that address fits a displacement. Also the crash report (registers, the stack's return addresses as module+offset) |
| Frame pacing (display.cpp, entry.cpp) | Windows' sleeps are as coarse as its timer (up to 15.6 ms late): 50 Hz came out at about 40 | `SDL_DelayPrecise` for the vertical blank's wait, `timeBeginPeriod(1)` at start-up |
| The disc image (disc.cpp) | No `pread` | `ReadFile` with an `OVERLAPPED` offset (the threads reading it share no position), the path as UTF-8 |
| Files (`std::filesystem`) | An open file can't be removed | The non-throwing calls; the self-tests close what they remove |
| DMA addresses (ee/address.cpp) | The renderer's 32 bit DMA addresses take a slot per 64 MB window of host memory, and an unused slot held 0: the same as the first window's base. macOS and Linux never map anything below 4 GB; Windows has the stack and heaps there, and a tag pointing into them took a slot that was never kept (later given to another window: the chain read other memory) | A slot's window is kept with bit 0 set (bases are 64 MB apart) |
| The level titles' pictures (shapes.cpp's `Shape2D::Read`, a retail bug) | A picture's sprite reads its texture unless one of its ID is loaded, then its material. A level's title picture has the texture of the icons' title of the level, loaded with the icons, so its material was read from the texture's bytes (a name 16 MB long): a crash on entering the first boss's area (the level sweep never shows a title) | The texture's bytes skipped when it's loaded already, and referenced as a read one is (`$TWIN_SWEEP_PICTURES` reads every picture OLEG loads) |
| Pointers to instances that are gone (instances.cpp, a retail use after free) | An object node keeps its AgentRef1 and AgentRef2 after their instances are freed; the retail code tells a gone one by the asleep flag its memory still says, which on the PS2 it does long enough. Natively (bigger structures, the memory used again sooner) the flag read another object's data: Cortex carrying his object into the caverns' next chunk while he and Crash roll and fight crashed | A set of the instances made and not destroyed: a reference to one that's gone let go of where the retail code reads it (`ForgetFreedInstance`, logged the first times) |
| An animation of no length (animation.cpp) | Its position is 0 / 0: the host's NaN, an index of 0x80000000, the frame 4 GB on (the PS2's FPU has no NaN, its 32 bit addresses wrap: the values before the frames read). A crash in the caverns | Its first frame; the frames' addresses wrap at 32 bits as the PS2's |
| A gamepad's sticks (ui/bindings.cpp's `Squared`) | The DualShock 2's stick reaches its square's corners; a modern gamepad's reports a circle, a full diagonal 0.71 of each axis (an Xbox Series pad's; an Xbox One pad's goes a little past the circle). The game takes its dead zones off each axis and runs past a push it measures on them: on a diagonal an Xbox Series pad's Crash walked | Each stick's point moved out along its direction so the circle's edge is the square's (an axis's own push unchanged), as PCSX2 scales its axes by 1.33. The keyboard's diagonals were the square's corners already. `--selftest-ui` checks it |
| A crash's report | Where the game was | The player's chunk and the save's after the backtrace |
| stb_truetype (`stbtt_PackFontRangesRenderIntoRects`) | The missing glyph was remembered by its index in its own range's table and copied into later ranges' by it: past a shorter table's end (a crash now and then making the overlay's font atlas) | Its data remembered instead (a change of the bundled header's, marked) |
| The rest | No `posix_memalign`, `localtime_r`, `setenv`; `GLsizeiptr` was `long` | `malloc` (16 byte aligned on Windows), `Native::LocalTime`, `Native::SetEnvironment`, `ptrdiff_t` |

## Speed

The renderer is the PS2's own (native/GRAPHICS.md): its DMA chains run on an emulation of the VIF, VU1 and GS, the GS's primitives
drawn by OpenGL. At the beach (60 Hz, the default) it went from 23 frames a second to the full 60 on the machine it was measured on (an
i7-8700K): the render thread (the GS's side, on the CPU: OpenGL's driver is about 2% of it) went from over 40 ms a frame to about
13.5 in play (65000 primitives in 227 batches). Each change gives the same pictures, checked by dumps of the same frames with and
without it (`$TWIN_FIXED_TIME`, below):

| Where | What | Switch |
| --- | --- | --- |
| `Gs::PagesOfRect` (gsmemory.cpp) | The pages a rectangle meets by page sized regions when the buffer starts on a page (a pixel's address every 8 pixels otherwise) | `$TWIN_GS_PAGES_BY_PIXELS` for the old way, `$TWIN_GS_CHECK_PAGES` compares the two (random rectangles at start, then every call) |
| `Hw::Gl::GetUniformLocation` (hw/gl.cpp) | The answers kept, by program and name, per thread: the hardware renderer's passes asked for their uniforms by name thousands of times a frame | `$TWIN_HW_UNIFORM_CACHE=0` |
| `Gs::DrawPrimitive` (gsdraw.cpp) | The primitive's DrawState kept while no register it's made from is written (`Gs::stateGeneration_`) | `$TWIN_GS_STATE_CACHE=0` |
| `HwGs::RectPages`, the batch key (hwgs.cpp) | The loops' bounds worked out once; the key compared and copied inline | - |
| `Gs::DrawPrimitive`'s state copy, the batch key (gsdraw.cpp, hwgs.cpp) | Copied inline (the C library's memcpy was 5% of the thread); the key copied only when it changes | - |
| The pixels' addresses (gs/gsaddress.h) | Inline, in a header of their own (the transfers worked one out per pixel through a call into gsmemory.cpp) | - |
| The compiler's target | x86-64-v2 (SSE4.2, POPCNT: what Windows 11 asks of a processor): floor and ceil inline; no FMA, so the same results | - |
| `HwGs::DecodeOnGpu` (hwgs.cpp) | A texture looked up again only when a register was written or the CLUT changed (`Gs::stateGeneration`); a complete one checked a page at a time against the pages' changes rather than a tile at a time | `$TWIN_HW_NO_TEXTURE_GENERATION`, `$TWIN_HW_NO_PAGE_CHECK` |
| `HwGs::Draw` (hwgs.cpp) | A mesh's next primitive drawn as the last one was while nothing it depended on changed (`DrawLikeLast`: the GS's state, the CLUT, the targets, the settings, the texture good as it was): no key, targets or texture worked out again, only its pages made current and its vertexes added. A quarter of the render thread's time | `$TWIN_HW_NO_FAST_DRAW` |
| `WaitForVBlank` (display.cpp) | `SDL_DelayPrecise` | - |

ThinLTO over the emulation's files (`-DTWIN_EMULATION_LTO=ON`) was measured no faster, and is off.

The threads: VIF1 and VU1 run on a thread of their own between the game's and the render thread (renderthread.h, three stages,
the default): the game's thread queues VIF1's words, the GIF's transfers and the GS's work in their order, the VU stage runs
VIF1 and VU1 and hands it all on to the render thread's queue, so the GS gets the same data in the same order. Where the scenes
are big the game's thread went from about 17.5 ms a frame to 9 (VU1 was half of it): the Totem's beach, the school's boiler room
and rooftops went from 57 to 60 frames a second. `$TWIN_RENDER_THREAD=gs` puts VIF1 and VU1 back on the game's thread.
`$TWIN_FRAME_STATS`' `game` is the game's thread's work between its waits for the vertical blank.

Comparing two builds' pictures: `$TWIN_FIXED_TIME=1` with `$TWINSANITY_AUDIO=frames` (the sound made in step with the frames)
gives the same frames run after run (with the sound in real time, a run's speed changes what the game does with it after a
few hundred frames).

## Languages

The PAL game has five: English, French, German, Italian and Spanish, each with its texts (the archive's Language folders), its
voices (CRASH6\<LANGUAGE>.MB and .MH, loaded once at start-up) and its sound in the story's movies (a channel each). The PS2
took the language from its own settings once, at start-up; the native build takes Windows' (system.cpp's
`Platform::System::Language`), or the one Options > Gameplay > language names (options.txt's `language`, 1-5), from the game's
next start.

## Mods

A file in the settings folder's `mods` folder (`%APPDATA%\Crash Twinsanity\mods`) at the path of one of the game's files is read
instead of it, in any case, whether it's one of the disc's (`mods\FMV\H01_A.PSS`, the music's banks) or one of its archive's
(CRASH6\CRASH.BD: `mods\Levels\Earth\Hub\beach.rm2`, `mods\Language\AgentLab\English.txt`, the fonts, the startup
files...). The log says each one used (`[native] mods: ...`). `CrashTwinsanity.exe --list-files` lists the
archive's files and `--extract=PATH` copies one (or one of the disc's) into the mods folder to be changed (commands.cpp);
`native/tools/iso_extract.py IMAGE [--archive] --list` and `iso_extract.py IMAGE [--archive] PATH OUT` do the same without the
game. The disc.cpp side: a
disc file's mod is given a place past 16 GB (a GB each, its sectors still 32 bit for the I/O processor), an archive file's one
past the image's end within 4 GB of the archive's start (filestream.cpp's `FileStreamOpen` takes its start and size), and
`Native::Disc::Read` reads them from the files. The game's own code does what it would with the original (a file it doesn't
ask for can't be added: the archive's table is the game's list).

## Frame rates past 60

Options > Display > refresh rate has 120 Hz, 144 Hz and unlimited past the game's 50 and 60 (`refresh_60hz` 2, 3 and 4 in
options.txt; NTSC's timing). The game's clock is its frames' real time (clock.cpp: the PS2's 50 and 60 Hz both play at one
speed), so it plays at the same speed at any rate; the steps a frame's time is capped at (a frame far slower than the PS2's
isn't taken whole) stay two 60ths of a second. What counts in 60ths of a second a frame (the particles' emitters, their events'
wheel, the particles each makes a frame; the decals' lives) is done once for each 60th gone by past 60 Hz (`NativeUi::GameTicks`),
as at 60 Hz once a frame, so effects last as long and are as dense; the particles' places are by the time every frame. The
movies keep the sound processor's clock. Unlimited, with v-sync off, ran at about 140 frames a second on the beach, 123 at the
first boss's and 91 on the Totem's beach (the heaviest) on the i7-8700K it was measured on.

## A runaway VU program

VU1's model program (the one at address 0: a list of commands, each a handler) never ended in some frames: after Crash dies on
the beach, in cutscenes, and all the time at the first boss's (hundreds in a couple of minutes). The cause was the data's
conversion, on every host: a retail label two files declare differently got the last declaration's type, and the converter
wrote only that much of it. `D_002EC39C` is `s16 g_ShaderType0BEntries[2]` in shaders.cpp (the program's entry and its
entry for clipped triangles, 0x74) and `s16 g_ShaderType0BSize` in vuprograms.cpp: two bytes were written, the clipped entry
read the next object's padding, 0. A triangle crossing the guard band went to the clipper (0x92 on), which ends by calling the
command's clipped entry: the model handler (0x2E4) itself, which drew the model again, met the triangle again, for ever. Shader
types 0x10 and 0x11 (`D_002EC398`, `D_002EC3C0`) were the same. A worse one: `DecalUnusedInt` is a `u32` to decals.h and the
stream's 2048 byte `g_ScratchBuffer` to stream.cpp, and only 4 bytes were made: the stream's reads overran into other data.
native/tools/data_types.py keeps a label's widest declaration now (a sized array over a scalar, the larger count first, an
array without its size last), so the converter takes in the labels after it (as it does for any type longer than its label).

The guard stays: a program past 2 million instructions (the game's never run more than about 20000: `$TWIN_VU_LONGEST` says
the longest; `$TWIN_VU_RUNAWAY` sets it, 0 never) is stopped and said (`[native] vu: a program started at ... stopped`), the frame
going on without the rest of its work. To look at one: `$TWIN_VU_DUMP=FOLDER` writes the VU's memories and registers
(vu_code.bin, vu_data.bin, vu_registers.txt: native/tools/vudecode.py disassembles them), `$TWIN_VIF_HISTORY=FOLDER` VIF1's last
256 codes with their registers and data (vif_history_N.txt), `$TWIN_VU_TRACE_RUNAWAY` (with `$TWIN_VU_TRACE_STEPS`) the
instructions it goes round, each with the integer registers.

## Debugging

- A crash prints the registers and the return addresses: `llvm-symbolizer --obj=build/native/cmake/CrashTwinsanity.exe -f -i
  0x10000000+OFFSET` gives the lines (native/tools/level_sweep.py does it for its logs).
- LLDB (LLVM-MinGW's) runs it: `lldb build\native\cmake\CrashTwinsanity.exe`.
- `$TWIN_DEBUG_HEAP`: a small allocation freed twice or not at an entry's start, and a free outside the game's pool, said with
  where it came from.
- `$TWIN_PIXEL_WATCH=X,Y` (and `$TWIN_PIXEL_WATCH_VERTEXES`): every primitive drawn over a pixel of the PS2's picture, with its
  state and vertexes.
- `native/tools/level_sweep.py OUT` plays every level chunk headless (native/LEVEL_SWEEP.md); on Windows it isolates each run's
  settings (`APPDATA`) and names the functions of a crash's backtrace with llvm-symbolizer. `$TWIN_SWEEP_TRACE` logs its progress
  every 5 seconds.
- `$TWIN_PROFILE=FILE` (`$TWIN_PROFILE_DELAY`, default 15 s, `$TWIN_PROFILE_SECONDS`, default 20): a sampling profiler of every
  thread (src/platform/native/profiler_win32.cpp); `native/tools/profile_report.py FILE` gives each busy thread's functions
  (`--lines` by source line).
- `$TWIN_FIXED_TIME=1`: the test runs' time (debug.h), every frame a frame's time: with `$TWIN_DUMP_FRAMES=FOLDER:N` (every Nth
  picture as a PNG), two builds' pictures can be compared frame by frame; with `$TWINSANITY_AUDIO=frames` too, runs agree
  frame for frame (the sound in real time changes a run after some 700 frames).
- `$TWIN_FRAME_STATS`: each frame's time, VU1's work, the primitives, the render thread's time and the GPU renderer's batches, texture decodes, uploads, downloads and copies.

## Releasing

    .venv\Scripts\python.exe native\tools\build_windows.py --configure
    .venv\Scripts\python.exe native\tools\package_windows.py

`package_windows.py` makes `build/native/dist/Crash Twinsanity/` and `Crash-Twinsanity-Windows.zip`: the program, the
DLLs, the fonts, the licences and native/PLAYING_WINDOWS.md as README.txt. The program's icon is native/icon/AppIcon.ico (the box
art, 16 to 256 pixels; `native/tools/make_icon.py` still makes one from the HUD's Crash head for the Mac build).

The program is built with the retail executable's data (`build/native/data.cpp`, from the split) and VU1's programs translated
from its microcode compiled in: a build is for its builder's own use, made from their own disc, and isn't to be shared. A
release that reads those from the player's disc image at start-up instead is the next step.
