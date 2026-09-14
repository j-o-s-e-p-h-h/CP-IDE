// cp-ide-wx: a native front end built on the same core as cp-ide.
//
// Nothing here is a rewrite — storage, the runner, the toolchain and the problem
// model are the exact files the web build uses. Only the UI differs: wxWidgets
// gives real platform controls on Windows, GTK and macOS, and wxStyledTextCtrl
// (Scintilla) is the editor. The two binaries build side by side, so this one can
// grow without the working app ever breaking.
#include <wx/wx.h>
#include <map>
#include <wx/splitter.h>
#include <wx/stc/stc.h>
#include <wx/statline.h>
#include <wx/image.h>
#include <wx/utils.h>
#include <wx/clipbrd.h>
#include <wx/textdlg.h>

#include <atomic>
#include <thread>

#include "http_client.hpp"
#include "model.hpp"
#include "runner.hpp"
#include "statement.hpp"
#include "storage.hpp"
#include "util.hpp"
#include "webview/webview.h"
#ifdef _WIN32
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {

fs::path dataRoot() {
  std::string env = util::envVar("CP_IDE_HOME");
  if (!env.empty()) return util::upath(env);
  std::string home = util::envVar(util::kWindows ? "USERPROFILE" : "HOME");
  if (home.empty()) home = util::kWindows ? "C:\\" : "/tmp";
  return util::upath(home) / "cp";
}

// The web build's palette. wx uses real platform controls, so this cannot be
// applied everywhere — but leaving half the window default grey around a dark
// editor looks worse than either choice, so everything that accepts a colour
// gets one, and the controls that refuse (notebook tabs) are not used.
// Values taken straight from ui/style.css so the two builds match.
struct Theme {
  wxColour bg{24, 24, 24};          // --bg     #181818
  wxColour panel{34, 34, 34};       // --panel  #222222
  wxColour panel2{42, 42, 42};      // --panel2 #2a2a2a
  wxColour border{54, 54, 54};      // --border #363636
  wxColour text{236, 236, 236};     // --text   #ececec
  wxColour muted{154, 154, 154};    // --muted  #9a9a9a
  wxColour codebg{29, 29, 29};      // --codebg #1d1d1d
  wxColour accent{44, 187, 93};     // --accent #2cbb5d
  wxColour ok{44, 187, 93}, bad{239, 71, 67};
  wxColour kw{197, 134, 192}, type{78, 201, 176}, str{206, 145, 120}, num{181, 206, 168};
  wxColour comment{106, 153, 85}, fn{220, 220, 170};
};
// Not const: the theme toggle swaps the whole palette, exactly like the web
// build's :root[data-theme="light"] block.
Theme T;

Theme DarkTheme() { return Theme(); }

// ui/style.css :root[data-theme="light"], value for value.
Theme LightTheme() {
  Theme t;
  t.bg = wxColour(244, 245, 247);
  t.panel = wxColour(255, 255, 255);
  t.panel2 = wxColour(241, 242, 244);
  t.border = wxColour(226, 228, 232);
  t.text = wxColour(28, 28, 30);
  t.muted = wxColour(107, 114, 128);
  t.codebg = wxColour(250, 250, 250);
  t.ok = wxColour(31, 158, 77);
  t.bad = wxColour(217, 47, 42);
  // Light syntax colours (VS Code light+), so the editor is not dark-on-white.
  t.kw = wxColour(0, 0, 255);
  t.type = wxColour(38, 127, 153);
  t.str = wxColour(163, 21, 21);
  t.num = wxColour(9, 134, 88);
  t.comment = wxColour(0, 128, 0);
  t.fn = wxColour(121, 94, 38);
  return t;
}

// The web UI's .pane: a rounded 1px-bordered card on the window background.
class Card : public wxPanel {
 public:
  explicit Card(wxWindow* parent, const wxColour& fill = T.panel)
      : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), fill_(fill) {
    SetBackgroundColour(T.bg);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
      wxPaintDC dc(this);
      wxSize s = GetClientSize();
      dc.SetBrush(wxBrush(T.bg));
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.DrawRectangle(0, 0, s.GetWidth(), s.GetHeight());
      dc.SetBrush(wxBrush(fill_));
      dc.SetPen(wxPen(T.border));
      dc.DrawRoundedRectangle(0, 0, s.GetWidth(), s.GetHeight(), 10);
    });
  }

 private:
  wxColour fill_;
};

// A flat, colourable stand-in for the controls that ignore SetBackgroundColour.
class Pill : public wxPanel {
 public:
  Pill(wxWindow* parent, const wxString& label, std::function<void()> onClick)
      : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), onClick_(std::move(onClick)) {
    SetLabel(label);
    SetBackgroundColour(T.panel2);
    SetForegroundColour(T.muted);
    Bind(wxEVT_PAINT, &Pill::OnPaint, this);
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { if (onClick_) onClick_(); });
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; Refresh(); });
    SetCursor(wxCursor(wxCURSOR_HAND));
  }

  // The web UI's button shapes: .tab (outlined), .btn-run (outlined, panel2 fill),
  // .btn-submit (solid accent), and the plain text link used for "delete".
  enum class Style { Tab, Button, Primary, Link, Underline };

  void SetStyle(Style s) { style_ = s; Refresh(); }
  void SetDot(const wxColour& c, bool on) { dot_ = c; hasDot_ = on; InvalidateBestSize(); SetMinSize(DoGetBestSize()); Refresh(); }
  void SetMono(bool on) {
    mono_ = on;
    if (on) SetFont(wxFont(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD));
    InvalidateBestSize();
    SetMinSize(DoGetBestSize());
  }
  void SetActive(bool on) { active_ = on; Refresh(); }
  // The web UI's two inline SVGs: ICON_CLOCK on the timer, ICON_PLAY on Run.
  // No installed font carries either, so they are drawn.
  enum class Glyph { None, Clock, Play };
  void SetGlyph(Glyph g) { glyph_ = g; InvalidateBestSize(); SetMinSize(DoGetBestSize()); Refresh(); }
  void SetAccent(const wxColour& c, bool use) { accent_ = c; useAccent_ = use; Refresh(); }
  void SetText(const wxString& s) {
    SetLabel(s);
    InvalidateBestSize();
    SetMinSize(DoGetBestSize());
    Refresh();
  }

 protected:
  wxSize DoGetBestSize() const override {
    wxClientDC dc(const_cast<Pill*>(this));
    dc.SetFont(GetFont());
    wxSize t = dc.GetTextExtent(GetLabel());
    int pad = style_ == Style::Link ? 8 : style_ == Style::Primary ? 36 : 22;
    return wxSize(t.GetWidth() + pad + (hasDot_ ? 13 : 0) + (glyph_ != Glyph::None ? 18 : 0), t.GetHeight() + 13);
  }

 private:
  void OnPaint(wxPaintEvent&) {
    wxPaintDC dc(this);
    wxSize s = GetClientSize();
    dc.SetBrush(wxBrush(GetParent()->GetBackgroundColour()));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, s.GetWidth(), s.GetHeight());

    wxColour fg = T.muted;
    if (style_ == Style::Primary) {
      dc.SetBrush(wxBrush(T.accent));
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.DrawRoundedRectangle(0, 0, s.GetWidth(), s.GetHeight(), 6);
      fg = *wxWHITE;
    } else if (style_ == Style::Link) {
      fg = hover_ ? T.text : T.muted;
    } else if (style_ == Style::Underline) {
      // .ptab: no box, just a 2px accent rule under the active one
      fg = active_ || hover_ ? T.text : T.muted;
      if (active_) {
        dc.SetBrush(wxBrush(T.accent));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.DrawRectangle(0, s.GetHeight() - 2, s.GetWidth(), 2);
      }
    } else {
      // .tab / .btn-run: 1px border, panel2 fill when active or a button
      bool filled = style_ == Style::Button || active_;
      dc.SetBrush(wxBrush(filled ? T.panel2 : GetParent()->GetBackgroundColour()));
      dc.SetPen(wxPen(hover_ ? (style_ == Style::Tab && !active_ ? T.accent : T.muted) : (active_ ? T.muted : T.border)));
      dc.DrawRoundedRectangle(0, 0, s.GetWidth() - 1, s.GetHeight() - 1, 6);
      fg = active_ || style_ == Style::Button ? T.text : T.muted;
    }
    if (useAccent_) fg = accent_;

    dc.SetFont(GetFont());
    dc.SetTextForeground(fg);
    wxSize t = dc.GetTextExtent(GetLabel());
    int tx = (s.GetWidth() - t.GetWidth() - (hasDot_ ? 13 : 0) + (glyph_ != Glyph::None ? 18 : 0)) / 2;
    dc.DrawText(GetLabel(), tx, (s.GetHeight() - t.GetHeight()) / 2);
    if (glyph_ == Glyph::Clock) {
      int cy = s.GetHeight() / 2, cx = tx - 11;
      dc.SetBrush(*wxTRANSPARENT_BRUSH);
      dc.SetPen(wxPen(fg, 1));
      dc.DrawCircle(cx, cy, 6);
      dc.DrawLine(cx, cy - 3, cx, cy);
      dc.DrawLine(cx, cy, cx + 3, cy);
    } else if (glyph_ == Glyph::Play) {
      int cy = s.GetHeight() / 2, cx = tx - 14;
      wxPoint tri[3] = {{cx, cy - 5}, {cx, cy + 5}, {cx + 8, cy}};
      dc.SetBrush(wxBrush(fg));
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.DrawPolygon(3, tri);
    }
    if (hasDot_) {  // the solved/attempted dot the web UI puts on a problem tab
      dc.SetBrush(wxBrush(dot_));
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.DrawCircle(tx + t.GetWidth() + 7, s.GetHeight() / 2, 4);
    }
  }

  std::function<void()> onClick_;
  bool active_ = false, hover_ = false, useAccent_ = false, hasDot_ = false, mono_ = false;
  Glyph glyph_ = Glyph::None;
  Style style_ = Style::Tab;
  wxColour accent_, dot_;
};

// wxSplitterWindow paints its sash with the system face colour, which leaves a
// light grey bar between the dark cards. The web UI has an 8px gap of --bg there.
class DarkSplitter : public wxSplitterWindow {
 public:
  DarkSplitter(wxWindow* parent, long style)
      : wxSplitterWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, style) {
    SetBackgroundColour(T.bg);
    SetSashSize(8);
  }

 protected:
  void DrawSash(wxDC& dc) override {
    wxSize s = GetClientSize();
    dc.SetBrush(wxBrush(T.bg));
    dc.SetPen(*wxTRANSPARENT_PEN);
    if (GetSplitMode() == wxSPLIT_VERTICAL)
      dc.DrawRectangle(GetSashPosition(), 0, GetSashSize(), s.GetHeight());
    else
      dc.DrawRectangle(0, GetSashPosition(), s.GetWidth(), GetSashSize());
  }
};

// The web UI's .chip: a small rounded badge, not plain text.
class Chip : public wxPanel {
 public:
  Chip(wxWindow* parent, const wxString& label, bool link = false)
      : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), link_(link) {
    SetLabel(label);
    SetBackgroundColour(T.panel);
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
      wxPaintDC dc(this);
      wxSize s = GetClientSize();
      dc.SetBrush(wxBrush(T.panel));
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.DrawRectangle(0, 0, s.GetWidth(), s.GetHeight());
      dc.SetBrush(wxBrush(T.panel2));
      dc.DrawRoundedRectangle(0, 0, s.GetWidth(), s.GetHeight(), s.GetHeight() / 2.0);
      dc.SetFont(GetFont());
      dc.SetTextForeground(link_ && hover_ ? T.text : T.muted);
      wxSize t = dc.GetTextExtent(GetLabel());
      dc.DrawText(GetLabel(), (s.GetWidth() - t.GetWidth()) / 2, (s.GetHeight() - t.GetHeight()) / 2);
    });
    if (link) {
      SetCursor(wxCursor(wxCURSOR_HAND));
      Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { hover_ = true; Refresh(); });
      Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { hover_ = false; Refresh(); });
    }
  }

 protected:
  wxSize DoGetBestSize() const override {
    wxClientDC dc(const_cast<Chip*>(this));
    dc.SetFont(GetFont());
    wxSize t = dc.GetTextExtent(GetLabel());
    return wxSize(t.GetWidth() + 20, t.GetHeight() + 8);
  }

 private:
  bool link_ = false, hover_ = false;
};

// The web UI's .fbox: a rounded, bordered well around a borderless text control.
class Box : public wxPanel {
 public:
  Box(wxWindow* parent, const wxString& value, bool readOnly, int height)
      : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, height), wxBORDER_NONE) {
    SetBackgroundColour(T.panel);
    // wxTE_NO_VSCROLL: a Win32 scrollbar inside the control cannot be recoloured,
    // and the web UI's boxes have none either — they wrap and grow instead.
    long st = wxTE_MULTILINE | wxTE_BESTWRAP | wxTE_NO_VSCROLL | wxBORDER_NONE | (readOnly ? wxTE_READONLY : 0);
    text_ = new wxTextCtrl(this, wxID_ANY, value, wxDefaultPosition, wxDefaultSize, st);
    text_->SetBackgroundColour(T.codebg);
    text_->SetForegroundColour(T.text);
    text_->SetFont(wxFont(9, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL));
    auto* s = new wxBoxSizer(wxVERTICAL);
    s->Add(text_, 1, wxEXPAND | wxALL, 6);   // inset so the rounded border shows
    SetSizer(s);
    // A floor low enough that three stacked boxes still fit a short panel, so
    // the sizer splits what is left evenly instead of starving the last one.
    text_->SetMinSize(wxSize(-1, 14));
    SetMinSize(wxSize(-1, 26));
    Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
      wxPaintDC dc(this);
      wxSize sz = GetClientSize();
      dc.SetBrush(wxBrush(T.panel));
      dc.SetPen(*wxTRANSPARENT_PEN);
      dc.DrawRectangle(0, 0, sz.GetWidth(), sz.GetHeight());
      dc.SetBrush(wxBrush(T.codebg));
      dc.SetPen(wxPen(T.border));
      dc.DrawRoundedRectangle(0, 0, sz.GetWidth() - 1, sz.GetHeight() - 1, 6);
    });
  }
  wxTextCtrl* Text() { return text_; }

 private:
  wxTextCtrl* text_ = nullptr;
};

// A Pill that opens a menu: stands in for wxChoice, which is a real Win32 combo
// box and ignores colours, leaving three light boxes on a dark bar.
class MenuPill : public Pill {
 public:
  MenuPill(wxWindow* parent, std::function<void(int)> onPick)
      : Pill(parent, "", nullptr), onPick_(std::move(onPick)) {
    Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { Popup(); });
  }

  void SetItems(const std::vector<wxString>& items, int sel) {
    items_ = items;
    sel_ = sel;
    if (!face_.empty()) return;   // an icon button keeps its glyph
    SetText(sel >= 0 && sel < (int)items_.size() ? items_[sel] + Caret() : wxString(L"—"));
  }
  // Show a fixed glyph instead of the chosen item, like the web UI's layout icon.
  void SetFace(const wxString& glyph) { face_ = glyph; SetText(glyph); }
  int GetSelection() const { return sel_; }
  // A narrow UTF-8 literal appended to a wxString is converted with the locale,
  // which turns the caret into mojibake; build it as UTF-8 explicitly.
  static wxString Caret() { return wxString(L"   \u25BE"); }

 private:
  void Popup() {
    if (items_.empty()) return;
    wxMenu m;
    for (size_t i = 0; i < items_.size(); ++i) {
      auto* it = m.AppendCheckItem(1000 + (int)i, items_[i]);
      if ((int)i == sel_) it->Check(true);
    }
    m.Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
      int i = e.GetId() - 1000;
      if (i < 0 || i >= (int)items_.size()) return;
      sel_ = i;
      if (face_.empty()) SetText(items_[i] + Caret());
      if (onPick_) onPick_(i);
    });
    PopupMenu(&m, 0, GetSize().GetHeight());
  }

  std::function<void(int)> onPick_;
  std::vector<wxString> items_;
  wxString face_;
  int sel_ = -1;
};

const char* kCppKeywords =
    "alignas alignof auto bool break case catch char class const constexpr continue decltype default delete do "
    "double else enum explicit export extern false float for friend goto if inline int long mutable namespace new "
    "noexcept nullptr operator private protected public register return short signed sizeof static static_assert "
    "struct switch template this throw true try typedef typeid typename union unsigned using virtual void volatile while";
const char* kCppTypes =
    "vector string pair map set queue deque stack bitset array tuple size_t int64_t uint64_t ll pii ld "
    "priority_queue unordered_map unordered_set multiset multimap cin cout endl std";

// Every string from the core is UTF-8. Implicit conversion would use the current
// locale and mangle anything non-ASCII (contest names are full of em dashes).
inline wxString U(const std::string& s) { return wxString::FromUTF8(s.c_str(), s.size()); }

// Where the vendored KaTeX and the rest of the runtime assets sit next to the exe.
fs::path assetDir() {
  fs::path exe = util::exeDir();
  std::error_code ec;
  for (auto cand : {exe / "ui", exe.parent_path() / "ui", exe.parent_path().parent_path() / "ui"})
    if (fs::exists(cand / "katex" / "katex.min.css", ec)) return cand;
  return exe / "ui";
}

// Statement styling for the embedded view. Deliberately small: the judges ship
// their own markup, this only has to make it readable on a dark background.
const char* kStmtCss = R"CSS(
:root{--bg:#181818;--panel:#222;--border:#363636;--text:#ececec;--muted:#9a9a9a;--code:#1d1d1d}
html,body{margin:0;background:var(--bg);color:var(--text);
  font:14px/1.65 -apple-system,'Segoe UI',Helvetica,Arial,sans-serif}
.wrap{padding:16px 20px 40px}
h1,h2,h3,.section-title{font-size:13px;font-weight:600;text-transform:uppercase;letter-spacing:.6px;
  color:var(--muted);margin:20px 0 8px}
p{margin:0 0 12px;text-wrap:pretty}
pre{background:var(--code);border:1px solid var(--border);border-radius:6px;padding:10px 12px;
  font-family:Consolas,monospace;font-size:13px;white-space:pre-wrap;overflow-x:auto}
img{max-width:100%;height:auto;display:block;margin:8px auto}
ul,ol{margin:0 0 12px;padding-left:22px}
table{border-collapse:collapse;margin:8px 0}td,th{border:1px solid var(--border);padding:4px 8px}
hr{border:none;border-top:1px solid var(--border);margin:18px 0}
var{font-style:normal;font-family:Consolas,monospace}
.io-format{background:var(--code);border:1px solid var(--border);border-radius:6px;padding:10px 12px;
  font-family:Consolas,monospace;font-size:13px;white-space:pre-wrap;line-height:1.7;margin:0 0 12px}
/* Codeforces' own problem block, styled the way ui/style.css does it. Without
   these the header collapses into one bare line per field and the title is the
   wrong size, which is most of what makes the pane look different. */
.problem-statement{font-family:Verdana,Arial,sans-serif;font-size:14px;line-height:1.5}
.header{margin-bottom:1em}
.header .title{font-size:150%;font-weight:700;text-align:center;margin-bottom:.4em;
  color:var(--text);text-transform:none;letter-spacing:0}
.header .time-limit,.header .memory-limit,.header .input-file,.header .output-file{text-align:center}
.property-title{display:inline;font-weight:700}
.property-title::after{content:': '}
.section-title{font-weight:700;font-size:115%;margin:1.2em 0 .5em;text-transform:none;
  letter-spacing:0;color:var(--text)}
.sample-tests .title{background:var(--panel);border-bottom:1px solid var(--border);font-weight:700;
  padding:.3em .6em;font-size:13px}
.sample-tests .input,.sample-tests .output{border:1px solid var(--border);border-radius:6px;
  overflow:hidden;margin-bottom:.6em}
.sample-tests pre{margin:0;padding:.5em .6em;border:none;border-radius:0}
.sample-tests .input-output-copier{display:none}
.test-example-line{display:block}
.test-example-line-odd{background:rgba(236,236,236,.06)}
.note .section-title{margin-top:1.2em}
center{display:block;text-align:center;margin:.6em 0}
.tex-font-style-bf{font-weight:600}.tex-font-style-it{font-style:italic}
.tex-font-style-tt{font-family:Consolas,monospace}
.tex-graphics{max-width:100%}
.muted{color:var(--muted)}
/* thin dark scrollbar, like the web UI's */
::-webkit-scrollbar{width:10px;height:10px}
::-webkit-scrollbar-thumb{background:#4a4a4a;border-radius:5px}
::-webkit-scrollbar-track{background:transparent}
)CSS";

// Same delimiter order and \color handling the web UI needed: Codeforces writes
// display maths as six dollars, and \color{red}{x} must colour only x.
const char* kKatexBoot = R"JS(
document.addEventListener('DOMContentLoaded', function () {
  if (!window.renderMathInElement) return;
  try {
    renderMathInElement(document.body, {
      delimiters: [
        {left:'$$$$$$', right:'$$$$$$', display:true},
        {left:'$$$',    right:'$$$',    display:false},
        {left:'$$',     right:'$$',     display:true},
        {left:'\\[',    right:'\\]',    display:true},
        {left:'\\(',    right:'\\)',    display:false}
      ],
      colorIsTextColor: true,
      throwOnError: false
    });
  } catch (e) {}
});
)JS";

}  // namespace

// ---------------------------------------------------------------- main frame
class MainFrame : public wxFrame {
 public:
  explicit MainFrame(Storage& st)
      : wxFrame(nullptr, wxID_ANY, "CP IDE", wxDefaultPosition, wxSize(1180, 760)), storage_(st) {
    tc_ = Toolchain::fromConfig(storage_.config());
    LoadUiPrefs();   // must run before anything reads T
#ifdef _WIN32
    // Without this the window keeps a white title bar above a black app.
    BOOL dark = light_ ? FALSE : TRUE;
    DwmSetWindowAttribute((HWND)GetHandle(), 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
#endif
    BuildUi();
    LoadContests();
    SetStmtTab(0);
    SetPanelTab(0);
    CallAfter([this] { ApplyLayout(); });
  }

  ~MainFrame() override {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
    if (wv_) { webview_destroy(wv_); wv_ = nullptr; }
  }

 private:
  // ------------------------------------------------------------------- layout
  void BuildUi() {
    SetBackgroundColour(T.panel);
    auto* root = new wxPanel(this);
    root->SetBackgroundColour(T.panel);
    auto* col = new wxBoxSizer(wxVERTICAL);

    // --- top bar, in the web UI's .topbar order:
    //     logo | contest ▾ | tabs | Run  Submit | timer | History | theme
    auto* barPanel = new wxPanel(root);
    barPanel->SetBackgroundColour(T.panel);
    auto* bar = new wxBoxSizer(wxHORIZONTAL);

    {
      wxImage::AddHandler(new wxPNGHandler());
      wxImage img;
      // The white logo disappears on a light bar; the web build swaps it too.
      fs::path logo = assetDir() / "assets" / (light_ ? "logo.png" : "logo-white.png");
      std::error_code ec;
      if (fs::exists(logo, ec) && img.LoadFile(U(util::pstr(logo)), wxBITMAP_TYPE_PNG)) {
        int h = 26, w = img.GetWidth() * h / std::max(1, img.GetHeight());
        img = img.Scale(w, h, wxIMAGE_QUALITY_HIGH);
        auto* bmp = new wxStaticBitmap(barPanel, wxID_ANY, wxBitmap(img));
        bmp->SetBackgroundColour(T.panel);
        bar->Add(bmp, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 12);
      }
    }

    contestChoice_ = new MenuPill(barPanel, [this](int i) { OpenContest(i); });
    contestChoice_->SetStyle(Pill::Style::Link);
    bar->Add(contestChoice_, 0, wxALIGN_CENTER_VERTICAL);

    // Problem tabs: one pill per problem, exactly like .tab
    tabRow_ = new wxPanel(barPanel);
    tabRow_->SetBackgroundColour(T.panel);
    tabSizer_ = new wxBoxSizer(wxHORIZONTAL);
    tabRow_->SetSizer(tabSizer_);
    bar->Add(tabRow_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    auto* addProb = new Pill(barPanel, "+", [this] { ImportUrl(); });
    addProb->SetStyle(Pill::Style::Tab);
    bar->Add(addProb, 0, wxALIGN_CENTER_VERTICAL);

    bar->AddStretchSpacer();
    runBtn_ = new Pill(barPanel, "Run", [this] { RunAll(); });
    runBtn_->SetStyle(Pill::Style::Button);
    runBtn_->SetGlyph(Pill::Glyph::Play);
    bar->Add(runBtn_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    submitBtn_ = new Pill(barPanel, "Submit", [this] { OnSubmit(); });
    submitBtn_->SetStyle(Pill::Style::Primary);
    bar->Add(submitBtn_, 0, wxALIGN_CENTER_VERTICAL);
    bar->AddStretchSpacer();

    timerPill_ = new Pill(barPanel, "00:00", [this] { ToggleTimer(); });
    timerPill_->SetStyle(Pill::Style::Tab);
    timerPill_->SetMono(true);
    timerPill_->SetGlyph(Pill::Glyph::Clock);
    bar->Add(timerPill_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    // ⊞ layout switcher, then History, then the theme toggle — the web UI's order.
    layoutPill_ = new MenuPill(barPanel, [this](int i) { SetLayout(i); });
    layoutPill_->SetStyle(Pill::Style::Button);
    layoutPill_->SetItems({"Default", "Note-taking", "Debug", "Wide statement"}, layout_);
    layoutPill_->SetFace(wxString(L"⊞"));
    bar->Add(layoutPill_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    auto* hist = new Pill(barPanel, "History", [this] { ShowHistory(); });
    hist->SetStyle(Pill::Style::Button);
    bar->Add(hist, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    themePill_ = new Pill(barPanel, wxString(light_ ? L"◐" : L"◑"), [this] { ToggleTheme(); });
    themePill_->SetStyle(Pill::Style::Button);
    bar->Add(themePill_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    auto* help = new Pill(barPanel, "?", [this] { ShowShortcuts(); });
    help->SetStyle(Pill::Style::Button);
    bar->Add(help, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

    barPanel->SetSizer(bar);
    barPanel->SetMinSize(wxSize(-1, 50));
    col->Add(barPanel, 0, wxEXPAND);

    // Per-problem stopwatch, like the web UI's timer.
    clock_.SetOwner(this);
    Bind(wxEVT_TIMER, &MainFrame::OnTick, this);
    clock_.Start(1000);

    // --- body: statement | (editor above, tests below) — the web UI's Default layout
    auto* outer = new DarkSplitter(root, wxSP_LIVE_UPDATE);
    outer_ = outer;

    // Left card: Description / Submissions tabs, chips, then the statement view.
    auto* stmtCard = new Card(outer);
    auto* sc = new wxBoxSizer(wxVERTICAL);
    auto* stabs = new wxBoxSizer(wxHORIZONTAL);
    tabDesc_ = new Pill(stmtCard, "Description", [this] { SetStmtTab(0); });
    tabSubs_ = new Pill(stmtCard, "Submissions", [this] { SetStmtTab(1); });
    for (auto* t : {tabDesc_, tabSubs_}) { t->SetStyle(Pill::Style::Underline); t->SetBackgroundColour(T.panel); }
    stabs->Add(tabDesc_, 0, wxLEFT, 8);
    stabs->Add(tabSubs_, 0);
    sc->Add(stabs, 0, wxTOP, 4);

    chipRow_ = new wxPanel(stmtCard);
    chipRow_->SetBackgroundColour(T.panel);
    chipSizer_ = new wxBoxSizer(wxHORIZONTAL);
    chipRow_->SetSizer(chipSizer_);
    sc->Add(chipRow_, 0, wxEXPAND | wxLEFT | wxTOP | wxBOTTOM, 14);

    stmtHost_ = new wxPanel(stmtCard);
    stmtHost_->SetBackgroundColour(T.bg);
    stmtHost_->Bind(wxEVT_SIZE, &MainFrame::OnStmtSize, this);
    sc->Add(stmtHost_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 1);
    stmtCard->SetSizer(sc);

    auto* split = new DarkSplitter(outer, wxSP_LIVE_UPDATE);
    split_ = split;

    // Editor card with the web UI's "</> Code" header.
    auto* edCard = new Card(split, T.codebg);
    auto* ec = new wxBoxSizer(wxVERTICAL);
    auto* eh = new wxBoxSizer(wxHORIZONTAL);
    auto* codeLbl = new wxStaticText(edCard, wxID_ANY, "</>  Code");
    codeLbl->SetForegroundColour(T.muted);
    codeLbl->SetBackgroundColour(T.codebg);
    eh->Add(codeLbl, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 14);
    langChoice_ = new MenuPill(edCard, [this](int) { SwitchLanguage(); });
    langChoice_->SetStyle(Pill::Style::Tab);
    {
      langChoice_->SetItems(LangLabels(), 0);
    }
    eh->Add(langChoice_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
    eh->AddStretchSpacer();
    auto* fmtBtn = new Pill(edCard, "{}", [this] { NotYet("Formatting"); });
    auto* copyBtn = new Pill(edCard, wxString(L"\u29C9"), [this] { CopyCode(); });
    auto* resetBtn = new Pill(edCard, wxString(L"\u21BA"), [this] { ResetCode(); });
    auto* stress = new Pill(edCard, "Stress", [this] { NotYet("Stress testing"); });
    for (auto* b : {fmtBtn, copyBtn, resetBtn, stress}) { b->SetStyle(Pill::Style::Button); eh->Add(b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6); }
    ec->Add(eh, 0, wxEXPAND | wxTOP | wxBOTTOM, 10);
    editor_ = MakeEditor(edCard);
    ec->Add(editor_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 1);
    edCard->SetSizer(ec);
    right_ = new Card(split);
    right_->SetBackgroundColour(T.panel);
    auto* rcol = new wxBoxSizer(wxVERTICAL);

    // Panel header: "Testcase | Test Result", like the web UI's .tests-head
    auto* head = new wxPanel(right_);
    head->SetBackgroundColour(T.panel);
    auto* hs = new wxBoxSizer(wxHORIZONTAL);
    tabCase_ = new Pill(head, wxString(L"\u2611  Testcase"), [this] { SetPanelTab(0); });
    tabResult_ = new Pill(head, ">_  Test Result", [this] { SetPanelTab(1); });
    for (auto* t : {tabCase_, tabResult_}) t->SetStyle(Pill::Style::Underline);
    hs->Add(tabCase_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 6);
    auto* tsep = new wxStaticText(head, wxID_ANY, "|");   // the web UI's .tsep
    tsep->SetForegroundColour(T.border);
    tsep->SetBackgroundColour(T.panel);
    hs->Add(tsep, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 4);
    hs->Add(tabResult_, 0, wxALIGN_CENTER_VERTICAL);
    // "5 cases" sits right after the tabs in the web UI, not out at the edge.
    caseCount_ = new wxStaticText(head, wxID_ANY, "");
    caseCount_->SetForegroundColour(T.muted);
    caseCount_->SetBackgroundColour(T.panel);
    hs->Add(caseCount_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
    summary_ = new wxStaticText(head, wxID_ANY, "");
    summary_->SetForegroundColour(T.muted);
    summary_->SetBackgroundColour(T.panel);
    hs->Add(summary_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 12);
    hs->AddStretchSpacer();
    collapsePill_ = new Pill(head, wxString(L"⌄"), [this] { ToggleTests(); });
    collapsePill_->SetStyle(Pill::Style::Button);
    collapsePill_->SetToolTip("Collapse the panel (Ctrl+B)");
    hs->Add(collapsePill_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    head->SetSizer(hs);
    head->SetMinSize(wxSize(-1, 38));
    rcol->Add(head, 0, wxEXPAND);

    // Verdict headline, then what actually went wrong, then the cases.
    verdict_ = new wxStaticText(right_, wxID_ANY, "");
    wxFont vf = verdict_->GetFont();
    vf.SetPointSize(vf.GetPointSize() + 3);
    vf.SetWeight(wxFONTWEIGHT_BOLD);
    verdict_->SetFont(vf);
    verdict_->SetForegroundColour(T.text);
    verdict_->Hide();
    rcol->Add(verdict_, 0, wxLEFT | wxRIGHT | wxTOP, 12);

    why_ = new wxStaticText(right_, wxID_ANY, "");
    why_->SetForegroundColour(T.bad);
    why_->Hide();
    rcol->Add(why_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

    // Case pills, drawn by us so they can be dark and carry a verdict mark.
    pillRow_ = new wxPanel(right_);
    pillRow_->SetBackgroundColour(T.panel);
    pillSizer_ = new wxBoxSizer(wxHORIZONTAL);
    pillRow_->SetSizer(pillSizer_);
    rcol->Add(pillRow_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

    caseBody_ = new wxPanel(right_);
    caseBody_->SetBackgroundColour(T.panel);
    auto* cb = new wxBoxSizer(wxVERTICAL);
    caseBody_->SetSizer(cb);
    rcol->Add(caseBody_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
    right_->SetSizer(rcol);
    split->SplitHorizontally(edCard, right_, -330);
    split->SetMinimumPaneSize(120);
    split->SetSashGravity(1.0);

    outer->SplitVertically(stmtCard, split, 430);
    outer->SetMinimumPaneSize(260);
    col->Add(outer, 1, wxEXPAND | wxALL, 8);

    // The webview needs a realised window, so attach after the frame is up.
    CallAfter([this] { AttachStatementView(); });

    root->SetSizer(col);
    // A custom strip instead of wxStatusBar, which is another Win32 control.
    auto* statusRow = new wxPanel(root);
    statusRow->SetBackgroundColour(T.panel);
    auto* srow = new wxBoxSizer(wxHORIZONTAL);
    statusLeft_ = new wxStaticText(statusRow, wxID_ANY, "ready");
    statusLeft_->SetForegroundColour(T.muted);
    statusRight_ = new wxStaticText(statusRow, wxID_ANY, "");
    statusRight_->SetForegroundColour(T.muted);
    statusRight_->Hide();
    srow->Add(statusLeft_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);
    srow->AddStretchSpacer();
    srow->Add(statusRight_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);
    statusRow->SetSizer(srow);
    statusRow->SetMinSize(wxSize(-1, 26));
    col->Add(statusRow, 0, wxEXPAND);

    // Ctrl+Enter runs and Ctrl+B folds the test panel, like the web build.
    wxAcceleratorEntry acc[2];
    acc[0].Set(wxACCEL_CTRL, WXK_RETURN, wxID_HIGHEST + 1);
    acc[1].Set(wxACCEL_CTRL, 'B', wxID_HIGHEST + 2);
    SetAcceleratorTable(wxAcceleratorTable(2, acc));
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { RunAll(); }, wxID_HIGHEST + 1);
    Bind(wxEVT_MENU, [this](wxCommandEvent&) { ToggleTests(); }, wxID_HIGHEST + 2);
  }

  wxStyledTextCtrl* MakeEditor(wxWindow* parent) {
    auto* ed = new wxStyledTextCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    ed->SetUseHorizontalScrollBar(false);
    ed->StyleSetForeground(wxSTC_STYLE_DEFAULT, T.text);
    ed->StyleSetBackground(wxSTC_STYLE_DEFAULT, T.bg);
    ed->StyleSetFaceName(wxSTC_STYLE_DEFAULT, "Consolas");
    ed->StyleSetSize(wxSTC_STYLE_DEFAULT, 11);
    ed->StyleClearAll();
    ed->SetMarginType(0, wxSTC_MARGIN_NUMBER);
    ed->SetMarginWidth(0, 46);
    ed->StyleSetForeground(wxSTC_STYLE_LINENUMBER, T.muted);
    ed->StyleSetBackground(wxSTC_STYLE_LINENUMBER, T.bg);
    ed->SetCaretForeground(T.text);
    ed->SetSelBackground(true, wxColour(44, 187, 93, 77));
    ed->SetUseTabs(false);
    ed->SetTabWidth(4);
    ed->SetIndent(4);
    ed->Bind(wxEVT_STC_CHANGE, [this](wxStyledTextEvent&) { dirty_ = true; });
    return ed;
  }

  void ApplyLexer(const std::string& lang) {
    if (!editor_) return;
    editor_->SetLexer(lang == "python" ? wxSTC_LEX_PYTHON : lang == "java" ? wxSTC_LEX_CPP
                      : lang == "js"   ? wxSTC_LEX_CPP    : wxSTC_LEX_CPP);
    if (lang == "python") {
      editor_->SetKeyWords(0, "def class return if elif else for while in import from as and or not break "
                              "continue lambda try except finally with yield global pass raise del is assert None True False");
      editor_->StyleSetForeground(wxSTC_P_WORD, T.kw);
      editor_->StyleSetForeground(wxSTC_P_STRING, T.str);
      editor_->StyleSetForeground(wxSTC_P_CHARACTER, T.str);
      editor_->StyleSetForeground(wxSTC_P_TRIPLE, T.str);
      editor_->StyleSetForeground(wxSTC_P_TRIPLEDOUBLE, T.str);
      editor_->StyleSetForeground(wxSTC_P_COMMENTLINE, T.comment);
      editor_->StyleSetForeground(wxSTC_P_NUMBER, T.num);
      editor_->StyleSetForeground(wxSTC_P_DEFNAME, T.fn);
      editor_->StyleSetForeground(wxSTC_P_CLASSNAME, T.type);
    } else {
      editor_->SetKeyWords(0, kCppKeywords);
      editor_->SetKeyWords(1, kCppTypes);
      editor_->StyleSetForeground(wxSTC_C_WORD, T.kw);
      editor_->StyleSetForeground(wxSTC_C_WORD2, T.type);
      editor_->StyleSetForeground(wxSTC_C_STRING, T.str);
      editor_->StyleSetForeground(wxSTC_C_CHARACTER, T.str);
      editor_->StyleSetForeground(wxSTC_C_NUMBER, T.num);
      editor_->StyleSetForeground(wxSTC_C_COMMENT, T.comment);
      editor_->StyleSetForeground(wxSTC_C_COMMENTLINE, T.comment);
      editor_->StyleSetForeground(wxSTC_C_COMMENTDOC, T.comment);
      editor_->StyleSetForeground(wxSTC_C_PREPROCESSOR, T.kw);
    }
  }

  // ---------------------------------------------------------- statement pane
  void AttachStatementView() {
    try {
      wv_ = webview_create(0, stmtHost_->GetHandle());
    } catch (const std::exception&) {
      wv_ = nullptr;
    }
    if (!wv_) { if (statusLeft_) statusLeft_->SetLabel("statement view unavailable (WebView2 missing?)"); return; }
    SizeStatementView();
    if (probIdx_ >= 0) ShowStatement();
  }

  // webview creates its host window 0x0 and only resizes it itself when it owns the
  // top-level window. Embedded in someone else's window it never gets a WM_SIZE, so
  // the child has to be found and sized here or the pane stays blank.
  void SizeStatementView() {
    if (!wv_ || !stmtHost_) return;
    wxSize s = stmtHost_->GetClientSize();
    if (s.GetWidth() <= 0 || s.GetHeight() <= 0) return;
#ifdef _WIN32
    HWND child = FindWindowExW((HWND)stmtHost_->GetHandle(), nullptr, L"webview_widget", nullptr);
    if (child) SetWindowPos(child, nullptr, 0, 0, s.GetWidth(), s.GetHeight(), SWP_NOZORDER | SWP_NOACTIVATE);
#else
    webview_set_size(wv_, s.GetWidth(), s.GetHeight(), WEBVIEW_HINT_NONE);
#endif
  }

  void OnStmtSize(wxSizeEvent& e) {
    SizeStatementView();
    e.Skip();
  }

  // kStmtCss declares its colours as :root custom properties; a second :root
  // block after it repaints the statement pane when the theme flips.
  static std::string ThemeCss() {
    auto hex = [](const wxColour& c) {
      char b[8];
      snprintf(b, sizeof b, "#%02x%02x%02x", c.Red(), c.Green(), c.Blue());
      return std::string(b);
    };
    return ":root{--bg:" + hex(T.bg) + ";--panel:" + hex(T.panel) + ";--border:" + hex(T.border) +
           ";--text:" + hex(T.text) + ";--muted:" + hex(T.muted) + ";--code:" + hex(T.codebg) + "}";
  }

  void ShowStatement() {
    if (!wv_ || probIdx_ < 0) return;
    const Problem& p = contest_.problems[probIdx_];
    std::string body;
    if (stmtTab_ == 1) {
      body = "<h3>Submissions</h3>";
      auto hist = storage_.loadHistory();
      bool any = false;
      for (auto it = hist.rbegin(); it != hist.rend(); ++it) {
        if (it->prob != p.id) continue;
        any = true;
        body += "<p><b style=\"color:" + std::string(it->ok ? "#2cbb5d" : "#ef4743") + "\">" +
                util::htmlEscape(it->verdict) + "</b> &nbsp; " + util::htmlEscape(it->lang) + " &nbsp; " +
                util::clockHHMM(it->at) + "</p>";
      }
      if (!any) body += "<p class=\"muted\">No submissions for this problem yet.</p>";
    } else {
      body = p.statementHtml.empty() ? "<p class=\"muted\">No statement stored for this problem.</p>"
                                     : p.statementHtml;
    }
    std::string doc =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<link rel=\"stylesheet\" href=\"katex/katex.min.css\">"
        "<script src=\"katex/katex.min.js\"></script>"
        "<script src=\"katex/auto-render.min.js\"></script>"
        "<style>" + std::string(kStmtCss) + ThemeCss() + "</style>"
        "<script>" + std::string(kKatexBoot) + "</script></head><body><div class=\"wrap problem-statement\">" +
        body + "</div></body></html>";
    // Written next to the vendored KaTeX so the relative asset paths resolve.
    fs::path out = assetDir() / "_statement.html";
    if (!util::writeFile(out, doc)) return;
    std::string url = "file:///" + util::replaceAll(util::pstr(out), "\\", "/");
    webview_navigate(wv_, url.c_str());
  }

  // -------------------------------------------------------------- contest data
  void LoadContests() {
    contests_ = storage_.listContests();
    {
      std::vector<wxString> items;
      for (auto& c : contests_) items.push_back(U(c.name) + wxString::Format("   (%d/%d)", c.solved, c.total));
      contestChoice_->SetItems(items, items.empty() ? -1 : 0);
    }
    if (contests_.empty()) { if (statusLeft_) statusLeft_->SetLabel("no contests in " + U(util::pstr(storage_.root()))); return; }
    OpenContest(0);
  }

  void OpenContest(int i) {
    if (i < 0 || i >= (int)contests_.size()) return;
    SaveCode();
    if (!storage_.loadContest(contests_[i].dir, contest_)) { if (statusLeft_) statusLeft_->SetLabel("could not open that contest"); return; }
    contestIdx_ = i;
    {
      std::vector<wxString> items;
      for (auto& p : contest_.problems) items.push_back(U(p.id + ". " + p.title));
      (void)items;
      BuildProblemTabs();
    }
    if (contest_.problems.empty()) { if (statusLeft_) statusLeft_->SetLabel(U(contest_.name) + wxString(L" \u2014 no problems")); return; }
    OpenProblem(0);
  }

  void OpenProblem(int i) {
    if (i < 0 || i >= (int)contest_.problems.size()) return;
    SaveCode();
    probIdx_ = i;
    Problem& p = contest_.problems[i];
    int li = 0;
    auto& langs = Storage::languages();
    for (size_t k = 0; k < langs.size(); ++k)
      if (langs[k] == p.lang) li = (int)k;
    langChoice_->SetItems(LangLabels(), li);
    ApplyLexer(p.lang);
    editor_->SetText(U(storage_.loadCode(contest_, p, p.lang)));
    editor_->EmptyUndoBuffer();
    dirty_ = false;
    BuildCases();
    RefreshProblemTabs();
    UpdateChips();
    if (timerPill_) timerPill_->SetText(U(util::fmtClock(p.timeSeconds)));
    ClearVerdict();
    ShowStatement();
    if (statusLeft_) statusLeft_->SetLabel(U(contest_.name) + wxString(L" \u2014 ") + U(p.id + ". " + p.title));
    const wxString dot = wxString(L"  \u00B7  ");
    if (statusRight_) statusRight_->SetLabel(U(p.judge) + dot + wxString::Format("%.0f s", p.timeLimitSec) + dot + wxString::Format("%d MB", p.memoryMB));
  }

  void SwitchLanguage() {
    if (probIdx_ < 0) return;
    SaveCode();
    Problem& p = contest_.problems[probIdx_];
    p.lang = Storage::languages()[langChoice_->GetSelection()];
    storage_.saveProblemState(contest_, p);
    ApplyLexer(p.lang);
    editor_->SetText(U(storage_.loadCode(contest_, p, p.lang)));
    editor_->EmptyUndoBuffer();
    dirty_ = false;
  }

  void SaveCode() {
    if (probIdx_ < 0 || !dirty_) return;
    Problem& p = contest_.problems[probIdx_];
    storage_.saveCode(contest_, p, p.lang, std::string(editor_->GetText().utf8_string()));
    dirty_ = false;
  }

  // ------------------------------------------------------------------- cases
  // The web UI's LANGS labels, so the picker reads "Python 3", not "python".
  static std::vector<wxString> LangLabels() {
    return {"Python 3", "C++ (g++ 15)", "Java", "JavaScript (Node)"};
  }

  // ------------------------------------------------------------- top bar bits
  void NotYet(const wxString& what) {
    wxMessageBox(what + " is not in this build yet.\nIt still works in cp-ide.exe.", "CP IDE",
                 wxOK | wxICON_INFORMATION, this);
  }

  void CopyCode() {
    if (wxTheClipboard->Open()) {
      wxTheClipboard->SetData(new wxTextDataObject(editor_->GetText()));
      wxTheClipboard->Close();
      if (statusLeft_) statusLeft_->SetLabel("solution copied");
    }
  }

  void ResetCode() {
    if (probIdx_ < 0) return;
    Problem& p = contest_.problems[probIdx_];
    auto tpl = storage_.loadTemplates();
    std::string code = tpl.contains(p.lang) && tpl[p.lang].is_string() ? tpl[p.lang].get<std::string>() : "";
    editor_->SetText(U(code));
    dirty_ = true;
    SaveCode();
  }

  void ImportUrl() {
    wxTextEntryDialog dlg(this, "Paste a problem or contest URL", "Import");
    if (dlg.ShowModal() != wxID_OK) return;
    NotYet("Importing (" + dlg.GetValue() + ")");
  }

  // The web UI's "refresh ↻" chip: fetch the statement from the judge again and
  // keep the custom cases, replacing the samples.
  void RefetchStatement() {
    if (probIdx_ < 0 || running_) return;
    Problem& p = contest_.problems[probIdx_];
    if (p.url.empty()) return;
    if (statusLeft_) statusLeft_->SetLabel("fetching statement...");
    wxBusyCursor busy;
    HttpClient http;
    StatementInfo si = fetchStatement(http, p.url, p.judge);
    if (!si.ok) {
      if (statusLeft_) statusLeft_->SetLabel(U("fetch failed: " + si.error));
      return;
    }
    p.statementHtml = si.html;
    p.statementExact = si.exact;
    p.statementSamples = si.selfSamples;
    if (si.rating) p.rating = si.rating;
    if (si.timeLimitSec > 0) p.timeLimitSec = si.timeLimitSec;
    if (si.memoryMB > 0) p.memoryMB = si.memoryMB;
    if (!si.samples.empty()) {
      std::vector<TestCase> keep;
      for (const auto& t : p.tests)
        if (t.custom) keep.push_back(t);
      p.tests = si.samples;
      for (auto& t : keep) p.tests.push_back(t);
    }
    storage_.saveProblemMeta(contest_, p);
    storage_.saveTests(contest_, p);
    lastStatus_.clear();
    lastGot_.clear();
    UpdateChips();
    BuildCases();
    ShowStatement();
    if (statusLeft_) statusLeft_->SetLabel("statement refreshed");
  }

  // ---------------------------------------------------------- layout / theme
  // The web UI's layout cards, as pane proportions: how much room the statement
  // gets, and how the editor and the test panel divide what is left.
  void SetLayout(int i) {
    layout_ = i;
    SaveUiPrefs();
    ApplyLayout();
  }

  void ApplyLayout() {
    if (!outer_ || !split_) return;
    wxSize s = outer_->GetClientSize();
    int w = std::max(600, s.GetWidth());
    int h = std::max(400, split_->GetClientSize().GetHeight());
    // statement width fraction, then the test panel's height fraction
    static const double kW[] = {0.36, 0.46, 0.30, 0.55};
    static const double kH[] = {0.49, 0.42, 0.62, 0.42};
    int idx = (layout_ >= 0 && layout_ < 4) ? layout_ : 0;
    outer_->SetSashPosition((int)(w * kW[idx]));
    split_->SetSashPosition(h - (int)(h * kH[idx]));
  }

  // Both builds read the same cp/state.json keys, so the theme and layout you
  // pick in one are the theme and layout you get in the other.
  void LoadUiPrefs() {
    json st = storage_.loadState();
    light_ = st.value("theme", std::string("dark")) == "light";
    std::string lay = st.value("layout", std::string("default"));
    layout_ = lay == "note" ? 1 : lay == "debug" ? 2 : lay == "wide" ? 3 : 0;
    T = light_ ? LightTheme() : DarkTheme();
  }

  void SaveUiPrefs() {
    json st = storage_.loadState();
    st["theme"] = light_ ? "light" : "dark";
    static const char* kNames[] = {"default", "note", "debug", "wide"};
    st["layout"] = kNames[(layout_ >= 0 && layout_ < 4) ? layout_ : 0];
    storage_.saveState(st);
  }

  void ToggleTheme() {
    SaveCode();
    light_ = !light_;
    T = light_ ? LightTheme() : DarkTheme();
    SaveUiPrefs();
#ifdef _WIN32
    BOOL dark = light_ ? FALSE : TRUE;
    DwmSetWindowAttribute((HWND)GetHandle(), 20, &dark, sizeof(dark));
#endif
    RebuildUi();
  }

  // Every widget takes its colours from T when it is built, so a theme change is
  // a rebuild. State lives in contest_/probIdx_, not in the widgets.
  void RebuildUi() {
    int ci = contestIdx_, pi = probIdx_, sc = sel_, st = stmtTab_, pt = panelTab_;
    if (wv_) { webview_destroy(wv_); wv_ = nullptr; }
    Freeze();
    DestroyChildren();
    ForgetWidgets();
    BuildUi();
    Layout();
    Thaw();
    contestIdx_ = ci;
    if (ci >= 0 && ci < (int)contests_.size()) {
      std::vector<wxString> items;
      for (auto& c : contests_) items.push_back(U(c.name) + wxString::Format("   (%d/%d)", c.solved, c.total));
      contestChoice_->SetItems(items, ci);
      BuildProblemTabs();
    }
    SetStmtTab(st);
    SetPanelTab(pt);
    if (pi >= 0 && pi < (int)contest_.problems.size()) { probIdx_ = -1; OpenProblem(pi); }
    sel_ = sc;
    BuildCases();
    // The rows filled in above (contest name, problem tabs, chips) were sized
    // while they were still empty, so every nested sizer needs a second pass.
    RelayoutAll(this);
  }

  static void RelayoutAll(wxWindow* w) {
    w->Layout();
    for (wxWindow* c : w->GetChildren()) RelayoutAll(c);
  }

  // Clears every pointer into the window tree RebuildUi just destroyed.
  void ForgetWidgets() {
    contestChoice_ = langChoice_ = layoutPill_ = nullptr;
    tabRow_ = stmtHost_ = chipRow_ = pillRow_ = caseBody_ = nullptr;
    tabSizer_ = chipSizer_ = pillSizer_ = nullptr;
    tabCase_ = tabResult_ = tabDesc_ = tabSubs_ = nullptr;
    runBtn_ = submitBtn_ = timerPill_ = themePill_ = collapsePill_ = nullptr;
    summary_ = caseCount_ = verdict_ = why_ = statusLeft_ = statusRight_ = nullptr;
    caseIn_ = caseOut_ = caseGot_ = nullptr;
    editor_ = nullptr;
    right_ = nullptr;
    outer_ = split_ = nullptr;
    probPills_.clear();
    pills_.clear();
  }

  // Ctrl+B in the web build: fold the test panel down to its header.
  void ToggleTests() {
    testsCollapsed_ = !testsCollapsed_;
    if (collapsePill_) collapsePill_->SetText(wxString(testsCollapsed_ ? L"⌃" : L"⌄"));
    if (!split_) return;
    if (testsCollapsed_) {
      savedSash_ = split_->GetSashPosition();
      split_->SetSashPosition(split_->GetClientSize().GetHeight() - 40);
    } else {
      split_->SetSashPosition(savedSash_ > 0 ? savedSash_ : split_->GetClientSize().GetHeight() - 330);
    }
  }

  void ShowHistory() {
    auto hist = storage_.loadHistory();
    wxString s;
    int n = 0;
    for (auto it = hist.rbegin(); it != hist.rend() && n < 30; ++it, ++n)
      s += U(it->prob) + "   " + U(it->verdict) + "   " + U(it->lang) + "   " + U(util::clockHHMM(it->at)) + "\n";
    if (s.empty()) s = "No submissions yet.";
    wxMessageBox(s, "History", wxOK, this);
  }

  void ShowShortcuts() {
    wxMessageBox("Ctrl+Enter\tRun all tests\nAlt+Left / Alt+Right\tPrevious / next problem\n",
                 "Keyboard shortcuts", wxOK, this);
  }

  // --------------------------------------------------------- timer / submit
  void OnTick(wxTimerEvent&) {
    if (probIdx_ < 0 || timerPaused_) return;
    Problem& p = contest_.problems[probIdx_];
    if (p.solved) return;
    p.timeSeconds++;
    if (timerPill_) timerPill_->SetText(U(util::fmtClock(p.timeSeconds)));
    if (p.timeSeconds % 10 == 0) storage_.saveProblemState(contest_, p);
  }

  void ToggleTimer() {
    timerPaused_ = !timerPaused_;
    if (timerPill_) timerPill_->SetAccent(timerPaused_ ? T.muted : T.text, true);
  }

  // Submitting needs the judge's browser window, which only the web build has.
  void OnSubmit() {
    if (probIdx_ < 0) return;
    SaveCode();
    const Problem& p = contest_.problems[probIdx_];
    if (p.url.empty()) return;
    wxMessageBox("Submitting from this build is not wired up yet.\n\nOpening the problem page instead:\n" + U(p.url),
                 "Submit", wxOK | wxICON_INFORMATION, this);
    wxLaunchDefaultBrowser(U(p.url));
  }

  // ------------------------------------------------------- statement header
  void SetStmtTab(int t) {
    stmtTab_ = t;
    if (tabDesc_) tabDesc_->SetActive(t == 0);
    if (tabSubs_) tabSubs_->SetActive(t == 1);
    if (tabDesc_) tabDesc_->SetAccent(t == 0 ? T.text : T.muted, true);
    if (tabSubs_) tabSubs_->SetAccent(t == 1 ? T.text : T.muted, true);
    ShowStatement();
  }

  void UpdateChips() {
    if (!chipRow_ || probIdx_ < 0) return;
    const Problem& p = contest_.problems[probIdx_];
    chipSizer_->Clear(true);
    auto add = [&](const wxString& s, bool link = false) {
      chipSizer_->Add(new Chip(chipRow_, s, link), 0, wxRIGHT, 8);
    };
    add(p.rating ? wxString::Format("*%d", p.rating) : wxString(L"*—"));
    add(wxString::Format("%.10g s", p.timeLimitSec) + wxString(L" · ") + wxString::Format("%d MB", p.memoryMB));
    if (!p.url.empty()) {
      auto* c = new Chip(chipRow_, U(p.judge) + wxString(L" ↗"), true);
      c->Bind(wxEVT_LEFT_UP, [url = p.url](wxMouseEvent&) { wxLaunchDefaultBrowser(U(url)); });
      chipSizer_->Add(c, 0, wxRIGHT, 8);
    }
    auto* folder = new Chip(chipRow_, wxString(L"folder ↗"), true);
    folder->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) {
      if (probIdx_ >= 0) wxLaunchDefaultBrowser(U(util::pstr(storage_.problemDir(contest_, contest_.problems[probIdx_]))));
    });
    chipSizer_->Add(folder, 0, wxRIGHT, 8);
    if (!p.url.empty() && !p.statementHtml.empty()) {
      auto* re = new Chip(chipRow_, wxString(L"refresh ↻"), true);
      re->SetToolTip("Fetch the statement from the judge again");
      re->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { RefetchStatement(); });
      chipSizer_->Add(re, 0, wxRIGHT, 8);
    }
    chipRow_->Layout();
    if (chipRow_->GetParent()) chipRow_->GetParent()->Layout();
  }

  // One pill per problem, mono and outlined like the web UI's .tab, with the
  // solved/attempted dot.
  void BuildProblemTabs() {
    tabSizer_->Clear(true);
    probPills_.clear();
    for (size_t i = 0; i < contest_.problems.size(); ++i) {
      const Problem& p = contest_.problems[i];
      auto* pill = new Pill(tabRow_, U(p.id), [this, i] { OpenProblem((int)i); });
      pill->SetStyle(Pill::Style::Tab);
      pill->SetMono(true);
      if (p.solved || p.attempted) pill->SetDot(p.solved ? T.ok : T.bad, true);
      tabSizer_->Add(pill, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, 6);
      probPills_.push_back(pill);
    }
    tabRow_->Layout();
    // The row just changed width; the bar's sizer has to re-place it, or a long
    // contest name gets clipped to its middle.
    if (tabRow_->GetParent()) tabRow_->GetParent()->Layout();
    RefreshProblemTabs();
  }

  void RefreshProblemTabs() {
    for (size_t i = 0; i < probPills_.size(); ++i) probPills_[i]->SetActive((int)i == probIdx_);
  }

  void SetPanelTab(int t) {
    panelTab_ = t;
    tabCase_->SetActive(t == 0);
    tabResult_->SetActive(t == 1);
    tabCase_->SetAccent(t == 0 ? T.text : T.muted, true);
    tabResult_->SetAccent(t == 1 ? T.text : T.muted, true);
    BuildCases();   // the pill row differs between the two tabs, not just the body
  }

  // "Case 1" for a judge sample, "Custom 3" for one you added — the web UI's naming.
  static wxString CaseLabel(const TestCase& t, int i) {
    return wxString::Format(t.custom ? "Custom %d" : "Case %d", i + 1);
  }

  void CopyCaseInput() {
    if (!caseIn_ || !wxTheClipboard->Open()) return;
    wxTheClipboard->SetData(new wxTextDataObject(caseIn_->GetValue()));
    wxTheClipboard->Close();
    if (statusLeft_) statusLeft_->SetLabel("input copied");
  }

  // One pill per case plus a "+", and a single detail view for the selected one.
  void BuildCases() {
    pillSizer_->Clear(true);
    pills_.clear();
    if (probIdx_ >= 0) {
      Problem& p = contest_.problems[probIdx_];
      for (size_t i = 0; i < p.tests.size(); ++i) {
        auto* pill = new Pill(pillRow_, CaseLabel(p.tests[i], (int)i), [this, i] { SelectCase((int)i); });
        pill->SetStyle(Pill::Style::Tab);
        pillSizer_->Add(pill, 0, wxRIGHT | wxBOTTOM, 5);
        pills_.push_back(pill);
        if (p.tests[i].custom) {
          // The web UI's ✕ inside a custom case's pill.
          auto* x = new Pill(pillRow_, wxString(L"✕"), [this, i] { SelectCase((int)i); DeleteCase(); });
          x->SetStyle(Pill::Style::Link);
          x->SetToolTip("Delete this case");
          pillSizer_->Add(x, 0, wxRIGHT | wxBOTTOM | wxALIGN_CENTER_VERTICAL, 5);
        }
      }
      if (panelTab_ == 0) {   // the web UI's caseTabsHtml(false): no "+" on results
        auto* add = new Pill(pillRow_, "+", [this] { AddCase(); });
        add->SetStyle(Pill::Style::Tab);
        pillSizer_->Add(add, 0, wxBOTTOM, 5);
      }
    }
    if (caseCount_) {
      size_t n = probIdx_ >= 0 ? contest_.problems[probIdx_].tests.size() : 0;
      caseCount_->SetLabel(wxString::Format("%zu case%s", n, n == 1 ? "" : "s"));
    }
    pillRow_->Layout();
    if (sel_ >= (int)pills_.size()) sel_ = (int)pills_.size() - 1;
    if (sel_ < 0 && !pills_.empty()) sel_ = 0;
    BuildCaseBody();
  }

  void BuildCaseBody() {
    auto* s = caseBody_->GetSizer();
    s->Clear(true);
    caseIn_ = caseOut_ = caseGot_ = nullptr;
    if (probIdx_ < 0 || sel_ < 0) { caseBody_->Layout(); return; }
    Problem& p = contest_.problems[probIdx_];
    if (sel_ >= (int)p.tests.size()) { caseBody_->Layout(); return; }

    auto label = [&](const wxString& t) {
      auto* l = new wxStaticText(caseBody_, wxID_ANY, t);
      l->SetForegroundColour(T.muted);
      return l;
    };
    auto* head = new wxBoxSizer(wxHORIZONTAL);
    head->Add(label("Input"), 0, wxALIGN_CENTER_VERTICAL);
    head->AddStretchSpacer();
    // The web UI puts a copy icon here, not a delete link — deleting a custom
    // case is the ✕ on its own pill.
    auto* copyIn = new Pill(caseBody_, wxString(L"⧉"), [this] { CopyCaseInput(); });
    copyIn->SetStyle(Pill::Style::Button);
    copyIn->SetToolTip("Copy input");
    head->Add(copyIn, 0);
    s->Add(head, 0, wxEXPAND | wxBOTTOM, 4);

    auto* inBox = new Box(caseBody_, U(p.tests[sel_].in), panelTab_ == 1, 44);
    caseIn_ = inBox->Text();
    s->Add(inBox, 1, wxEXPAND | wxBOTTOM, 10);

    // Testcase tab edits Input + Expected; Test Result adds what the program
    // actually printed, in the web UI's order: Input, Your output, Expected.
    if (panelTab_ == 1) {
      s->Add(label("Your output"), 0, wxBOTTOM, 4);
      auto* gotBox = new Box(caseBody_, U(lastGot_.count(sel_) ? lastGot_[sel_] : std::string()), true, 44);
      caseGot_ = gotBox->Text();
      s->Add(gotBox, 1, wxEXPAND | wxBOTTOM, 10);
    }
    s->Add(label("Expected"), 0, wxBOTTOM, 4);
    auto* outBox = new Box(caseBody_, U(p.tests[sel_].out), panelTab_ == 1, 44);
    caseOut_ = outBox->Text();
    s->Add(outBox, 1, wxEXPAND);
    caseBody_->Layout();
    RefreshPills();
  }

  void SelectCase(int i) {
    if (i < 0 || i >= (int)pills_.size()) return;
    CollectCases();
    sel_ = i;
    BuildCaseBody();
  }

  void RefreshPills() {
    for (size_t i = 0; i < pills_.size(); ++i) {
      pills_[i]->SetActive((int)i == sel_);
      wxString base = probIdx_ >= 0 && i < contest_.problems[probIdx_].tests.size()
                          ? CaseLabel(contest_.problems[probIdx_].tests[i], (int)i)
                          : wxString::Format("Case %d", (int)i + 1);
      auto it = lastStatus_.find((int)i);
      if (it == lastStatus_.end()) {
        pills_[i]->SetText(base);
        pills_[i]->SetAccent(T.muted, false);
      } else {
        pills_[i]->SetText(base + "  " + MarkFor(it->second));
        bool good = it->second == "pass";
        bool neutral = it->second == "out";
        pills_[i]->SetAccent(good ? T.ok : neutral ? T.muted : T.bad, true);
      }
    }
    pillRow_->Layout();
  }

  void AddCase() {
    if (probIdx_ < 0) return;
    CollectCases();
    Problem& p = contest_.problems[probIdx_];
    TestCase t;
    t.custom = true;
    p.tests.push_back(t);
    storage_.saveTests(contest_, p);
    BuildCases();
    sel_ = (int)contest_.problems[probIdx_].tests.size() - 1;
    BuildCases();
  }

  void DeleteCase() {
    if (probIdx_ < 0) return;
    int i = sel_;
    Problem& p = contest_.problems[probIdx_];
    if (i < 0 || i >= (int)p.tests.size()) return;
    if (wxMessageBox(wxString::Format("Delete case %d?", i + 1), "CP IDE", wxYES_NO | wxICON_QUESTION, this) != wxYES)
      return;
    CollectCases();
    p.tests.erase(p.tests.begin() + i);
    storage_.saveTests(contest_, p);
    BuildCases();
  }

  // Only the visible case has live controls, so only it can have edits to collect.
  void CollectCases() {
    if (probIdx_ < 0 || sel_ < 0 || !caseIn_ || !caseOut_) return;
    Problem& p = contest_.problems[probIdx_];
    if (sel_ >= (int)p.tests.size()) return;
    p.tests[sel_].in = caseIn_->GetValue().utf8_string();
    p.tests[sel_].out = caseOut_->GetValue().utf8_string();
  }

  // --------------------------------------------------------------------- run
  void RunAll() {
    if (running_ || probIdx_ < 0) return;
    SaveCode();
    CollectCases();
    Problem& p = contest_.problems[probIdx_];
    if (p.tests.empty()) { if (statusLeft_) statusLeft_->SetLabel("no test cases"); return; }
    storage_.saveTests(contest_, p);

    running_ = true;
    cancel_ = false;
    runBtn_->SetText("Running...");
    if (caseGot_) caseGot_->ChangeValue("");
    ClearVerdict();
    SetPanelTab(1);
    if (summary_) { summary_->SetLabel("running..."); summary_->SetForegroundColour(T.muted); }
    if (statusLeft_) statusLeft_->SetLabel("running...");

    if (worker_.joinable()) worker_.join();
    auto dir = storage_.problemDir(contest_, p);
    auto tests = p.tests;
    auto lang = p.lang;
    double tl = p.timeLimitSec;
    Toolchain tc = tc_;

    worker_ = std::thread([this, dir, tests, lang, tl, tc] {
      std::string compileLog;
      if (Toolchain::needsCompile(lang)) {
        auto cr = compileFor(tc, lang, dir, &cancel_);
        if (!cr.ok) {
          CallAfter([this, log = cr.log] { Finish(-1, 0, "compile error", log); });
          return;
        }
      }
      int passed = 0, graded = 0, maxMs = 0;
      for (size_t i = 0; i < tests.size(); ++i) {
        if (cancel_) break;
        auto v = runTest(tc, lang, dir, tests[i], tl, &cancel_, /*scratchIfNoExpected=*/true);
        maxMs = std::max(maxMs, v.ms);
        if (v.status != "out") {
          graded++;
          if (v.status == "pass") passed++;
        }
        CallAfter([this, i, v] { ShowCase((int)i, v); });
      }
      CallAfter([this, passed, graded, maxMs] { Finish(passed, graded, "", "", maxMs); });
    });
  }

  void ShowCase(int i, const TestVerdict& v) {
    lastStatus_[i] = v.status;
    lastGot_[i] = v.got;
    RefreshPills();
    if (i == sel_ && caseGot_) {
      caseGot_->ChangeValue(U(v.got));
      caseGot_->SetForegroundColour(v.status == "pass" ? T.ok : v.status == "out" ? T.muted : T.bad);
      caseGot_->Refresh();
    }

    // The first failure is what you want on screen, with its reason.
    bool bad = v.status == "fail" || v.status == "tle" || v.status == "re";
    if (bad && firstBad_ < 0) {
      firstBad_ = i;
      SelectCase(i);
      double tl = probIdx_ >= 0 ? contest_.problems[probIdx_].timeLimitSec : 1.0;
      ShowVerdict(v.status, explainVerdict(v, tl));
    } else if (!bad && firstBad_ < 0) {
      ShowVerdict(v.status, "");
    }
  }

  void ClearVerdict() {
    firstBad_ = -1;
    if (verdict_) { verdict_->SetLabel(""); verdict_->Hide(); }
    if (why_) { why_->SetLabel(""); why_->Hide(); }
    lastStatus_.clear();
    lastGot_.clear();
    RefreshPills();
    Layout();
  }

  static wxString MarkFor(const std::string& s) {
    if (s == "pass") return wxString(L"\u2713");   // check mark
    if (s == "out") return wxString(L"\u00B7");    // middle dot
    if (s == "tle") return "TLE";
    if (s == "re") return "RE";
    return wxString(L"\u2715");                    // cross
  }

  void ShowVerdict(const std::string& status, const std::string& why) {
    wxString text = status == "pass"  ? "Accepted"
                    : status == "out" ? "Ran - nothing to compare"
                    : status == "tle" ? "Time limit exceeded"
                    : status == "re"  ? "Runtime error"
                    : status == "fail" ? "Wrong answer"
                                       : "";
    verdict_->SetLabel(text);
    verdict_->SetForegroundColour(status == "pass" ? T.ok : status == "out" ? T.muted : T.bad);
    why_->SetLabel(U(why));
    why_->Wrap(right_ ? right_->GetClientSize().GetWidth() - 24 : 300);
    // Empty labels still occupy a line each; hide them so the pills sit at the top
    // until there is actually a verdict to report.
    verdict_->Show(!text.IsEmpty());
    why_->Show(!why.empty());
    if (right_) right_->Layout();
    Layout();
  }

  void Finish(int passed, int graded, const std::string& err, const std::string& log, int ms = 0) {
    running_ = false;
    runBtn_->SetText("Run");
    if (!err.empty()) {
      if (statusLeft_) statusLeft_->SetLabel("compile error");
      wxMessageBox(U(log), "Compile error", wxOK | wxICON_ERROR, this);
      return;
    }
    const wxString dot = wxString(L"  \u00B7  ");
    if (summary_) {
      summary_->SetLabel(graded > 0 && passed == graded
                             ? wxString::Format("All %d passed", graded) + dot + wxString::Format("%d ms", ms)
                             : wxString::Format("%d/%d passed", passed, graded) + dot + wxString::Format("%d ms", ms));
      summary_->SetForegroundColour(graded > 0 && passed == graded ? T.ok : T.bad);
      if (right_) right_->Layout();
    }
    if (statusLeft_)
      statusLeft_->SetLabel(graded > 0 && passed == graded
                                ? wxString::Format("all %d passed", graded) + dot + wxString::Format("%d ms", ms)
                                : wxString::Format("%d/%d passed", passed, graded) + dot + wxString::Format("%d ms", ms));
  }

  Storage& storage_;
  Toolchain tc_;
  std::vector<ContestSummary> contests_;
  Contest contest_;
  int probIdx_ = -1;
  bool dirty_ = false;

  MenuPill* contestChoice_ = nullptr;
  wxPanel* tabRow_ = nullptr;
  wxBoxSizer* tabSizer_ = nullptr;
  std::vector<Pill*> probPills_;
  Pill* tabCase_ = nullptr;
  Pill* tabResult_ = nullptr;
  wxStaticText* summary_ = nullptr;
  wxStaticText* caseCount_ = nullptr;
  int panelTab_ = 0;
  MenuPill* langChoice_ = nullptr;
  Pill* runBtn_ = nullptr;
  wxPanel* stmtHost_ = nullptr;
  webview_t wv_ = nullptr;
  wxStyledTextCtrl* editor_ = nullptr;

  wxStaticText* verdict_ = nullptr;
  wxStaticText* why_ = nullptr;
  int firstBad_ = -1;
  Card* right_ = nullptr;
  Pill* submitBtn_ = nullptr;
  Pill* timerPill_ = nullptr;
  Pill* tabDesc_ = nullptr;
  Pill* tabSubs_ = nullptr;
  wxPanel* chipRow_ = nullptr;
  wxBoxSizer* chipSizer_ = nullptr;
  wxTimer clock_;
  int stmtTab_ = 0;
  bool timerPaused_ = false;
  wxPanel* pillRow_ = nullptr;
  wxPanel* caseBody_ = nullptr;
  wxBoxSizer* pillSizer_ = nullptr;
  std::vector<Pill*> pills_;
  wxTextCtrl* caseIn_ = nullptr;
  wxTextCtrl* caseOut_ = nullptr;
  wxTextCtrl* caseGot_ = nullptr;
  int sel_ = 0;
  std::map<int, std::string> lastStatus_, lastGot_;
  wxStaticText* statusLeft_ = nullptr;
  wxStaticText* statusRight_ = nullptr;

  MenuPill* layoutPill_ = nullptr;
  Pill* themePill_ = nullptr;
  Pill* collapsePill_ = nullptr;
  DarkSplitter* outer_ = nullptr;
  DarkSplitter* split_ = nullptr;
  int contestIdx_ = -1;
  int layout_ = 0;
  bool light_ = false;
  bool testsCollapsed_ = false;
  int savedSash_ = 0;

  std::thread worker_;
  std::atomic<bool> running_{false}, cancel_{false};
};

// ----------------------------------------------------------------------- app
class CpIdeWxApp : public wxApp {
 public:
  bool OnInit() override {
    storage_ = std::make_unique<Storage>(dataRoot());
    auto* f = new MainFrame(*storage_);
    f->Show();
    return true;
  }

 private:
  std::unique_ptr<Storage> storage_;
};

wxIMPLEMENT_APP(CpIdeWxApp);
