/* CP IDE front-end. Plain JS port of the design prototype; state lives here,
   the C++ core (window.cp_rpc) persists and executes. */
'use strict';

// ----------------------------------------------------------------- helpers
const rpc = (name, args) => (window.cp_rpc ? window.cp_rpc(name, args || {}) : Promise.resolve({}));
const esc = (s) => String(s == null ? '' : s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
const fmt = (t) => `${String(Math.floor(Math.max(0, t) / 60)).padStart(2, '0')}:${String(Math.max(0, t) % 60).padStart(2, '0')}`;
const $ = (sel, root) => (root || document).querySelector(sel);
const debounce = (fn, ms) => { let t; return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms); }; };

const EMPTY_PROB = {
  id: '—', title: 'No problems yet', rating: 0, tl: '—', ml: '—', tests: [], code: {}, bps: [], notes: '',
  timeSeconds: 0, timerMode: 'up', paused: false, solved: false, attempted: false, lang: 'python', statementHtml: '',
  empty: true, url: '', judge: 'other'
};

// ------------------------------------------------------------------- state
const S = {
  booted: false,
  theme: 'dark', layout: 'default', focus: false, fontSize: 14,
  session: null, contests: [], history: [], active: null,
  root: '', config: {}, templates: { python: '', cpp: '' },
  running: false, compiling: false, compileError: '', judging: false, judgeMsg: '',
  historyOpen: false, contestMenuOpen: false, layoutMenuOpen: false, timerMenuOpen: false, importOpen: false,
  timerEdit: '', importUrl: '',
  testsCollapsed: true, probTab: 'desc', focusCase: 0, curLn: 1, curCol: 1,
  sessionOpen: false, sessionName: '', sessionMode: 'blank', ratingMin: '1200', ratingMax: '1500', sessionUrl: '', sessionBusy: false,
  stressOpen: false, stress: { state: 'idle', iter: 0, input: '', expected: '', got: '', message: '' },
  debugging: false, debugStarting: false, debugLine: null, dbgVars: [], dbgStack: [], consoleText: '',
  toast: null, busy: '',
  stmtErrors: {},
  setupOpen: false, tools: [], judgeStatus: {}, defaultLang: 'python', port: 10045,
  cppCandidates: [], cppFlags: '-O2 -std=c++23', editTool: '', toolPath: ''
};
const JUDGES = [['codeforces', 'Codeforces'], ['atcoder', 'AtCoder'], ['cses', 'CSES'], ['usaco', 'USACO']];

function problems() { return S.session ? S.session.problems : []; }
function prob() { return problems().find((p) => p.id === S.active) || EMPTY_PROB; }
function tests() { return prob().tests || []; }
function lang() { const p = prob(); return p.empty ? 'python' : p.lang || 'python'; }
function light() { return S.theme === 'light'; }
function currentCode() { const p = prob(); return p.empty ? S.templates[lang()] : (p.code[lang()] ?? S.templates[lang()]); }

// -------------------------------------------------------------------- toast
let toastT = null;
function toast(msg, color, ms) {
  clearTimeout(toastT);
  S.toast = { msg, color: color || 'var(--accent)' };
  renderToast();
  toastT = setTimeout(() => { S.toast = null; renderToast(); }, ms || 2600);
}
function renderToast() {
  let root = $('#toast-root');
  if (!root) { root = document.createElement('div'); root.id = 'toast-root'; document.body.appendChild(root); }
  root.innerHTML = (S.toast ? `<div class="toast" style="border-color:${S.toast.color}"><span class="d" style="background:${S.toast.color}"></span>${esc(S.toast.msg)}</div>` : '') +
    (S.busy ? `<div class="busy"><span class="spin"></span>${esc(S.busy)}</div>` : '');
}

// ------------------------------------------------------------------- monaco
let editor = null, monacoReady = false;
const models = {};
let bpDecorations = [], dbgDecorations = [];
let suppressChange = false;

const DARK = { com: '6A9955', str: 'CE9178', num: 'B5CEA8', kw: 'C586C0', cons: '569CD6', type: '4EC9B0', fn: 'DCDCAA', vr: '9CDCFE', op: 'D4D4D4', br: ['FFD700', 'DA70D6', '179FFF'], bg: '#1d1d1d', ln: '#9a9a9a8c', lnActive: '#ececec', text: '#ececec', sel: '#2cbb5d4d', cursor: '#ececec' };
const LIGHT = { com: '008000', str: 'A31515', num: '098658', kw: 'AF00DB', cons: '0000FF', type: '267F99', fn: '795E26', vr: '001080', op: '3b3b3b', br: ['0431FA', '319331', '7B3814'], bg: '#fafafa', ln: '#6b72808c', lnActive: '#1c1c1e', text: '#1c1c1e', sel: '#1f9e4d40', cursor: '#1c1c1e' };

function defineLanguages() {
  const kwPy = 'def class return if elif else for while in import from as and or not break continue lambda try except finally with yield global pass raise del is assert async await nonlocal'.split(' ');
  const ctrlPy = 'return if elif else for while import from break continue try except finally with yield pass raise del assert async await'.split(' ');
  const consPy = ['True', 'False', 'None'];
  const typesPy = 'int str float bool list set dict tuple bytes object complex frozenset bytearray List Dict Set Tuple Optional Union Any'.split(' ');
  monaco.languages.register({ id: 'cp-python' });
  monaco.languages.setLanguageConfiguration('cp-python', {
    comments: { lineComment: '#', blockComment: ["'''", "'''"] },
    brackets: [['{', '}'], ['[', ']'], ['(', ')']],
    autoClosingPairs: [{ open: '{', close: '}' }, { open: '[', close: ']' }, { open: '(', close: ')' }, { open: '"', close: '"', notIn: ['string'] }, { open: "'", close: "'", notIn: ['string', 'comment'] }],
    onEnterRules: [{ beforeText: /:\s*$/, action: { indentAction: monaco.languages.IndentAction.Indent } }],
    indentationRules: { increaseIndentPattern: /^\s*(def|class|if|elif|else|for|while|try|except|finally|with|async).*:\s*$/, decreaseIndentPattern: /^\s*(elif|else|except|finally)\b.*:\s*$/ }
  });
  monaco.languages.setMonarchTokensProvider('cp-python', {
    ctrl: ctrlPy, kw: kwPy, cons: consPy, types: typesPy,
    tokenizer: {
      root: [
        [/#.*$/, 'comment'],
        [/[a-zA-Z_]\w*(?=\s*\()/, { cases: { 'def': 'keyword', '@ctrl': 'keyword.control', '@kw': 'keyword', '@cons': 'constant', '@types': 'type', '@default': 'function' } }],
        [/\b(def)(\s+)([a-zA-Z_]\w*)/, ['keyword', '', 'function']],
        [/\b(class)(\s+)([a-zA-Z_]\w*)/, ['keyword', '', 'type']],
        [/\bself\b|\bcls\b/, 'variable'],
        [/[a-zA-Z_]\w*/, { cases: { '@ctrl': 'keyword.control', '@kw': 'keyword', '@cons': 'constant', '@types': 'type', '[A-Z]\\w*': 'type', '@default': 'variable' } }],
        [/\d+\.\d*([eE][-+]?\d+)?j?|\.\d+([eE][-+]?\d+)?|\d+[eE][-+]?\d+|0[xX][0-9a-fA-F_]+|0[oO][0-7_]+|0[bB][01_]+|\d[\d_]*j?/, 'number'],
        [/[{}()\[\]]/, '@brackets'],
        [/(->|[+\-*\/%=!<>&|^~:]+|[.,;@])/, 'operator'],
        [/[rRbBfFuU]{0,2}"""/, 'string', '@str3d'], [/[rRbBfFuU]{0,2}'''/, 'string', '@str3s'],
        [/[rRbBfFuU]{0,2}"/, 'string', '@strd'], [/[rRbBfFuU]{0,2}'/, 'string', '@strs'],
        [/\s+/, '']
      ],
      strd: [[/[^\\"]+/, 'string'], [/\\./, 'string'], [/"/, 'string', '@pop']],
      strs: [[/[^\\']+/, 'string'], [/\\./, 'string'], [/'/, 'string', '@pop']],
      str3d: [[/[^"\\]+/, 'string'], [/\\./, 'string'], [/"""/, 'string', '@pop'], [/"/, 'string']],
      str3s: [[/[^'\\]+/, 'string'], [/\\./, 'string'], [/'''/, 'string', '@pop'], [/'/, 'string']]
    }
  });

  const ctrlCpp = 'if else for while do switch case default break continue return goto try catch throw co_return co_await co_yield'.split(' ');
  const kwCpp = 'using namespace struct class template typename public private protected const constexpr consteval constinit static inline virtual override final explicit typedef sizeof new delete friend operator extern mutable volatile register enum union noexcept static_assert decltype alignas alignof concept requires export import module'.split(' ');
  const consCpp = ['true', 'false', 'nullptr', 'NULL', 'this'];
  const typesCpp = 'int long double float char bool void short unsigned signed auto std vector string pair map set queue deque stack bitset array tuple size_t int64_t int32_t uint64_t uint32_t ll ull pii pll ld priority_queue unordered_map unordered_set multiset multimap list string_view optional variant function wchar_t char16_t char32_t int8_t int16_t uint8_t uint16_t ptrdiff_t ostream istream stringstream'.split(' ');
  monaco.languages.register({ id: 'cp-cpp' });
  monaco.languages.setLanguageConfiguration('cp-cpp', {
    comments: { lineComment: '//', blockComment: ['/*', '*/'] },
    brackets: [['{', '}'], ['[', ']'], ['(', ')']],
    autoClosingPairs: [{ open: '{', close: '}' }, { open: '[', close: ']' }, { open: '(', close: ')' }, { open: '"', close: '"', notIn: ['string'] }, { open: "'", close: "'", notIn: ['string', 'comment'] }],
    onEnterRules: [{ beforeText: /^\s*.*\{\s*$/, afterText: /^\s*\}/, action: { indentAction: monaco.languages.IndentAction.IndentOutdent } }, { beforeText: /^\s*.*\{\s*$/, action: { indentAction: monaco.languages.IndentAction.Indent } }]
  });
  monaco.languages.setMonarchTokensProvider('cp-cpp', {
    ctrl: ctrlCpp, kw: kwCpp, cons: consCpp, types: typesCpp,
    tokenizer: {
      root: [
        [/\/\/.*$/, 'comment'], [/\/\*/, 'comment', '@comment'],
        [/^\s*#\s*\w+/, 'keyword.control', '@pp'],
        [/[a-zA-Z_]\w*(?=\s*\()/, { cases: { '@ctrl': 'keyword.control', '@kw': 'keyword', '@cons': 'constant', '@types': 'type', '@default': 'function' } }],
        [/[a-zA-Z_]\w*(?=\s*<)/, { cases: { '@ctrl': 'keyword.control', '@kw': 'keyword', '@types': 'type', '@default': 'type' } }],
        [/[a-zA-Z_]\w*/, { cases: { '@ctrl': 'keyword.control', '@kw': 'keyword', '@cons': 'constant', '@types': 'type', '[A-Z][A-Za-z0-9_]*': 'type', '@default': 'variable' } }],
        [/\d+\.\d*([eE][-+]?\d+)?[fFlL]?|\.\d+([eE][-+]?\d+)?[fFlL]?|0[xX][0-9a-fA-F']+[uUlL]*|0[bB][01']+[uUlL]*|\d[\d']*[uUlL]*/, 'number'],
        [/[{}()\[\]]/, '@brackets'],
        [/(->|::|<<=|>>=|<<|>>|\+\+|--|[+\-*\/%=!<>&|^~?:]+|[.,;])/, 'operator'],
        [/"/, 'string', '@strd'], [/'(\\.|[^\\'])'/, 'string'],
        [/\s+/, '']
      ],
      pp: [[/<[^>\n]*>/, 'string'], [/"[^"\n]*"/, 'string'], [/\/\/.*$/, 'comment', '@pop'], [/\\\s*$/, 'keyword.control'], [/$/, '', '@pop'], [/[^<"\n\/\\]+/, 'keyword.control'], [/./, 'keyword.control']],
      comment: [[/[^\/*]+/, 'comment'], [/\*\//, 'comment', '@pop'], [/[\/*]/, 'comment']],
      strd: [[/[^\\"]+/, 'string'], [/\\./, 'string'], [/"/, 'string', '@pop']]
    }
  });
  // ---- Java
  const ctrlJava = 'if else for while do switch case default break continue return try catch finally throw throws assert yield'.split(' ');
  const kwJava = 'public private protected static final abstract class interface enum extends implements import package new this super instanceof native synchronized transient volatile strictfp record sealed permits var'.split(' ');
  const consJava = ['true', 'false', 'null'];
  const typesJava = 'int long double float char boolean byte short void String Integer Long Double Boolean Character Math System Scanner BufferedReader InputStreamReader PrintWriter StringBuilder ArrayList List Map HashMap Set HashSet TreeMap TreeSet Arrays Collections Deque ArrayDeque PriorityQueue Queue LinkedList Iterator Object StringTokenizer BigInteger'.split(' ');
  monaco.languages.register({ id: 'cp-java' });
  monaco.languages.setLanguageConfiguration('cp-java', {
    comments: { lineComment: '//', blockComment: ['/*', '*/'] },
    brackets: [['{', '}'], ['[', ']'], ['(', ')']],
    autoClosingPairs: [{ open: '{', close: '}' }, { open: '[', close: ']' }, { open: '(', close: ')' }, { open: '"', close: '"', notIn: ['string'] }, { open: "'", close: "'", notIn: ['string', 'comment'] }],
    onEnterRules: [{ beforeText: /^\s*.*\{\s*$/, afterText: /^\s*\}/, action: { indentAction: monaco.languages.IndentAction.IndentOutdent } }, { beforeText: /^\s*.*\{\s*$/, action: { indentAction: monaco.languages.IndentAction.Indent } }]
  });
  monaco.languages.setMonarchTokensProvider('cp-java', {
    ctrl: ctrlJava, kw: kwJava, cons: consJava, types: typesJava,
    tokenizer: {
      root: [
        [/\/\/.*$/, 'comment'], [/\/\*/, 'comment', '@comment'],
        [/@[a-zA-Z_]\w*/, 'type'],
        [/[a-zA-Z_$][\w$]*(?=\s*\()/, { cases: { '@ctrl': 'keyword.control', '@kw': 'keyword', '@cons': 'constant', '@types': 'type', '@default': 'function' } }],
        [/[a-zA-Z_$][\w$]*/, { cases: { '@ctrl': 'keyword.control', '@kw': 'keyword', '@cons': 'constant', '@types': 'type', '[A-Z][A-Za-z0-9_]*': 'type', '@default': 'variable' } }],
        [/\d+\.\d*([eE][-+]?\d+)?[fFdD]?|\.\d+([eE][-+]?\d+)?[fFdD]?|0[xX][0-9a-fA-F_]+[lL]?|0[bB][01_]+[lL]?|\d[\d_]*[lLfFdD]?/, 'number'],
        [/[{}()\[\]]/, '@brackets'],
        [/(->|::|<<=|>>>=|>>=|<<|>>>|>>|\+\+|--|[+\-*\/%=!<>&|^~?:]+|[.,;])/, 'operator'],
        [/"""/, 'string', '@str3'], [/"/, 'string', '@strd'], [/'(\\.|[^\\'])'/, 'string'],
        [/\s+/, '']
      ],
      comment: [[/[^\/*]+/, 'comment'], [/\*\//, 'comment', '@pop'], [/[\/*]/, 'comment']],
      strd: [[/[^\\"]+/, 'string'], [/\\./, 'string'], [/"/, 'string', '@pop']],
      str3: [[/[^"\\]+/, 'string'], [/\\./, 'string'], [/"""/, 'string', '@pop'], [/"/, 'string']]
    }
  });
  // ---- JavaScript
  const ctrlJs = 'if else for while do switch case default break continue return try catch finally throw await yield import export from as of in'.split(' ');
  const kwJs = 'const let var function class extends new this super typeof instanceof void delete async static get set with debugger'.split(' ');
  const consJs = ['true', 'false', 'null', 'undefined', 'NaN', 'Infinity'];
  const typesJs = 'Math Number String Array Object BigInt Map Set JSON Promise Symbol Date RegExp Error Boolean Int32Array Float64Array Uint8Array BigInt64Array process console require module Buffer'.split(' ');
  monaco.languages.register({ id: 'cp-js' });
  monaco.languages.setLanguageConfiguration('cp-js', {
    comments: { lineComment: '//', blockComment: ['/*', '*/'] },
    brackets: [['{', '}'], ['[', ']'], ['(', ')']],
    autoClosingPairs: [{ open: '{', close: '}' }, { open: '[', close: ']' }, { open: '(', close: ')' }, { open: '"', close: '"', notIn: ['string'] }, { open: "'", close: "'", notIn: ['string', 'comment'] }, { open: '`', close: '`', notIn: ['string', 'comment'] }],
    onEnterRules: [{ beforeText: /^\s*.*\{\s*$/, afterText: /^\s*\}/, action: { indentAction: monaco.languages.IndentAction.IndentOutdent } }, { beforeText: /^\s*.*\{\s*$/, action: { indentAction: monaco.languages.IndentAction.Indent } }]
  });
  monaco.languages.setMonarchTokensProvider('cp-js', {
    ctrl: ctrlJs, kw: kwJs, cons: consJs, types: typesJs,
    tokenizer: {
      root: [
        [/\/\/.*$/, 'comment'], [/\/\*/, 'comment', '@comment'],
        [/[a-zA-Z_$][\w$]*(?=\s*\()/, { cases: { '@ctrl': 'keyword.control', '@kw': 'keyword', '@cons': 'constant', '@types': 'type', '@default': 'function' } }],
        [/[a-zA-Z_$][\w$]*(?=\s*=>)/, 'function'],
        [/[a-zA-Z_$][\w$]*/, { cases: { '@ctrl': 'keyword.control', '@kw': 'keyword', '@cons': 'constant', '@types': 'type', '[A-Z][A-Za-z0-9_]*': 'type', '@default': 'variable' } }],
        [/\d+\.\d*([eE][-+]?\d+)?|\.\d+([eE][-+]?\d+)?|0[xX][0-9a-fA-F_]+n?|0[bB][01_]+n?|0[oO][0-7_]+n?|\d[\d_]*n?/, 'number'],
        [/[{}()\[\]]/, '@brackets'],
        [/(=>|\?\?=|\?\.|\?\?|\*\*=|\*\*|<<=|>>>=|>>=|<<|>>>|>>|\+\+|--|&&=|\|\|=|[+\-*\/%=!<>&|^~?:]+|[.,;])/, 'operator'],
        [/"/, 'string', '@strd'], [/'/, 'string', '@strs'], [/`/, 'string', '@tpl'],
        [/\s+/, '']
      ],
      comment: [[/[^\/*]+/, 'comment'], [/\*\//, 'comment', '@pop'], [/[\/*]/, 'comment']],
      strd: [[/[^\\"]+/, 'string'], [/\\./, 'string'], [/"/, 'string', '@pop']],
      strs: [[/[^\\']+/, 'string'], [/\\./, 'string'], [/'/, 'string', '@pop']],
      tpl: [[/\$\{/, 'operator', '@tplExpr'], [/[^\\`$]+/, 'string'], [/\\./, 'string'], [/\$/, 'string'], [/`/, 'string', '@pop']],
      tplExpr: [[/\}/, 'operator', '@pop'], [/[a-zA-Z_$][\w$]*/, 'variable'], [/\d+/, 'number'], [/[^}\w$]+/, 'operator']]
    }
  });
  for (const [name, C, base] of [['cp-dark', DARK, 'vs-dark'], ['cp-light', LIGHT, 'vs']]) {
    monaco.editor.defineTheme(name, {
      base, inherit: true,
      rules: [
        { token: 'comment', foreground: C.com, fontStyle: 'italic' }, { token: 'string', foreground: C.str }, { token: 'number', foreground: C.num },
        { token: 'keyword.control', foreground: C.kw }, { token: 'keyword', foreground: C.cons }, { token: 'constant', foreground: C.cons },
        { token: 'type', foreground: C.type }, { token: 'function', foreground: C.fn }, { token: 'variable', foreground: C.vr }, { token: 'operator', foreground: C.op },
        { token: 'delimiter', foreground: C.op }
      ],
      colors: {
        'editor.background': C.bg, 'editorGutter.background': C.bg, 'editor.foreground': C.text,
        'editorLineNumber.foreground': C.ln, 'editorLineNumber.activeForeground': C.lnActive,
        'editor.selectionBackground': C.sel, 'editorCursor.foreground': C.cursor,
        'editor.lineHighlightBackground': C.bg, 'editor.lineHighlightBorder': C.bg,
        'editorBracketHighlight.foreground1': '#' + C.br[0], 'editorBracketHighlight.foreground2': '#' + C.br[1], 'editorBracketHighlight.foreground3': '#' + C.br[2],
        'editorBracketHighlight.unexpectedBracket.foreground': '#ef4743',
        'editorIndentGuide.background': C.bg, 'editorIndentGuide.activeBackground': C.bg,
        'editorWidget.background': light() ? '#ffffff' : '#222222', 'editorWidget.border': light() ? '#e2e4e8' : '#363636',
        'editorSuggestWidget.background': light() ? '#ffffff' : '#222222', 'editorSuggestWidget.border': light() ? '#e2e4e8' : '#363636',
        'scrollbarSlider.background': light() ? '#c9ccd280' : '#4a4a4a80', 'scrollbarSlider.hoverBackground': light() ? '#c9ccd2' : '#4a4a4a', 'scrollbarSlider.activeBackground': light() ? '#c9ccd2' : '#4a4a4a'
      }
    });
  }
}

function initMonaco(cb) {
  require.config({ paths: { vs: 'vs' } });
  require(['vs/editor/editor.main'], () => {
    defineLanguages();
    editor = monaco.editor.create($('#monaco'), {
      value: '', language: 'cp-python', theme: light() ? 'cp-light' : 'cp-dark',
      fontFamily: "Menlo, Monaco, Consolas, 'Courier New', monospace", fontSize: S.fontSize, lineHeight: Math.round(S.fontSize * 1.6),
      minimap: { enabled: false }, scrollBeyondLastLine: false, renderLineHighlight: 'none', lineNumbersMinChars: 3, glyphMargin: false,
      folding: false, lineDecorationsWidth: 10, tabSize: 4, insertSpaces: true, automaticLayout: false, wordWrap: 'off',
      overviewRulerLanes: 0, hideCursorInOverviewRuler: true, overviewRulerBorder: false, padding: { top: 14, bottom: 14 },
      scrollbar: { verticalScrollbarSize: 10, horizontalScrollbarSize: 10, useShadows: false },
      bracketPairColorization: { enabled: true }, guides: { bracketPairs: false, indentation: false }, matchBrackets: 'always',
      renderWhitespace: 'none', cursorBlinking: 'smooth', smoothScrolling: true, contextmenu: true, mouseWheelZoom: false,
      'semanticHighlighting.enabled': false, quickSuggestions: { other: true, comments: false, strings: false }, suggestOnTriggerCharacters: true,
      stickyScroll: { enabled: false }
    });
    editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.Enter, () => run());
    editor.addCommand(monaco.KeyCode.Escape, () => { if (S.focus) exitFocus(); });
    editor.onDidChangeModelContent(() => {
      if (suppressChange) return;
      const p = prob();
      if (p.empty) return;
      p.code[lang()] = editor.getValue();
      saveCodeDebounced(p.id, lang(), p.code[lang()]);
      updateGutter();
    });
    editor.onDidChangeCursorPosition((e) => {
      S.curLn = e.position.lineNumber; S.curCol = e.position.column;
      const pos = $('#curpos'); if (pos) pos.textContent = `Ln ${S.curLn}, Col ${S.curCol}  ·  Saved`;
    });
    editor.onMouseDown((e) => {
      const t = e.target;
      if (t.type === monaco.editor.MouseTargetType.GUTTER_LINE_NUMBERS || t.type === monaco.editor.MouseTargetType.GUTTER_GLYPH_MARGIN || t.type === monaco.editor.MouseTargetType.GUTTER_LINE_DECORATIONS) {
        toggleBreakpoint(t.position.lineNumber);
      }
    });
    editor.onDidChangeModel(() => updateGutter());
    monacoReady = true;
    cb && cb();
  });
}

function modelFor(p, lg) {
  const key = (p.empty ? '__empty' : p.id) + ':' + lg;
  if (!models[key]) {
    const code = p.empty ? S.templates[lg] : (p.code[lg] ?? S.templates[lg]);
    models[key] = monaco.editor.createModel(code, { cpp: 'cp-cpp', java: 'cp-java', js: 'cp-js' }[lg] || 'cp-python');
  }
  return models[key];
}

function dropModels(prefix) {
  for (const k of Object.keys(models)) if (!prefix || k.startsWith(prefix + ':')) { models[k].dispose(); delete models[k]; }
}

function mountEditor() {
  if (!monacoReady) return;
  const host = $('#editor-host');
  const slot = $('#editor-slot');
  if (!slot) { if (host.parentElement !== document.body) document.body.appendChild(host); host.classList.remove('mounted'); return; }
  if (host.parentElement !== slot) slot.appendChild(host);
  host.classList.add('mounted');
  const p = prob();
  const m = modelFor(p, lang());
  suppressChange = true;
  if (editor.getModel() !== m) editor.setModel(m);
  suppressChange = false;
  editor.updateOptions({ readOnly: !!p.empty, fontSize: S.fontSize, lineHeight: Math.round(S.fontSize * 1.6) });
  monaco.editor.setTheme(light() ? 'cp-light' : 'cp-dark');
  editor.layout();
  updateGutter();
}

function updateGutter() {
  if (!monacoReady) return;
  const p = prob();
  const bps = p.bps || [];
  const lines = editor.getModel() ? editor.getModel().getLineCount() : 0;
  const decs = bps.filter((n) => n <= lines).map((n) => ({ range: new monaco.Range(n, 1, n, 1), options: { isWholeLine: false, linesDecorationsClassName: 'bp-line-num', stickiness: monaco.editor.TrackedRangeStickiness.NeverGrowsWhenTypingAtEdges } }));
  bpDecorations = editor.deltaDecorations(bpDecorations, decs);
  const dl = S.debugging && S.debugLine && S.debugLine <= lines ? [{ range: new monaco.Range(S.debugLine, 1, S.debugLine, 1), options: { isWholeLine: true, className: 'dbg-line', linesDecorationsClassName: 'dbg-line-num' } }] : [];
  dbgDecorations = editor.deltaDecorations(dbgDecorations, dl);
  if (dl.length) editor.revealLineInCenterIfOutsideViewport(S.debugLine);
}

function toggleBreakpoint(n) {
  const p = prob();
  if (p.empty) return;
  const cur = p.bps || [];
  p.bps = cur.includes(n) ? cur.filter((x) => x !== n) : [...cur, n].sort((a, b) => a - b);
  rpc('saveState', { id: p.id, bps: p.bps });
  updateGutter();
  if (S.layout === 'debug') render();
}

const saveCodeDebounced = debounce((id, lg, code) => rpc('saveCode', { id, lang: lg, code }), 500);
const saveTestsDebounced = debounce((id, ts) => rpc('saveTests', { id, tests: ts.map((t) => ({ in: t.in, out: t.out, custom: !!t.custom })) }), 500);
const saveNotesDebounced = debounce((id, notes) => rpc('saveState', { id, notes }), 500);

// -------------------------------------------------------------------- timer
function persistTimer(p) {
  if (!p || p.empty) return;
  rpc('saveState', { id: p.id, timeSeconds: p.timeSeconds, timerMode: p.timerMode, paused: p.paused });
}
let tickN = 0;
setInterval(() => {
  const p = prob();
  if (p.empty || p.solved || p.paused || S.stressOpen && false) return;
  if (p.timerMode === 'down') {
    if (p.timeSeconds > 0) {
      p.timeSeconds--;
      if (p.timeSeconds === 0) toast(`Time's up on ${p.id}!`, 'var(--bad)');
    }
  } else p.timeSeconds++;
  const el = $('#timerText'); if (el) el.textContent = fmt(p.timeSeconds);
  if (++tickN % 10 === 0) persistTimer(p);
}, 1000);

// ------------------------------------------------------------------ actions
function setActive(id) {
  if (S.active === id) return;
  persistTimer(prob());
  S.active = id; S.focusCase = 0; S.compileError = '';
  if (S.debugging) stopDebug();
  rpc('setActive', { id });
  render();
}

function setLayout(l) {
  S.layout = l; S.layoutMenuOpen = false;
  if (l === 'default' || l === 'leet') S.testsCollapsed = true;
  if (l !== 'debug' && S.debugging) stopDebug();
  rpc('saveUi', { layout: l });
  render();
}

function toggleFocus() { S.focus = !S.focus; S.layoutMenuOpen = false; S.focusCase = 0; rpc('saveUi', { focus: S.focus }); render(); }
function exitFocus() { if (S.focus) { S.focus = false; rpc('saveUi', { focus: false }); render(); } }

function setTheme(t) {
  S.theme = t;
  document.documentElement.setAttribute('data-theme', t);
  rpc('saveUi', { theme: t });
  render();
}

function closeMenus() { S.contestMenuOpen = S.layoutMenuOpen = S.timerMenuOpen = S.importOpen = false; }

function testsPayload() { return tests().map((t) => ({ in: t.in, out: t.out, custom: !!t.custom })); }

async function run() {
  const p = prob();
  if (S.running || p.empty) { if (p.empty) toast('No problem open — import one with Competitive Companion or a URL', 'var(--bad)'); return; }
  if (!tests().length) { toast('No test cases — add one with + Add test', 'var(--bad)'); return; }
  S.running = true; S.compileError = ''; S.testsCollapsed = false; S.compiling = lang() === 'cpp';
  p.attempted = true;
  for (const t of p.tests) { t.status = 'idle'; t.got = ''; }
  render();
  const res = await rpc('run', { id: p.id, lang: lang(), code: currentCode(), tests: testsPayload() });
  if (res && res.ok === false) { S.running = false; S.compiling = false; toast(res.error || 'Could not start', 'var(--bad)'); render(); }
}

async function submit() {
  const p = prob();
  if (p.empty) return;
  // The core decides whether a submission is really in flight (a judge window waiting for a
  // login can be replaced by a new Submit), so no S.judging guard here.
  S.judging = true; S.judgeMsg = 'Judging…';
  render();
  const res = await rpc('submit', { id: p.id, lang: lang(), code: currentCode() });
  if (res && res.ok === false) { S.judging = false; toast(res.error || 'Could not submit', 'var(--bad)'); render(); }
}

function addTest() {
  const p = prob(); if (p.empty) return;
  p.tests.push({ in: '', out: '', custom: true, status: 'idle', got: '' });
  S.testsCollapsed = false;
  saveTestsDebounced(p.id, p.tests);
  render();
}

function delTest(i) {
  const p = prob(); if (p.empty) return;
  p.tests.splice(i, 1);
  if (S.focusCase >= p.tests.length) S.focusCase = Math.max(0, p.tests.length - 1);
  saveTestsDebounced(p.id, p.tests);
  render();
}

async function startDebug() {
  const p = prob(); if (p.empty || S.debugging || S.debugStarting) return;
  if (!tests().length) { toast('Add a test case first — the debugger feeds its input to your program', 'var(--bad)'); return; }
  S.debugStarting = true; S.consoleText = ''; S.dbgVars = []; S.dbgStack = []; S.debugLine = null;
  render();
  const res = await rpc('debugStart', { id: p.id, lang: lang(), code: currentCode(), bps: p.bps || [], testIndex: 0 });
  S.debugStarting = false;
  if (!res || !res.ok) { S.debugging = false; toast(res && res.error ? res.error.split('\n')[0] : 'Could not start the debugger', 'var(--bad)'); S.consoleText = (res && res.error) || ''; render(); return; }
  S.debugging = true;
  render();
}
function stopDebug() { S.debugging = false; S.debugLine = null; S.dbgVars = []; S.dbgStack = []; rpc('debugCmd', { cmd: 'stop' }); updateGutter(); }
function debugCmd(cmd) {
  if (!S.debugging) return;
  if (cmd === 'stop') { stopDebug(); render(); return; }
  rpc('debugCmd', { cmd });
}

async function createSession() {
  const name = S.sessionName.trim();
  if (S.sessionMode === 'url' && !S.sessionUrl.trim()) { toast('Paste a problem URL first', 'var(--bad)'); return; }
  S.sessionBusy = true; render();
  const res = await rpc('newSession', { name, mode: S.sessionMode, ratingMin: parseInt(S.ratingMin) || 1200, ratingMax: parseInt(S.ratingMax) || 1500, url: S.sessionUrl.trim() });
  S.sessionBusy = false;
  if (!res || res.ok === false) { toast((res && res.error) || 'Could not create the session', 'var(--bad)'); render(); return; }
  if (res.session) applySession(res.session, res.contests, res.toast);
  S.sessionOpen = false; S.contestMenuOpen = false; S.sessionName = ''; S.sessionUrl = '';
  render();
}

// Problems whose statement never arrived (offline at import, app restarted) get one more try.
function refetchMissingStatements() {
  for (const p of problems()) if ((!p.statementHtml || (p.statementVersion || 0) < 2) && p.url && !S.stmtErrors[p.id]) rpc('refetchStatement', { id: p.id });
}

function applySession(session, contests, tst, activate) {
  persistTimer(prob());
  S.session = session; if (contests) S.contests = contests;
  S.active = activate || (session && session.active) || (session && session.problems[0] ? session.problems[0].id : null);
  S.focusCase = 0; S.compileError = ''; S.running = false; S.compiling = false;
  if (S.debugging) stopDebug();
  dropModels();
  if (tst) toast(tst.msg, tst.color);
}

async function openContest(dir) {
  S.contestMenuOpen = false;
  if (S.session && S.session.dir === dir) { toast('This contest is already open', 'var(--accent)'); render(); return; }
  const res = await rpc('openContest', { dir });
  if (res && res.ok) { applySession(res.session, res.contests, { msg: `Opened "${res.session.name}"`, color: 'var(--accent)' }); refetchMissingStatements(); }
  else toast('Could not open that contest', 'var(--bad)');
  render();
}

async function deleteContest(dir, name) {
  const wasOpen = S.session && S.session.dir === dir;
  const res = await rpc('deleteContest', { dir });
  if (res && res.ok) {
    if (wasOpen) applySession(res.session || null, res.contests);
    else S.contests = res.contests;
    toast(`Deleted "${name}" — folder removed from cp/contests/`, 'var(--bad)');
  } else toast((res && res.error) || 'Could not delete', 'var(--bad)');
  S.contestMenuOpen = true;
  render();
}

async function importUrl() {
  const url = S.importUrl.trim();
  if (!url) { toast('Paste a problem URL first', 'var(--bad)'); return; }
  S.importOpen = false; S.importUrl = ''; S.busy = 'Fetching the problem…'; renderToast(); render();
  const res = await rpc('importUrl', { url });
  if (res && res.ok === false) { S.busy = ''; renderToast(); toast(res.error || 'Import failed', 'var(--bad)'); }
}

async function formatCode() {
  const p = prob(); if (p.empty) return;
  const res = await rpc('format', { lang: lang(), code: currentCode() });
  if (res && res.ok) {
    suppressChange = true; editor.setValue(res.code); suppressChange = false;
    p.code[lang()] = res.code; saveCodeDebounced(p.id, lang(), res.code);
    toast('Formatted', 'var(--accent)');
  } else toast((res && res.message) || 'Formatter not available', 'var(--bad)');
}

function resetTemplate() {
  const p = prob(); if (p.empty) return;
  const code = S.templates[lang()];
  suppressChange = true; editor.setValue(code); suppressChange = false;
  p.code[lang()] = code; saveCodeDebounced(p.id, lang(), code);
  toast('Code reset to template', 'var(--accent)');
}

function setLang(l) {
  const p = prob(); if (p.empty) return;
  p.lang = l; rpc('saveState', { id: p.id, lang: l });
  render();
}

function adoptStress() {
  const p = prob(); if (p.empty) return;
  p.tests.push({ in: S.stress.input, out: S.stress.expected, custom: true, status: 'idle', got: '' });
  saveTestsDebounced(p.id, p.tests);
  S.stressOpen = false; S.stress = { state: 'idle', iter: 0, input: '', expected: '', got: '', message: '' }; S.testsCollapsed = false;
  toast('Counterexample added as custom test', 'var(--ok)');
  render();
}

// --------------------------------------------------------------- templates
const ICON_PLAY = '<svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor"><path d="M6 4l14 8-14 8z"></path></svg>';
const ICON_CLOCK = '<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4"><circle cx="12" cy="13" r="8"></circle><path d="M12 9v4l2.5 2.5M9 2h6"></path></svg>';
const ICON_LAYOUT = '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="18" height="18" rx="2"></rect><path d="M9 3v18M15 9h6M15 15h6"></path></svg>';

const LANGS = [['python', 'Python 3'], ['cpp', 'C++ (g++ 15)'], ['java', 'Java'], ['js', 'JavaScript (Node)']];
function langSelectHtml() {
  return `<select class="sel" data-sel="lang">${LANGS.map(([v, l]) => `<option value="${v}"${lang() === v ? ' selected' : ''}>${l}</option>`).join('')}</select>`;
}

function tSummary() {
  const T = tests();
  const done = T.filter((t) => t.status === 'pass').length;
  const failed = T.some((t) => t.status === 'fail' || t.status === 'tle' || t.status === 're');
  const anyRun = T.some((t) => t.status !== 'idle');
  const text = !anyRun ? `${T.length} case${T.length === 1 ? '' : 's'}` : failed ? `${done}/${T.length} passed` : done === T.length ? `All ${done} passed ✓` : `${done}/${T.length}…`;
  const color = failed ? 'var(--bad)' : anyRun && done === T.length && T.length ? 'var(--ok)' : 'var(--muted)';
  return { text, color, anyRun, failed, done };
}
function statusText(t) { return t.status === 'pass' ? 'PASS' : t.status === 'fail' ? 'FAIL' : t.status === 'tle' ? 'TLE' : t.status === 're' ? 'RE' : t.status === 'running' ? '' : '—'; }
function statusClass(t) { return t.status === 'pass' ? 'pass' : (t.status === 'fail' || t.status === 'tle' || t.status === 're') ? 'fail' : ''; }
function showGot(t) { return statusClass(t) === 'fail' && !!t.got; }

function timerHtml() {
  const p = prob();
  return `<div class="rel">
    <div class="timer${p.solved ? ' solved' : ''}" data-act="timerMenu" title="Click to pause, reset or set the timer">${ICON_CLOCK}<span id="timerText">${fmt(p.timeSeconds || 0)}</span>${p.paused ? '<span class="paused">⏸</span>' : ''}</div>
    ${S.timerMenuOpen ? `<div class="menu timer-menu" data-stop="1">
      <div class="seg"><button class="${p.timerMode === 'up' ? 'on' : ''}" data-act="timerMode" data-arg="up">Stopwatch</button><button class="${p.timerMode === 'down' ? 'on' : ''}" data-act="timerMode" data-arg="down">Countdown</button></div>
      <div class="row"><button class="btn-sm" data-act="timerPause">${p.paused ? 'Resume' : 'Pause'}</button><button class="btn-sm danger" data-act="timerReset">Reset 0:00</button></div>
      <div class="row">${[10, 20, 40].map((m) => `<button class="preset" data-act="timerPreset" data-arg="${m}">${m}:00</button>`).join('')}</div>
      <div class="row"><input class="inp" data-in="timerEdit" value="${esc(S.timerEdit)}" placeholder="mm:ss"><button class="btn-sm acc fit" data-act="timerSet">Set</button></div>
    </div>` : ''}
  </div>`;
}

function topbarHtml() {
  const p = prob();
  const logo = light() ? 'assets/logo.png' : 'assets/logo-white.png';
  if (S.focus) {
    return `<div class="topbar focus">
      <div class="logo" title="cp — competitive programming IDE"><img src="${logo}" alt="cp logo"></div>
      <div class="grow"></div>
      <button class="btn-ghost" data-act="prev">‹ Prev</button><button class="btn-ghost" data-act="next">Next ›</button>
      <div class="grow"></div>
      ${timerHtml()}
      <button class="btn-ghost b" data-act="focus">Exit Focus</button>
    </div>`;
  }
  const contestName = S.session ? S.session.name : 'No session';
  const tabs = problems().map((pr) => `<button class="tab${pr.id === S.active ? ' active' : ''}" data-act="tab" data-arg="${esc(pr.id)}">${esc(pr.id)}${pr.solved || pr.attempted ? `<span class="dot" style="background:${pr.solved ? 'var(--ok)' : 'var(--bad)'}"></span>` : ''}</button>`).join('');
  const contests = S.contests.map((c) => `<div class="contest-row" data-act="openContest" data-arg="${esc(c.dir)}">
      <div style="flex:1;min-width:0"><div class="name" style="font-weight:${c.active ? 600 : 400}">${esc(c.name)}</div><div class="meta">${esc(c.date)} · ${esc(c.meta)}</div></div>
      <button class="xbtn" data-act="deleteContest" data-arg="${esc(c.dir)}" data-name="${esc(c.name)}" title="Delete contest folder${c.active ? ' (it is open now)' : ''}">✕</button>
    </div>`).join('');
  return `<div class="topbar">
    <div class="logo" title="cp — competitive programming IDE"><img src="${logo}" alt="cp logo"></div>
    <div class="vsep"></div>
    <div class="rel">
      <button class="contest-btn" data-act="contestMenu">${esc(contestName)} <span class="caret">▾</span></button>
      ${S.contestMenuOpen ? `<div class="menu contest-menu" data-stop="1">
        <div class="head"><div class="lbl">Saved contests</div><div class="grow"></div><button class="btn-outline-accent" data-act="newSession">+ New session</button></div>
        ${contests || '<div class="empty" style="padding:12px 14px">No saved contests yet.</div>'}
        <div class="foot" title="${esc(S.root)}\\contests">Stored on disk in cp/contests/ — deleting removes the folder. <a href="#" data-act="setupOpen">Setup…</a></div>
      </div>` : ''}
    </div>
    <div class="tabs">${tabs}
      <div class="rel"><button class="tab-add" data-act="importMenu" title="Import a problem: Competitive Companion (localhost:10045) or paste a URL">+</button>
      ${S.importOpen ? `<div class="menu import-menu" data-stop="1">
        <div class="hint">Listening for <b>Competitive Companion</b> on localhost:10045 — click a problem or contest in your browser and it appears here. Or paste a problem URL:</div>
        <div class="row"><input class="inp" data-in="importUrl" value="${esc(S.importUrl)}" placeholder="https://codeforces.com/problemset/problem/…"><button class="btn-sm acc fit" data-act="importUrl">Import</button></div>
      </div>` : ''}</div>
    </div>
    <div class="grow"></div>
    <button class="btn-run" data-act="run">${S.running ? '<span class="spin"></span>' : ICON_PLAY}Run</button>
    <button class="btn-submit" data-act="submit">${S.judging ? '<span class="spin white"></span>' : ''}${S.judging ? 'Judging…' : 'Submit'}</button>
    <div class="grow"></div>
    ${timerHtml()}
    <div class="rel">
      <button class="iconbtn" data-act="layoutMenu" title="Layouts">${ICON_LAYOUT}</button>
      ${S.layoutMenuOpen ? layoutMenuHtml() : ''}
    </div>
    <button class="btn-history" data-act="history">History</button>
    <button class="iconbtn" data-act="theme" title="Toggle theme">${light() ? '◐' : '◑'}</button>
  </div>`;
}

function layoutMenuHtml() {
  const box = (flex) => `<div class="box" style="flex:${flex}"></div>`;
  const col = (flex, inner) => `<div class="col" style="flex:${flex}">${inner}</div>`;
  const card = (id, name, wire) => `<button class="laycard${S.layout === id ? ' on' : ''}" data-act="layout" data-arg="${id}"><div class="wire">${wire}</div><div class="nm">${name}</div></button>`;
  return `<div class="menu layout-menu" data-stop="1">
    <div class="ttl">Layouts</div>
    <div class="layout-grid">
      ${card('default', 'Default', box(45) + col(55, box(3) + box(1)))}
      ${card('leet', 'Leet', box(35) + col(40, box(6) + box(1)) + box(25))}
      ${card('note', 'Note-taking', box(30) + box(40) + col(30, box(65) + box(35)))}
      ${card('debug', 'Debug', box(30) + box(40) + col(30, box(7) + box(3)))}
    </div>
    <div class="hr"></div>
    <button class="btn-focus${S.focus ? ' on' : ''}" data-act="focus">${S.focus ? 'Exit Focus Mode (Esc)' : '⌖ Focus Mode'}</button>
  </div>`;
}

const stmtCache = {};
// Second line of defence for fetched statement HTML (the core strips scripts too):
// drop code-bearing elements, event handlers and javascript:/data: URLs before innerHTML.
function sanitizeHtml(html) {
  const tpl = document.createElement('template');
  tpl.innerHTML = html;
  const bad = new Set(['SCRIPT', 'STYLE', 'IFRAME', 'OBJECT', 'EMBED', 'SVG', 'MATH', 'TEMPLATE', 'NOSCRIPT', 'FORM', 'LINK', 'META', 'BASE', 'FRAME', 'FRAMESET', 'APPLET', 'INPUT', 'BUTTON', 'TEXTAREA', 'SELECT']);
  const walk = (node) => {
    for (const el of Array.from(node.children)) {
      if (bad.has(el.tagName)) { el.remove(); continue; }
      for (const a of Array.from(el.attributes)) {
        const n = a.name.toLowerCase(), v = a.value.trim().toLowerCase();
        if (n.startsWith('on') || n === 'style' || n === 'srcdoc' || ((n === 'href' || n === 'src' || n === 'xlink:href' || n === 'formaction') && /^(javascript|data|vbscript):/.test(v))) el.removeAttribute(a.name);
      }
      walk(el);
    }
  };
  walk(tpl.content);
  return tpl.innerHTML;
}

function statementHtml(p, focusStyle) {
  if (p.empty) return `<p style="margin:0 0 12px;text-wrap:pretty">This session is empty. Click a problem or contest in your browser with Competitive Companion, or add one from a URL via the <b>+</b> button.</p>`;
  const samples = (p.tests || []).filter((t) => !t.custom);
  let body = '';
  if (p.statementHtml && p.statementExact) {
    // The judge's own problem block: title, limits, samples and note come with it.
    return `<div class="stmt cf" data-stmt="${esc(p.id)}">${stmtCache[p.id] || sanitizeHtml(p.statementHtml)}</div>`;
  }
  if (p.statementHtml) {
    body = `<div class="stmt" data-stmt="${esc(p.id)}">${stmtCache[p.id] || sanitizeHtml(p.statementHtml)}</div>`;
  } else {
    const err = S.stmtErrors[p.id];
    body = `<p style="margin:0 0 12px">${err ? `Statement could not be fetched (${esc(err)}).` : 'Fetching the statement…'} ${p.url ? `<a href="#" data-act="openUrl" data-arg="${esc(p.url)}">Open on ${esc(p.judge)}</a> · <a href="#" data-act="refetch">Retry</a>` : ''}</p>
      <div class="figbox">Statement text and images render here once the problem page has been fetched — samples below come from Competitive Companion</div>`;
  }
  const ex = samples.map((s, i) => focusStyle
    ? `<div class="fex"><div class="lbl">Example ${i + 1}</div><div class="blk"><div class="l">Input:</div><pre>${esc(s.in)}</pre><div class="l">Output:</div><pre>${esc(s.out)}</pre></div></div>`
    : `<div class="example"><div class="lbl">Example ${i + 1}</div><div class="exbox"><div class="h">Input</div><pre>${esc(s.in)}</pre><div class="h out">Output</div><pre>${esc(s.out)}</pre></div></div>`).join('');
  return body + ex;
}

function chipsHtml(p) {
  return `<span class="chip mono">${p.rating ? '*' + p.rating : '*—'}</span><span class="chip">${esc(p.tl)} · ${esc(p.ml)}</span>${p.url ? `<span class="chip link" data-act="openUrl" data-arg="${esc(p.url)}" title="${esc(p.url)}">${esc(p.judge)} ↗</span>` : ''}${p.empty ? '' : `<span class="chip link" data-act="openFolder" title="Open the problem folder">folder ↗</span>`}`;
}

function subsHtml(p) {
  const subs = S.history.filter((h) => h.prob === p.id && (!S.session || h.contest === S.session.name));
  const judgeNames = { codeforces: 'Codeforces', atcoder: 'AtCoder', cses: 'CSES', usaco: 'USACO' };
  if (!subs.length) return `<div class="empty">No submissions for ${esc(p.id)} yet — hit Submit.${judgeNames[p.judge] ? ` <a href="#" data-act="judgeLogin" data-arg="${p.judge}">${judgeNames[p.judge]} account…</a>` : ''}</div>`;
  return `<div class="subs">${subs.map((h) => `<div class="subrow"><span class="v ${h.ok ? 'ok' : 'bad'}">${esc(h.verdict)}</span><div class="grow"></div><span class="l">${esc(h.lang)}</span><span class="t">${esc(h.at)}</span></div>`).join('')}</div>`;
}

function problemPaneHtml() {
  const p = prob();
  const w = S.layout === 'default' ? '45%' : S.layout === 'leet' ? '35%' : '30%';
  const tabs = [['desc', 'Description'], ['subs', 'Submissions']].map(([id, l]) => `<button class="ptab${S.probTab === id ? ' on' : ''}" data-act="probTab" data-arg="${id}">${l}</button>`).join('');
  const body = S.probTab === 'subs'
    ? `<div class="pbody" style="padding:14px 16px 32px">${subsHtml(p)}</div>`
    : `<div class="pbody">
        ${p.statementExact && p.statementHtml ? '' : `<div class="ptitle">${esc(p.id)}. ${esc(p.title)}</div>`}
        <div class="chips">${chipsHtml(p)}</div>
        <div style="height:16px"></div>
        ${statementHtml(p, false)}
      </div>`;
  return `<div class="pane problem" style="width:${w}"><div class="ptabs">${tabs}</div>${body}</div>`;
}

function testCardsHtml() {
  return tests().map((t, i) => `<div class="tcard ${statusClass(t)}">
      <div class="th"><span class="tname">${t.custom ? 'Custom' : 'Case'} ${i + 1}</span><span class="tstat ${statusClass(t)}">${statusText(t)}</span>${t.status === 'running' ? '<span class="spin sm"></span>' : ''}<div class="grow"></div>${t.custom ? `<button class="xbtn" data-act="delTest" data-arg="${i}">✕</button>` : ''}</div>
      <div class="grid">
        <div><div class="lb">Input</div><textarea rows="3" data-tin="${i}" spellcheck="false">${esc(t.in)}</textarea></div>
        <div><div class="lb">Expected</div><textarea rows="3" data-tout="${i}" spellcheck="false">${esc(t.out)}</textarea></div>
      </div>
      ${showGot(t) ? `<div class="got"><div class="lb bad">Got${t.ms ? ` · ${t.ms} ms` : ''}</div><pre>${esc(t.got)}</pre></div>` : ''}
    </div>`).join('');
}

function editorPaneHtml() {
  const p = prob();
  const isDebug = S.layout === 'debug';
  return `<div class="editor-pane">
    <div class="editor-head">
      <span class="code-glyph">&lt;/&gt;</span><span class="pane-title">Code</span>
      ${langSelectHtml()}
      <div class="grow"></div>
      ${isDebug ? `<button class="btn-hd dbg${S.debugging ? ' on' : ''}" data-act="debugStart">${S.debugStarting ? '<span class="spin sm"></span>' : ''}Debug ▶</button>` : ''}
      <button class="btn-hd" data-act="stress">Stress</button>
    </div>
    <div class="editor-slot" id="editor-slot"></div>
    ${S.compileError ? `<div class="compile-error"><div class="h">Compile error<div class="grow"></div><button class="xbtn" data-act="closeCompile">✕</button></div><pre>${esc(S.compileError)}</pre></div>` : ''}
  </div>`;
}

function testsPaneHtml() {
  const sm = tSummary();
  const h = S.testsCollapsed ? '40px' : S.layout === 'leet' ? '170px' : '25%';
  return `<div class="tests-pane" style="height:${h};flex:0 0 ${h}">
    <div class="tests-head" data-act="testsToggle"><span class="chev">${S.testsCollapsed ? '▲' : '▼'}</span><div class="pane-title">Testcase</div><div class="summary" style="color:${sm.color}">${sm.text}</div><div class="grow"></div><button class="btn-add" data-act="addTest">+ Add test</button></div>
    ${S.testsCollapsed ? '' : `<div class="tests-strip">${testCardsHtml()}</div>`}
  </div>`;
}

function rightColHtml() {
  const p = prob();
  const sm = tSummary();
  const lay = S.layout;
  const w = lay === 'leet' ? '25%' : '30%';
  let inner = '';
  if (lay === 'leet') {
    inner = `<div class="pane" style="flex:1"><div class="pane-head" style="height:38px;flex:0 0 38px"><div class="pane-title">Test Result</div><div class="summary" style="color:${sm.color}">${sm.text}</div></div>
      <div class="result-list">${!sm.anyRun ? '<div class="empty" style="padding:10px 4px">Run your code to see results here.</div>' : ''}
      ${tests().map((t, i) => `<div class="rcard ${statusClass(t)}"><div class="rh"><span class="tname">${t.custom ? 'Custom' : 'Case'} ${i + 1}</span><span class="tstat ${statusClass(t)}">${statusText(t)}</span>${t.status === 'running' ? '<span class="spin sm"></span>' : ''}</div>${showGot(t) ? `<div class="got"><div class="lb">Got</div><pre>${esc(t.got)}</pre></div>` : ''}</div>`).join('')}
      </div></div>`;
  } else if (lay === 'note') {
    inner = `<div class="pane" style="flex:65"><div class="pane-head" style="height:38px;flex:0 0 38px;gap:8px"><span class="muted" style="font-size:12px">✎</span><div class="pane-title">Notes — ${esc(p.id)}</div></div>
      <textarea class="notes" data-in="notes" placeholder="Observations, edge cases, complexity…"${p.empty ? ' disabled' : ''}>${esc(p.notes || '')}</textarea></div>
      <div class="pane" style="flex:35"><div class="pane-head" style="height:38px;flex:0 0 38px"><div class="pane-title">Testcase · Result</div><div class="summary" style="color:${sm.color}">${sm.text}</div></div>
      <div class="mini-list">${tests().map((t, i) => `<div class="mini-row ${statusClass(t)}"><span class="tname">${t.custom ? 'Custom' : 'Case'} ${i + 1}</span><span class="tstat ${statusClass(t)}">${statusText(t)}</span></div>`).join('')}</div></div>`;
  } else if (lay === 'debug') {
    const bps = p.bps || [];
    inner = `<div class="pane" style="flex:7"><div class="pane-head" style="height:38px;flex:0 0 38px;padding:0 12px"><div class="pane-title">Debugger</div><div class="grow"></div>
        <button class="dbgbtn go" data-act="dbg" data-arg="continue" title="Continue">▶</button><button class="dbgbtn" data-act="dbg" data-arg="next" title="Step over">⤼</button><button class="dbgbtn" data-act="dbg" data-arg="step" title="Step into">⭣</button><button class="dbgbtn" data-act="dbg" data-arg="out" title="Step out">⭡</button><button class="dbgbtn stop" data-act="dbg" data-arg="stop" title="Stop">■</button></div>
      <div class="dbg-body">
        ${!S.debugging ? '<div class="hint">Click line numbers to set breakpoints, then press Debug ▶ above the editor.</div>' : `
          <div class="paused">${S.debugLine ? `paused at line ${S.debugLine}` : 'running…'}</div>
          <div class="sh first">Variables</div>
          ${S.dbgVars.length ? S.dbgVars.map((v) => `<div class="kv"><span class="k">${esc(v.k)}</span><span class="eq">=</span><span class="v">${esc(v.v)}</span></div>`).join('') : '<div class="muted" style="font-size:12px">—</div>'}
          <div class="sh">Call Stack</div>
          ${S.dbgStack.map((f) => `<div class="kv"><span>${esc(f.fn)}</span><span class="muted">line ${esc(f.line)}</span></div>`).join('')}`}
        <div class="sh">Breakpoints</div>
        ${bps.length ? bps.map((n) => `<div class="bprow"><span class="d"></span>line ${n}</div>`).join('') : '<div class="muted" style="font-size:12px">None set.</div>'}
      </div></div>
      <div class="pane" style="flex:3"><div class="console-head">Console</div><pre class="console" id="console">${esc(S.consoleText || (S.debugging ? '' : (tSummary().anyRun ? (tSummary().failed ? 'process exited · wrong answer on a case' : 'process exited with code 0') : 'Run or Debug to see output.')))}</pre></div>`;
  }
  return `<div class="right-col" style="width:${w};flex:0 0 ${w}">${inner}</div>`;
}

function historyHtml() {
  return `<div class="pane history"><div class="hh">Submissions<div class="grow"></div><button class="xbtn lg" data-act="history">✕</button></div>
    <div class="hb">${!S.history.length ? '<div class="empty" style="padding:12px 4px">No submissions yet.</div>' : S.history.map((h) => `<div class="hrow"><div class="top"><span class="p">${esc(h.prob)}</span><span class="l">${esc(h.lang)}</span><div class="grow"></div><span class="t">${esc(h.at)}</span></div><div class="v ${h.ok ? 'ok' : 'bad'}">${esc(h.verdict)}</div></div>`).join('')}</div></div>`;
}

function focusHtml() {
  const p = prob();
  const sm = tSummary();
  const T = tests();
  const sel = T[S.focusCase] || T[0] || { in: '', out: '', got: '' };
  const pills = [['desc', 'Description'], ['subs', 'Submissions']].map(([id, l]) => `<button class="pill${S.probTab === id ? ' on' : ''}" data-act="probTab" data-arg="${id}">${l}</button>`).join('');
  return `<div class="focus-scroll"><div class="focus-col">
    ${p.statementExact && p.statementHtml ? '' : `<div class="focus-title">${esc(p.id)}. ${esc(p.title)}</div>`}
    <div class="pills">${pills}</div>
    ${S.probTab === 'desc' ? `<div class="focus-body"><div class="chips">${chipsHtml(p)}</div>${statementHtml(p, true)}</div>` : `<div style="margin-top:18px">${subsHtml(p)}</div>`}
    <div class="fcard">
      <div class="fh">
        ${langSelectHtml()}
        <div class="grow"></div>
        <button class="sqbtn mono" data-act="format" title="Format code">{}</button><button class="sqbtn" data-act="reset" title="Reset to template">↺</button>
      </div>
      <div class="fbody"><div class="editor-slot" id="editor-slot" style="position:absolute;inset:0"></div></div>
      <div class="ff"><span class="pos" id="curpos">Ln ${S.curLn}, Col ${S.curCol}  ·  Saved</span><div class="grow"></div>
        <button class="btn-run" data-act="run">${S.running ? '<span class="spin"></span>' : ''}Run</button>
        <button class="btn-submit" data-act="submit">${S.judging ? '<span class="spin white"></span>' : ''}${S.judging ? 'Judging…' : 'Submit'}</button></div>
      ${S.compileError ? `<div class="compile-error"><div class="h">Compile error<div class="grow"></div><button class="xbtn" data-act="closeCompile">✕</button></div><pre>${esc(S.compileError)}</pre></div>` : ''}
    </div>
    <div class="fcard tests">
      <div class="fh t"><div class="ttl">Testcase</div><div class="summary" style="color:${sm.color}">${sm.text}</div><div class="grow"></div><button class="btn-add" data-act="addTest">+ Add test</button></div>
      <div class="fcases">
        ${T.map((t, i) => `<div class="fcase${S.focusCase === i ? ' on' : ''}" data-act="focusCase" data-arg="${i}"><span class="n">${i + 1}</span><span class="pv">${esc((t.in || '(empty)').replace(/\n/g, ' ⏎ '))}</span><span class="st ${statusClass(t)}" style="color:${statusClass(t) === 'pass' ? 'var(--ok)' : statusClass(t) === 'fail' ? 'var(--bad)' : 'var(--muted)'}">${statusText(t)}</span></div>`).join('')}
        <div class="fdetail">
          <div class="lb">Input</div><pre>${esc(sel.in)}</pre>
          <div class="lb">Expected</div><pre style="margin-bottom:0">${esc(sel.out)}</pre>
          ${showGot(sel) ? `<div class="lb bad">Got</div><pre class="got">${esc(sel.got)}</pre>` : ''}
        </div>
      </div>
    </div>
  </div></div>`;
}

function setupHtml() {
  const logo = light() ? 'assets/logo.png' : 'assets/logo-white.png';
  const cfgKey = { python: 'python', gpp: 'cppCompiler', javac: 'javac', java: 'java', node: 'node' };
  const toolRows = S.tools.filter((t) => t.id !== 'gdb').map((t) => {
    const editing = S.editTool === t.id;
    let control = '';
    if (editing) {
      control = `<input class="inp" style="flex:1;width:auto" data-in="toolPath" value="${esc(t.path || '')}" placeholder="full path to the executable, empty = auto-detect"><button class="btn-sm acc fit" data-act="saveTool" data-arg="${t.id}">Save</button><button class="btn-sm fit" data-act="editTool" data-arg="">Cancel</button>`;
    } else if (t.id === 'gpp' && S.cppCandidates.length > 1) {
      control = `<select class="sel" data-sel="cppCompiler">${S.cppCandidates.map((c) => `<option value="${esc(c)}"${c === t.path ? ' selected' : ''}>${esc(c)}</option>`).join('')}<option value="__custom">Custom path…</option></select>`;
    } else {
      control = `<span class="val" title="${esc(t.path || t.hint)}">${t.found ? esc(t.path) : 'not found — ' + esc(t.hint)}</span>` +
        (t.found ? '' : (t.hint.startsWith('winget ') ? `<button class="btn-sm acc fit" data-act="installTool" data-arg="${t.id}">Install</button>` : '')) +
        `<button class="btn-sm fit" data-act="editTool" data-arg="${t.id}" title="Use a specific executable">Path…</button>`;
    }
    return `<div class="setup-row"><span class="st ${t.found ? 'ok' : 'bad'}">${t.found ? '✓' : '✕'}</span><span class="lbl">${esc(t.label)}</span>${control}</div>` +
      (t.id === 'gpp' ? `<div class="setup-row"><span class="st wait">·</span><span class="lbl">C++ flags</span><span class="val body">passed to the compiler before <code>main.cpp -o sol.exe</code></span><input class="inp" style="flex:none;width:220px" data-sel="cppFlags" value="${esc(S.cppFlags)}"></div>` : '');
  }).join('');
  const judgeRows = JUDGES.map(([id, name]) => {
    const st = S.judgeStatus[id];
    const cls = !st ? 'wait' : st.loggedIn ? 'ok' : 'bad';
    const txt = !st ? 'checking…' : st.loggedIn ? `logged in${st.handle ? ' as ' + esc(st.handle) : ''}` : 'not logged in';
    return `<div class="setup-row"><span class="st ${cls}">${!st ? '·' : st.loggedIn ? '✓' : '✕'}</span><span class="lbl">${name}</span>
      <span class="val body">${txt}</span><button class="btn-sm fit" data-act="judgeLogin" data-arg="${id}">${st && st.loggedIn ? 'Open' : 'Log in…'}</button></div>`;
  }).join('');
  return `<div class="setup"><div class="setup-col">
    <div class="setup-head"><img src="${logo}" alt=""><div><div class="setup-title">Set up CP IDE</div><div class="setup-sub">Four quick checks. Everything here can be changed later from the contest menu → Setup.</div></div></div>
    <div class="setup-sec"><div class="sh"><span class="n">1</span><span class="t">Languages</span><span class="d">what Run and Debug use</span><div class="grow"></div><button class="btn-sm fit" data-act="recheckTools">Re-check</button></div>${toolRows}</div>
    <div class="setup-sec"><div class="sh"><span class="n">2</span><span class="t">Judges</span><span class="d">log in once, Submit does the rest inside the app</span><div class="grow"></div><button class="btn-sm fit" data-act="judgeStatusAll">Re-check</button></div>${judgeRows}</div>
    <div class="setup-sec"><div class="sh"><span class="n">3</span><span class="t">Competitive Companion</span><span class="d">sends problems from your browser</span></div>
      <div class="setup-row"><span class="st ok">✓</span><span class="lbl">Listening</span><span class="val body">CP IDE is listening on <code>localhost:${S.port}</code>. Install the extension, then click the green + on any problem or contest page.</span>
        <button class="btn-sm fit" data-act="openUrl" data-arg="https://github.com/jmerle/competitive-companion#readme">Get the extension</button></div>
      <div class="setup-row"><span class="st wait">·</span><span class="lbl">Port</span><span class="val body">Port ${S.port} is in the extension's default list. If it is not, open the extension's options and add it.</span></div>
    </div>
    <div class="setup-sec"><div class="sh"><span class="n">4</span><span class="t">Preferences</span></div>
      <div class="setup-row"><span class="st wait">·</span><span class="lbl">Default language</span><span class="val body">used for new problems</span>
        <select class="sel" data-sel="defaultLang">${LANGS.map(([v, l]) => `<option value="${v}"${S.defaultLang === v ? ' selected' : ''}>${l}</option>`).join('')}</select></div>
      <div class="setup-row"><span class="st wait">·</span><span class="lbl">Theme</span><span class="val body"></span>
        <select class="sel" data-sel="setupTheme"><option value="dark"${!light() ? ' selected' : ''}>Dark</option><option value="light"${light() ? ' selected' : ''}>Light</option></select></div>
      <div class="setup-row"><span class="st wait">·</span><span class="lbl">Editor font size</span><span class="val body">12–20 px</span>
        <input class="inp" type="number" min="12" max="20" data-sel="fontSize" value="${S.fontSize}"></div>
      <div class="setup-row"><span class="st wait">·</span><span class="lbl">Data folder</span><span class="val" title="${esc(S.root)}">${esc(S.root)}</span>
        <button class="btn-sm fit" data-act="openFolder" data-arg="root">Open</button></div>
    </div>
    <div class="setup-foot"><button class="btn-primary" style="margin-top:0" data-act="setupFinish">Done</button><button class="skip" data-act="setupFinish">Skip for now</button></div>
  </div></div>`;
}

function sessionModalHtml() {
  const m = S.sessionMode;
  return `<div class="overlay" data-act="sessionClose"><div class="modal session" data-stop="1">
    <div class="mh"><div class="mt">New practice session</div><div class="grow"></div><button class="xbtn lg" data-act="sessionClose">✕</button></div>
    <input class="inp body full" data-in="sessionName" value="${esc(S.sessionName)}" placeholder="Session name, e.g. DP practice">
    <div class="seg wide"><button class="${m === 'blank' ? 'on' : ''}" data-act="sessionMode" data-arg="blank">Blank</button><button class="${m === 'random' ? 'on' : ''}" data-act="sessionMode" data-arg="random">Random by rating</button><button class="${m === 'url' ? 'on' : ''}" data-act="sessionMode" data-arg="url">From URL</button></div>
    ${m === 'blank' ? '<div class="note">Starts empty — add problems by clicking them in your browser (Companion) or pasting URLs.</div>' : ''}
    ${m === 'random' ? `<div class="rating"><input class="inp" data-in="ratingMin" value="${esc(S.ratingMin)}"><span class="dash">–</span><input class="inp" data-in="ratingMax" value="${esc(S.ratingMax)}"><span class="lbl">3 unsolved CF problems in this range${S.config.cfHandle ? ` (for ${esc(S.config.cfHandle)})` : ''}</span></div>` : ''}
    ${m === 'url' ? `<input class="inp full mt10" data-in="sessionUrl" value="${esc(S.sessionUrl)}" placeholder="https://codeforces.com/problemset/problem/…">` : ''}
    <button class="btn-primary" data-act="sessionCreate"${S.sessionBusy ? ' disabled' : ''}>${S.sessionBusy ? 'Creating…' : 'Create session'}</button>
  </div></div>`;
}

function stressModalHtml() {
  const p = prob();
  const st = S.stress;
  const base = p.path ? p.path.replace(/\\/g, '/').split('/contests/').pop() : `${S.session ? S.session.dir : ''}/${p.id}`;
  return `<div class="overlay" data-act="stressClose"><div class="modal stress" data-stop="1">
    <div class="mh"><div class="mt">Stress test — ${esc(p.id)}</div><div class="grow"></div><button class="xbtn lg" data-act="stressClose">✕</button></div>
    <div class="desc">Runs your solution against a brute-force reference on random inputs until they disagree.</div>
    <div class="paths"><div class="pr"><span class="k">gen</span><span class="v" title="${esc(p.path || '')}\\gen.py">${esc(base)}/gen.py</span></div><div class="pr"><span class="k">brute</span><span class="v" title="${esc(p.path || '')}\\brute.py">${esc(base)}/brute.py</span></div></div>
    ${st.state === 'running' ? `<div class="stress-run"><span class="spin"></span>${st.message === 'compiling' ? 'compiling…' : `iteration ${st.iter} — no mismatch yet`}</div><button class="btn-adopt" data-act="stressStop">Stop</button>` : ''}
    ${st.state === 'found' ? `<div class="stress-found"><div class="h">${esc(st.message || 'Mismatch')} at iteration ${st.iter}</div><pre>input:\n${esc(st.input)}\nexpected: ${esc(st.expected)}\ngot:      ${esc(st.got)}</pre></div><button class="btn-adopt" data-act="stressAdopt">Add as test case</button> <button class="btn-adopt" data-act="stressStart">Run again</button>` : ''}
    ${st.state === 'error' ? `<div class="stress-err">${esc(st.message)}</div><button class="btn-adopt" data-act="stressStart">Retry</button>` : ''}
    ${st.state === 'idle' || st.state === 'stopped' ? `<button class="btn-primary" data-act="stressStart">Start</button>` : ''}
  </div></div>`;
}

function render() {
  const app = $('#app');
  const host = $('#editor-host');
  if (host && host.parentElement && host.parentElement !== document.body) document.body.appendChild(host);
  const focusedIn = document.activeElement && document.activeElement.getAttribute ? document.activeElement.getAttribute('data-in') : null;
  let html = `<div class="app">${topbarHtml()}`;
  if (S.focus) html += focusHtml();
  else {
    const showTests = S.layout === 'default' || S.layout === 'leet';
    const rightOpen = S.layout !== 'default';
    html += `<div class="work">${problemPaneHtml()}<div class="center-col">${editorPaneHtml()}${showTests ? testsPaneHtml() : ''}</div>${rightOpen ? rightColHtml() : ''}${S.historyOpen ? historyHtml() : ''}</div>`;
  }
  if (S.setupOpen) html += setupHtml();
  if (S.sessionOpen) html += sessionModalHtml();
  if (S.stressOpen) html += stressModalHtml();
  html += '</div>';
  app.innerHTML = html;
  if (focusedIn) { const el = $(`[data-in="${focusedIn}"]`); if (el) { el.focus(); try { el.selectionStart = el.selectionEnd = el.value.length; } catch (e) {} } }
  mountEditor();
  renderMath();
}

function renderMath() {
  const p = prob();
  const el = $('.stmt[data-stmt]');
  if (!el || stmtCache[p.id] || !window.renderMathInElement) return;
  try {
    renderMathInElement(el, { delimiters: [{ left: '$$$', right: '$$$', display: false }, { left: '$$', right: '$$', display: true }, { left: '\\(', right: '\\)', display: false }, { left: '\\[', right: '\\]', display: true }], throwOnError: false });
    stmtCache[p.id] = el.innerHTML;
  } catch (e) { /* offline: leave raw text */ }
}

// --------------------------------------------------------------- events UI
document.addEventListener('click', (e) => {
  const actEl = e.target.closest('[data-act]');
  const inMenu = e.target.closest('[data-stop]');
  if (!actEl) {
    if (!inMenu && (S.contestMenuOpen || S.layoutMenuOpen || S.timerMenuOpen || S.importOpen)) { closeMenus(); render(); }
    return;
  }
  const act = actEl.getAttribute('data-act'), arg = actEl.getAttribute('data-arg');
  const p = prob();
  if (['sessionClose', 'stressClose'].includes(act) && inMenu && e.target !== actEl) return;  // click inside modal body
  if (!['contestMenu', 'layoutMenu', 'timerMenu', 'importMenu'].includes(act) && !inMenu && (S.contestMenuOpen || S.layoutMenuOpen || S.timerMenuOpen || S.importOpen)) closeMenus();
  switch (act) {
    case 'contestMenu': { const o = S.contestMenuOpen; closeMenus(); S.contestMenuOpen = !o; render(); break; }
    case 'layoutMenu': { const o = S.layoutMenuOpen; closeMenus(); S.layoutMenuOpen = !o; render(); break; }
    case 'timerMenu': { const o = S.timerMenuOpen; closeMenus(); S.timerMenuOpen = !o; render(); break; }
    case 'importMenu': { const o = S.importOpen; closeMenus(); S.importOpen = !o; render(); if (S.importOpen) { const i = $('[data-in="importUrl"]'); i && i.focus(); } break; }
    case 'importUrl': importUrl(); break;
    case 'newSession': S.sessionOpen = true; S.contestMenuOpen = false; render(); setTimeout(() => { const i = $('[data-in="sessionName"]'); i && i.focus(); }, 0); break;
    case 'sessionClose': S.sessionOpen = false; render(); break;
    case 'sessionMode': S.sessionMode = arg; render(); break;
    case 'sessionCreate': createSession(); break;
    case 'openContest': openContest(arg); break;
    case 'deleteContest': e.stopPropagation(); deleteContest(arg, actEl.getAttribute('data-name')); break;
    case 'tab': setActive(arg); break;
    case 'run': run(); break;
    case 'submit': submit(); break;
    case 'timerMode': if (!p.empty) { p.timerMode = arg; persistTimer(p); } render(); break;
    case 'timerPause': if (!p.empty) { p.paused = !p.paused; persistTimer(p); } render(); break;
    case 'timerReset': if (!p.empty) { p.timeSeconds = 0; persistTimer(p); } S.timerMenuOpen = false; render(); toast(`Timer reset for ${p.id}`, 'var(--accent)'); break;
    case 'timerPreset': { const m = parseInt(arg); if (!p.empty) { p.timerMode = 'down'; p.paused = false; p.timeSeconds = m * 60; persistTimer(p); } S.timerMenuOpen = false; render(); toast(`Countdown — ${m} min on ${p.id}`, 'var(--accent)'); break; }
    case 'timerSet': {
      const parts = S.timerEdit.trim().split(':').map(Number);
      if (!S.timerEdit.trim() || parts.some(isNaN) || parts.length > 2) { toast('Use mm:ss or seconds', 'var(--bad)'); break; }
      const secs = parts.length === 2 ? parts[0] * 60 + parts[1] : parts[0];
      if (!p.empty) { p.timeSeconds = Math.max(0, secs); persistTimer(p); }
      S.timerMenuOpen = false; S.timerEdit = ''; render(); break;
    }
    case 'layout': setLayout(arg); break;
    case 'focus': toggleFocus(); break;
    case 'prev': { const i = problems().findIndex((x) => x.id === S.active); if (i > 0) setActive(problems()[i - 1].id); break; }
    case 'next': { const i = problems().findIndex((x) => x.id === S.active); if (i >= 0 && i < problems().length - 1) setActive(problems()[i + 1].id); break; }
    case 'history': S.historyOpen = !S.historyOpen; render(); break;
    case 'theme': setTheme(light() ? 'dark' : 'light'); break;
    case 'probTab': S.probTab = arg; render(); break;
    case 'testsToggle': S.testsCollapsed = !S.testsCollapsed; render(); break;
    case 'addTest': e.stopPropagation(); addTest(); break;
    case 'delTest': delTest(parseInt(arg)); break;
    case 'focusCase': S.focusCase = parseInt(arg); render(); break;
    case 'stress': S.stressOpen = true; if (S.stress.state !== 'running') S.stress = { state: 'idle', iter: 0, input: '', expected: '', got: '', message: '' }; render(); break;
    case 'stressClose': S.stressOpen = false; if (S.stress.state === 'running') rpc('stressStop'); S.stress.state = 'idle'; render(); break;
    case 'stressStart': if (p.empty) break; S.stress = { state: 'running', iter: 0, input: '', expected: '', got: '', message: '' }; render(); rpc('stressStart', { id: p.id, lang: lang(), code: currentCode() }).then((r) => { if (r && r.ok === false) { S.stress = { state: 'error', message: r.error || 'Could not start' }; render(); } }); break;
    case 'stressStop': rpc('stressStop'); break;
    case 'stressAdopt': adoptStress(); break;
    case 'debugStart': if (S.debugging) { stopDebug(); render(); } else startDebug(); break;
    case 'dbg': debugCmd(arg); break;
    case 'format': formatCode(); break;
    case 'reset': resetTemplate(); break;
    case 'closeCompile': S.compileError = ''; render(); break;
    case 'openUrl': e.preventDefault(); rpc('openExternal', { url: arg }); break;
    case 'judgeLogin': e.preventDefault(); rpc('judgeLogin', { judge: arg || 'codeforces' }); break;
    case 'setupOpen': e.preventDefault(); closeMenus(); S.setupOpen = true; render(); rpc('recheckTools').then((r) => { if (r && r.tools) { S.tools = r.tools; render(); } }); judgeStatusAll(); break;
    case 'setupFinish': S.setupOpen = false; rpc('saveUi', { setupDone: true }); render(); break;
    case 'recheckTools': rpc('recheckTools').then((r) => { if (r && r.tools) { applyTools(r); render(); toast(S.tools.filter((t) => !t.found && t.id !== 'gdb').length ? 'Some tools are still missing' : 'All languages ready', 'var(--accent)'); } }); break;
    case 'editTool': S.editTool = arg || ''; S.toolPath = ''; render(); break;
    case 'saveTool': {
      const key = { python: 'python', gpp: 'cppCompiler', javac: 'javac', java: 'java', node: 'node' }[arg];
      const val = ($('[data-in="toolPath"]') || {}).value || '';
      S.editTool = '';
      rpc('setTool', { key, value: val }).then((r) => { if (r && r.ok) { applyTools(r); toast(val ? 'Path saved' : 'Back to auto-detect', 'var(--accent)'); } render(); });
      break;
    }
    case 'installTool': rpc('installTool', { id: arg }).then((r) => toast(r && r.ok ? 'Installer opened in a console window — press Re-check when it finishes' : (r && r.error) || 'No installer', r && r.ok ? 'var(--accent)' : 'var(--bad)', 6000)); break;
    case 'judgeStatusAll': judgeStatusAll(); break;
    case 'openFolder': rpc('openFolder', arg === 'root' ? { root: true } : { id: p.id }); break;
    case 'refetch': e.preventDefault(); delete S.stmtErrors[p.id]; rpc('refetchStatement', { id: p.id }); render(); break;
  }
});

document.addEventListener('input', (e) => {
  const t = e.target;
  const p = prob();
  if (t.matches('[data-tin]')) { const i = +t.getAttribute('data-tin'); if (p.tests[i]) { p.tests[i].in = t.value; saveTestsDebounced(p.id, p.tests); } return; }
  if (t.matches('[data-tout]')) { const i = +t.getAttribute('data-tout'); if (p.tests[i]) { p.tests[i].out = t.value; saveTestsDebounced(p.id, p.tests); } return; }
  const key = t.getAttribute && t.getAttribute('data-in');
  if (!key) return;
  if (key === 'notes') { if (!p.empty) { p.notes = t.value; saveNotesDebounced(p.id, t.value); } return; }
  S[key] = t.value;
});

document.addEventListener('change', (e) => {
  const t = e.target;
  if (t.matches('[data-sel="lang"]')) setLang(t.value);
  else if (t.matches('[data-sel="defaultLang"]')) { S.defaultLang = t.value; rpc('saveUi', { defaultLang: t.value }); }
  else if (t.matches('[data-sel="setupTheme"]')) setTheme(t.value);
  else if (t.matches('[data-sel="cppCompiler"]')) {
    if (t.value === '__custom') { S.editTool = 'gpp'; render(); }
    else rpc('setTool', { key: 'cppCompiler', value: t.value }).then((r) => { if (r && r.ok) { applyTools(r); toast('C++ compiler: ' + t.value, 'var(--accent)'); render(); } });
  }
  else if (t.matches('[data-sel="cppFlags"]')) rpc('setTool', { key: 'cppFlags', value: t.value }).then((r) => { if (r && r.ok) { applyTools(r); toast('C++ flags saved', 'var(--accent)'); } });
  else if (t.matches('[data-sel="fontSize"]')) { S.fontSize = Math.min(20, Math.max(12, parseInt(t.value) || 14)); rpc('saveUi', { fontSize: S.fontSize }); if (monacoReady) editor.updateOptions({ fontSize: S.fontSize, lineHeight: Math.round(S.fontSize * 1.6) }); }
});

function applyTools(r) {
  if (r.tools) S.tools = r.tools;
  if (r.cppCandidates) S.cppCandidates = r.cppCandidates;
  if (r.cppFlags !== undefined) S.cppFlags = r.cppFlags;
}

function judgeStatusAll() {
  S.judgeStatus = {};
  render();
  for (const [id] of JUDGES) rpc('judgeStatus', { judge: id });
}

document.addEventListener('keydown', (e) => {
  if (e.ctrlKey && e.key === 'Enter') { e.preventDefault(); run(); }
  if (e.key === 'Escape') {
    if (S.sessionOpen) { S.sessionOpen = false; render(); return; }
    if (S.stressOpen) { S.stressOpen = false; if (S.stress.state === 'running') rpc('stressStop'); S.stress.state = 'idle'; render(); return; }
    if (S.contestMenuOpen || S.layoutMenuOpen || S.timerMenuOpen || S.importOpen) { closeMenus(); render(); return; }
    if (S.focus) exitFocus();
  }
  if (e.key === 'Enter' && e.target.matches && e.target.matches('[data-in="importUrl"]')) importUrl();
  if (e.key === 'Enter' && e.target.matches && e.target.matches('[data-in="timerEdit"]')) $('[data-act="timerSet"]') && $('[data-act="timerSet"]').click();
  if (e.key === 'Enter' && e.target.matches && (e.target.matches('[data-in="sessionName"]') || e.target.matches('[data-in="sessionUrl"]'))) createSession();
});

window.addEventListener('resize', () => { if (monacoReady) editor.layout(); });
window.addEventListener('beforeunload', () => persistTimer(prob()));

// ---------------------------------------------------------- backend events
window.__cp = {
  event(ev) {
    switch (ev.type) {
      case 'session': S.busy = ''; renderToast(); applySession(ev.session, ev.contests, ev.toast, ev.activate); render(); break;
      case 'problem': {
        const list = problems(); const i = list.findIndex((x) => x.id === ev.problem.id);
        if (i >= 0) {
          const old = list[i];
          const oldHasTests = old.tests.some((t) => (t.in || '').trim() || (t.out || '').trim());
          const keep = { timeSeconds: old.timeSeconds, paused: old.paused, timerMode: old.timerMode, code: old.code, tests: oldHasTests ? old.tests : ev.problem.tests, bps: old.bps, notes: old.notes, lang: old.lang };
          list[i] = Object.assign({}, ev.problem, keep);
          delete stmtCache[ev.problem.id];
          if (ev.problem.id === S.active) render();
        }
        break;
      }
      case 'statementFailed': S.stmtErrors[ev.id] = ev.message; if (ev.id === S.active) render(); break;
      case 'compile': S.compiling = ev.state === 'start'; break;
      case 'testStatus': {
        const p = problems().find((x) => x.id === ev.id); if (!p || !p.tests[ev.index]) break;
        const t = p.tests[ev.index]; t.status = ev.status; if (ev.got !== undefined) t.got = ev.got; if (ev.ms !== undefined) t.ms = ev.ms;
        if (ev.id === S.active) render();
        break;
      }
      case 'runDone': {
        S.running = false; S.compiling = false;
        if (ev.compileError) { S.compileError = ev.compileError; toast('Compilation failed — see the error panel', 'var(--bad)'); }
        else if (ev.cancelled) toast('Run stopped', 'var(--muted)');
        else if (ev.ok) toast(`All ${ev.total} test${ev.total === 1 ? '' : 's'} passed · ${ev.ms} ms`, 'var(--ok)');
        else {
          const p = problems().find((x) => x.id === ev.id);
          const bad = p ? p.tests.find((t) => statusClass(t) === 'fail') : null;
          const why = bad ? (bad.status === 'tle' ? 'Time limit exceeded' : bad.status === 're' ? 'Runtime error' : (bad.got === '(no output)' ? 'Wrong answer — solution produced no output' : 'Wrong answer')) : 'Wrong answer';
          toast(`${why} · ${ev.passed}/${ev.total} passed`, 'var(--bad)');
        }
        render();
        break;
      }
      case 'judge': {
        const p = problems().find((x) => x.id === ev.id);
        if (ev.state === 'judging') { S.judging = true; S.judgeMsg = ev.message; if (ev.message) toast(ev.message, 'var(--accent)', 6000); }
        else if (ev.state === 'browser') { S.judging = !!S.config.cfHandle && (p && p.judge === 'codeforces'); toast(ev.message, 'var(--accent)'); }
        else if (ev.state === 'done') {
          S.judging = false; S.historyOpen = true;
          if (ev.history) S.history = ev.history;
          if (p) { p.attempted = true; if (ev.ok) p.solved = true; }
          toast(ev.ok ? `Accepted — ${ev.id} solved in ${fmt(p ? p.timeSeconds : 0)}` : ev.verdict, ev.ok ? 'var(--ok)' : 'var(--bad)');
        } else if (ev.state === 'error') { S.judging = false; toast(ev.message, 'var(--bad)', 7000); }
        else if (ev.state === 'idle') { S.judging = false; }
        render();
        break;
      }
      case 'stress': {
        S.stress = { state: ev.state, iter: ev.iteration, input: ev.input, expected: ev.expected, got: ev.got, message: ev.message };
        if (ev.state === 'stopped') S.stress.state = 'idle';
        if (S.stressOpen) { const runEl = $('.stress-run'); if (ev.state === 'running' && runEl && ev.message !== 'compiling') runEl.lastChild.textContent = `iteration ${ev.iteration} — no mismatch yet`; else render(); }
        break;
      }
      case 'debug': {
        if (ev.event === 'started') { S.debugging = true; S.consoleText = ''; }
        else if (ev.event === 'paused') { S.debugging = true; S.debugLine = ev.line; S.dbgVars = ev.vars || []; S.dbgStack = ev.stack || []; updateGutter(); }
        else if (ev.event === 'output') { S.consoleText += ev.text.replace(/\r\n/g, '\n'); const c = $('#console'); if (c) { c.textContent = S.consoleText; c.scrollTop = c.scrollHeight; return; } }
        else if (ev.event === 'exited') { S.debugging = false; S.debugLine = null; S.dbgVars = []; S.dbgStack = []; S.consoleText += `\nprocess exited with code ${ev.code}`; updateGutter(); toast('Debug session ended', 'var(--accent)'); }
        else if (ev.event === 'error') { S.debugging = false; toast(ev.message, 'var(--bad)'); }
        render();
        break;
      }
      case 'toast': toast(ev.toast.msg, ev.toast.color); break;
      case 'busy': S.busy = ev.message || ''; renderToast(); break;
      case 'judgeStatus': S.judgeStatus[ev.judge] = { loggedIn: !!ev.loggedIn, handle: ev.handle || '' }; if (S.setupOpen) render(); break;
    }
  }
};

// --------------------------------------------------------------------- boot
async function boot() {
  const init = await rpc('init');
  S.root = init.root || ''; S.config = init.config || {}; S.templates = init.templates || S.templates;
  S.contests = init.contests || []; S.history = init.history || [];
  const ui = init.ui || {};
  S.theme = ui.theme === 'light' ? 'light' : 'dark';
  S.layout = ['default', 'leet', 'note', 'debug'].includes(ui.layout) ? ui.layout : 'default';
  S.fontSize = Math.min(20, Math.max(12, ui.fontSize || 14));
  document.documentElement.setAttribute('data-theme', S.theme);
  if (init.session) { S.session = init.session; S.active = init.session.active || (init.session.problems[0] ? init.session.problems[0].id : null); }
  S.tools = init.tools || [];
  S.cppCandidates = (init.config && init.config.cppCandidates) || [];
  S.cppFlags = (init.config && init.config.cppFlags) || '-O2 -std=c++23';
  S.port = init.port || 10045;
  S.defaultLang = ['python', 'cpp', 'java', 'js'].includes(ui.defaultLang) ? ui.defaultLang : 'python';
  S.setupOpen = !ui.setupDone;  // first run: the setup page
  const missing = S.tools.filter((t) => !t.found && t.id !== 'gdb');
  if (missing.length && !S.setupOpen) setTimeout(() => toast('Not found: ' + missing.map((t) => `${t.label} — install with: ${t.hint}`).join('  ·  '), 'var(--bad)', 12000), 1500);
  if (S.setupOpen) setTimeout(judgeStatusAll, 800);
  S.booted = true;
  render();
  initMonaco(() => render());
  refetchMissingStatements();
}
boot();
