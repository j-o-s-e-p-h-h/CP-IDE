# CP IDE

A desktop IDE for competitive programming on Windows. A problem arrives from the
[Competitive Companion](https://github.com/jmerle/competitive-companion) browser
extension, folders and files are created for it, you solve it in Python or C++
with a Monaco editor, run all sample tests with one click, submit to the judge,
and keep a verdict history. Everything is free, nothing is paywalled.

- **C++ core** with a native window through [webview/webview](https://github.com/webview/webview) (WebView2 on Windows).
- **Monaco** editor with VS Code Dark+/Light+ colours and rainbow bracket pairs.
- Four layouts: Default, Note-taking, Debug, Focus Mode. Dark by default, light toggle.
- Per-problem timer (stopwatch / countdown, presets, pause, reset), notes, breakpoints, tests.
- Local runs for Python, C++ (`g++ -O2 -std=c++23`), Java (`javac` + `java`) and JavaScript (`node`) with PASS / FAIL / TLE / RE verdicts.
- Stress testing with `gen.py` + `brute.py`, a real debugger (Python through `bdb`, C++ through gdb/MI).
- Judge submission behind one interface: Codeforces implemented (login + submit + verdict polling),
  AtCoder / CSES / USACO / HackerRank fall back to a browser submit with the code on the clipboard.
- New sessions: blank, three random unsolved Codeforces problems by rating, or from a URL. Left unnamed, a session is called `Session — Sep 13` (the date) or `Random 1200–1500` (the band).

## Starting up

A splash covers the window while Monaco loads, then every launch lands on the **home screen**:
resume the session you were in, reopen a recent contest, start a session, paste a problem URL,
and see whether Competitive Companion is being listened for and which compilers were found.
`Esc` (or Resume) goes on to the editor; the logo in the top bar and contest menu → Home bring
it back.

## Install (users)

Download `CP-IDE-Setup-<version>.exe` from the Releases page and run it. It installs CP IDE
(about 8 MB), adds Start Menu / desktop shortcuts, and fetches the Microsoft Edge WebView2
runtime if the machine does not have it (Windows 11 always does). On first start the app opens
a setup page that checks for Python, g++, Java and Node, offers one-click installs for what is
missing, and lets you log in to the judges.

## Build from source

Requirements on the build machine:

- Windows 10/11 with the Microsoft Edge WebView2 runtime (preinstalled on Windows 11).
- MSYS2 UCRT64 toolchain: `pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-gdb`
  (g++ 15.x at `C:\msys64\ucrt64\bin`).
- Python 3 on `PATH` (used to run Python solutions, generators and the debugger helper).

```powershell
git clone <this repo> cp-ide
cd cp-ide
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\bin\cp-ide.exe
```

All third-party code is vendored in `third_party/` (webview, WebView2 SDK headers + loader DLL,
nlohmann/json, a trimmed Monaco build, KaTeX), so no network access is needed to build.

To build the installer, install [Inno Setup 6](https://jrsoftware.org/isinfo.php)
(`winget install JRSoftware.InnoSetup`), re-run `cmake -B build`, then:

```powershell
cmake --build build --target installer     # -> installer\out\CP-IDE-Setup-0.1.0.exe
```

Codeforces sits behind an anti-bot check that rejects plain HTTP clients; statement fetching
falls back to the `curl.exe` that ships with Windows, which gets through. Auto-submit uses the
same in-process client and may be blocked too; the browser fallback always works.

Then install Competitive Companion in your browser and make sure port **10045** is in its
port list (it is by default). Click the green plus on a problem or contest page and it shows
up as a tab.

### Linux

```sh
sudo apt install g++ cmake ninja-build libwebkit2gtk-4.1-dev   # Debian/Ubuntu; Fedora: webkit2gtk4.1-devel
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bin/cp-ide                      # or: sudo cmake --install build   (adds a menu entry)
```

Clipboard for the browser fallback uses `wl-copy`, `xclip` or `xsel`, whichever is installed.
The judge windows use your WebKitGTK session, so logins persist like in a browser.

### macOS

```sh
xcode-select --install                  # clang++
brew install cmake ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build                     # -> build/bin/cp-ide.app
```

The C++ debugger needs gdb, which macOS does not have (lldb is not supported yet); the Python
debugger works. Linux and macOS builds are compiled from the same sources but have had far less
testing than Windows: please report what breaks.

## Where things live on disk

Default data root is `%USERPROFILE%\cp` (override with the `CP_IDE_HOME` environment variable).

```
cp/
  config.json                 tool paths + Codeforces credentials
  state.json                  last contest, layout, theme, pane sizes
  templates/main.cpp ...      your boilerplate, copied into every new problem
  history.json                every submission verdict
  contests/<contest>/contest.json
  contests/<contest>/<problem>/
      main.py  main.cpp  Main.java  main.js   your solutions (start empty)
      gen.py   brute.py       stress-test generator and reference
      tests/1.in 1.out ...    sample tests, custom1.in/.out for your own
      problem.json            name, URL, limits, fetched statement HTML
      state.json              timer, verdict flags, notes, breakpoints
```

Contests are listed, reopened and deleted (folder removed) from the contest menu in the top bar.

### Joining a live or virtual contest

Two ways, both of which create one contest folder with every problem in it:

- **Paste the contest URL** into the `+` box in the top bar or the box on the home screen —
  `https://codeforces.com/contest/2009` or `https://atcoder.jp/contests/abc319`. Every problem
  is imported with its statement, limits and sample tests. Works for a round that is running,
  upcoming or long finished, and for virtual participation.
- **Competitive Companion**: open the contest page in your browser and click the green **+**
  there; it sends the whole batch at once.

When Codeforces publishes a countdown for the round (live or virtual), the remaining time
appears next to the contest name in the top bar and turns red in the last 15 minutes.

### Where does an imported problem go?

- A whole-contest click (Companion batch) goes into a contest folder named after the contest.
- A single problem goes into the currently open session; if a contest is open instead,
  it goes into the contest folder for that problem's contest (created if needed).

## Submitting

**Codeforces submits from inside the app.** The first time you press Submit, a "Codeforces" window
opens inside CP IDE: log in there once (it is a real browser session, so Cloudflare checks and
captchas work). After that, Submit navigates that window to the problem's submit page, fills the
form with your code and compiler, waits for Cloudflare's Turnstile check to pass (it needs the
window on screen, so the window shows for a few seconds), presses Submit, and polls
`api/user.status` until the verdict arrives. The verdict lands in History and on the tab. If
Codeforces wants something from you (login, an interactive check, "same code submitted before"),
the window stays open with a message and you finish the step there. Open it any time from the
Submissions tab ("Codeforces account…") to log in or out.

Compiler ids are in `cp/config.json` (`pythonProgramTypeId` 31, `cppProgramTypeId` 91,
`javaProgramTypeId` 87, `jsProgramTypeId` 55; 70 = PyPy 3.10, 89 = C++20).

**AtCoder, CSES and USACO** work the same way, each in its own in-app window: log in once, then
Submit fills the site's form (AtCoder: task + language ids from `cp/config.json`, `atcoder.*LangId`;
CSES and USACO: language picked by name, code attached as the file the form wants) and reads the
verdict from the site's status page. USACO's result parsing is best effort. **HackerRank** uses the
browser fallback: Submit copies your code to the clipboard and opens the problem page.

`programTypeId` values are Codeforces compiler ids (31 = Python 3, 91 = C++23 GCC 14, 87 = Java 21, 55 = Node.js; 70 = PyPy 3.10, 89 = C++20).

## Compilers and interpreters

The setup page (contest menu → Setup) shows what was found and lets you change it:

- **C++**: every g++ and clang++ on the machine is listed; pick one or enter any path.
  The compile line is `<compiler> <flags> main.cpp -o sol.exe` with the flags editable
  (default `-O2 -std=c++23`). MSVC (`cl.exe`) is not supported: it needs its own environment.
- **Python, Java, Node**: auto-detected from PATH and the usual install folders; "Path…" pins a
  specific executable.

The same values live in `cp/config.json` (`python`, `cppCompiler`, `cppFlags`, `javac`, `java`, `node`).
An empty value means auto-detect.

## Shortcuts

Press `F1` in the app (or the `?` button in the top bar) for this list.

| Key | Does |
| --- | --- |
| `Ctrl+Enter` | Run all tests |
| `Ctrl+Shift+Enter` | Submit |
| `Ctrl+.` | Stop the run |
| `Alt+←` / `Alt+→` | Previous / next problem |
| `Alt+1` … `Alt+9` | Jump to a problem |
| `Ctrl+Alt+C` | Copy the whole solution |
| `Ctrl+Shift+T` | Add a test case |
| `Ctrl+B` | Show / hide the test panel |
| `Ctrl+ +` / `Ctrl+ -` / `Ctrl+0` | Editor font size |
| `Ctrl+Shift+F` | Focus Mode |
| `Esc` | Leave Focus Mode / close menus |

Click a line number to toggle a breakpoint (used by Debug ▶ in the Debug layout). `F5` and
`Ctrl+R` are deliberately inert: a reload mid-contest would throw away the editor's undo history.

## Test cases

Codeforces packs its whole sample into one block of `t` test cases, but tags every line with
the case it belongs to — so the import splits it into one test per case, each with its own
expected output and `1` as its count line. A verdict then names the case that failed instead of
pointing at a wall of text. Problems with a single case, and judges that do not tag their
samples, are left as they are. Refreshing a statement re-reads the samples; cases you added
yourself are kept.

Each case has a ▶ that runs only that one, a ⧉ that copies its input, and its wall-clock time
next to the verdict. A failing case shows the first line and token where your output and the
expected answer part ways. Leaving **Expected** empty is allowed: the case then reports `OUT`
with whatever your program printed, and ✓ turns that output into the expected answer.

Both the statement/editor split and the height of the test panel are draggable, and
double-clicking a divider puts it back to the layout default.

## Code templates

New problems start from `cp/templates/` (`main.cpp`, `main.py`, `Main.java`, `main.js`), which
is empty until you put something there. Contest menu → Setup → **Code templates** edits them,
with a starter for each language one click away. Existing problems are untouched; `↺` above the
editor pulls the template into the file you are looking at.

## Repository layout

```
src/            C++ core: window + RPC bridge, HTTP server, storage, runner, judges, stress, debugger
ui/             the web UI (index.html, style.css, app.js) served from 127.0.0.1:10045
tools/          cp_debug.py (Python debugger helper), make_icon.py (rebuilds src/res/cp-ide.ico from the logo)
third_party/    vendored dependencies
```

## License

MIT, see `LICENSE`. Monaco, webview and nlohmann/json keep their own licenses in `third_party/`.
