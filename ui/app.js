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
  root: '', config: {}, templates: { python: '', cpp: '', java: '', js: '' }, starters: {},
  running: false, runOnly: -1, compiling: false, compileError: '', judging: false, judgeMsg: '',
  historyOpen: false, contestMenuOpen: false, layoutMenuOpen: false, timerMenuOpen: false, importOpen: false,
  timerEdit: '', importUrl: '', confirmDelete: '', shortcutsOpen: false,
  tplOpen: false, tplLang: 'cpp', tplText: '', tplDirty: false, homeOpen: false, ctx: null,
  allContests: false,   // "Show N older" was clicked: list every saved contest, not just the recent ones
  paneW: 0, testsH: 0, rightW: 0, rightTop: 0,
  testsCollapsed: true, probTab: 'desc', tcTab: 'case', selCase: 0, curLn: 1, curCol: 1,
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
function currentCode() { const p = prob(); return p.empty ? (S.templates[lang()] || '') : (p.code[lang()] ?? S.templates[lang()] ?? ''); }

// Same normalisation the runner uses before comparing, so the diff the UI shows
// agrees with the verdict it came with: CRLF -> LF, trailing blanks per line
// dropped, leading/trailing blank lines dropped.
function normOut(s) {
  return String(s == null ? '' : s).replace(/\r/g, '').split('\n').map((l) => l.replace(/[ \t]+$/, '')).join('\n')
    .replace(/\n+$/, '').replace(/^\n+/, '');
}
// First place expected and got part ways, as a line/token pair.
function firstDiff(expected, got) {
  const A = normOut(expected).split('\n'), B = normOut(got).split('\n');
  const n = Math.max(A.length, B.length);
  for (let i = 0; i < n; i++) {
    if (A[i] === B[i]) continue;
    const ta = (A[i] || '').split(/\s+/).filter(Boolean), tb = (B[i] || '').split(/\s+/).filter(Boolean);
    let k = 0;
    while (k < Math.max(ta.length, tb.length) && ta[k] === tb[k]) k++;
    return { line: i + 1, token: k + 1, exp: A[i], got: B[i], expLines: A.length, gotLines: B.length };
  }
  return null;
}
// Two near-misses that cost a lot of time to spot by eye. Neither turns a FAIL
// into a PASS — the judge decides that — but saying so beats staring at
// "0.3333333" next to "0.33333330".
function nearMiss(expected, got) {
  const A = normOut(expected).split(/\s+/).filter(Boolean), B = normOut(got).split(/\s+/).filter(Boolean);
  if (!A.length || A.length !== B.length) return '';
  let caseOnly = true, numeric = true, differs = false;
  for (let i = 0; i < A.length; i++) {
    if (A[i] === B[i]) continue;
    differs = true;
    if (A[i].toLowerCase() !== B[i].toLowerCase()) caseOnly = false;
    const x = Number(A[i]), y = Number(B[i]);
    if (!isFinite(x) || !isFinite(y) || Math.abs(x - y) > 1e-6 * Math.max(1, Math.abs(x), Math.abs(y))) numeric = false;
    if (!caseOnly && !numeric) return '';
  }
  if (!differs) return '';
  if (caseOnly) return 'only the letter case differs — most judges accept either';
  if (numeric) return 'every number matches to 1e-6 — a judge with a tolerance would accept this';
  return '';
}

function copyText(text, what) {
  const done = () => toast(`${what} copied`, 'var(--accent)');
  if (navigator.clipboard && navigator.clipboard.writeText) { navigator.clipboard.writeText(text).then(done, () => fallbackCopy(text, done)); }
  else fallbackCopy(text, done);
}
function fallbackCopy(text, done) {
  const ta = document.createElement('textarea');
  ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = '0';
  document.body.appendChild(ta); ta.select();
  try { document.execCommand('copy'); done(); } catch (e) { toast('Could not copy', 'var(--bad)'); }
  ta.remove();
}

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
      folding: false, lineDecorationsWidth: 20, tabSize: 4, insertSpaces: true, automaticLayout: false, wordWrap: 'off',
      overviewRulerLanes: 0, hideCursorInOverviewRuler: true, overviewRulerBorder: false, padding: { top: 14, bottom: 14 },
      scrollbar: { verticalScrollbarSize: 10, horizontalScrollbarSize: 10, useShadows: false },
      bracketPairColorization: { enabled: true }, guides: { bracketPairs: false, indentation: false }, matchBrackets: 'always',
      renderWhitespace: 'none', cursorBlinking: 'smooth', smoothScrolling: true, contextmenu: true, mouseWheelZoom: false,
      'semanticHighlighting.enabled': false, quickSuggestions: { other: true, comments: false, strings: false }, suggestOnTriggerCharacters: true,
      stickyScroll: { enabled: false }
    });
    // Monaco swallows keys it has a binding for, so every global shortcut is
    // registered here as well — otherwise none of them work while you are typing.
    const K = monaco.KeyCode, M = monaco.KeyMod;
    const bind = [
      [M.CtrlCmd | K.Enter, ACTIONS.run],
      [M.CtrlCmd | M.Shift | K.Enter, ACTIONS.submit],
      [M.CtrlCmd | K.Period, ACTIONS.stop],
      [M.Alt | K.LeftArrow, ACTIONS.prev],
      [M.Alt | K.RightArrow, ACTIONS.next],
      [M.CtrlCmd | M.Alt | K.KeyC, ACTIONS.copyCode],
      [M.CtrlCmd | M.Shift | K.KeyT, ACTIONS.addTest],
      [M.CtrlCmd | M.Shift | K.KeyF, ACTIONS.focusMode],
      [M.CtrlCmd | K.KeyB, ACTIONS.toggleTests],
      [M.CtrlCmd | K.Equal, ACTIONS.fontUp],
      [M.CtrlCmd | K.Minus, ACTIONS.fontDown],
      [M.CtrlCmd | K.Digit0, ACTIONS.fontReset],
      [M.CtrlCmd | K.KeyS, ACTIONS.saved],
      [K.F1, ACTIONS.help],
    ];
    for (const [key, fn] of bind) editor.addCommand(key, fn);
    for (let d = 1; d <= 9; d++) editor.addCommand(M.Alt | K['Digit' + d], () => gotoIndex(d - 1));
    // Only leave Focus mode when Esc is not busy closing a Monaco popup.
    editor.addCommand(K.Escape, () => { closeTop(); }, '!suggestWidgetVisible && !parameterHintsVisible && !renameInputVisible && !findWidgetVisible && !inSnippetMode');
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

// automaticLayout is off (it polls), so the editor is re-laid-out whenever its
// slot actually changes size: pane drags, the resizable focus-mode card, and the
// window itself.
let slotRO = null;
function observeSlot(slot) {
  if (!window.ResizeObserver) return;
  if (!slotRO) slotRO = new ResizeObserver(() => { if (monacoReady) editor.layout(); });
  slotRO.disconnect();
  slotRO.observe(slot);
}

function mountEditor() {
  if (!monacoReady) return;
  const host = $('#editor-host');
  const slot = $('#editor-slot');
  if (!slot) { if (host.parentElement !== document.body) document.body.appendChild(host); host.classList.remove('mounted'); if (slotRO) slotRO.disconnect(); return; }
  if (host.parentElement !== slot) slot.appendChild(host);
  host.classList.add('mounted');
  observeSlot(slot);
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
const saveTestsDebounced = debounce((id, ts) => rpc('saveTests', { id, tests: ts.map((t) => ({ in: t.in, out: t.out, pre: t.pre || '', custom: !!t.custom })) }), 500);
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

// The round clock ticks even when no problem is open (e.g. before the start).
setInterval(() => {
  const el = $('#cclock');
  const c = contestClock();
  if (!el || !c) return;
  el.textContent = clockText(c);
  el.classList.toggle('warn', c.state === 'running' && c.secs <= 15 * 60);
  el.classList.toggle('over', c.state === 'over');
}, 1000);

// ------------------------------------------------------------------ actions
function setActive(id) {
  if (S.active === id) return;
  persistTimer(prob());
  S.active = id; S.selCase = 0; S.tcTab = 'case'; S.compileError = '';
  if (S.debugging) stopDebug();
  rpc('setActive', { id });
  render();
}

function setLayout(l) {
  S.layout = l; S.layoutMenuOpen = false;
  if (l === 'default') S.testsCollapsed = true;
  if (l !== 'debug' && S.debugging) stopDebug();
  rpc('saveUi', { layout: l });
  render();
}

function toggleFocus() { S.focus = !S.focus; S.layoutMenuOpen = false; S.selCase = 0; rpc('saveUi', { focus: S.focus }); render(); }
function exitFocus() { if (S.focus) { S.focus = false; rpc('saveUi', { focus: false }); render(); } }

function setTheme(t) {
  S.theme = t;
  document.documentElement.setAttribute('data-theme', t);
  rpc('saveUi', { theme: t });
  render();
}

function closeMenus() { S.contestMenuOpen = S.layoutMenuOpen = S.timerMenuOpen = S.importOpen = false; }

// `pre` is stdin the box does not show (a split sample's count line) — it has to
// survive every save, or editing a test would silently drop it.
function testsPayload() { return tests().map((t) => ({ in: t.in, out: t.out, pre: t.pre || '', custom: !!t.custom })); }

// Which detected tools a language needs to run at all.
const LANG_TOOLS = { python: ['python'], cpp: ['gpp'], java: ['javac', 'java'], js: ['node'] };
// Reported when you try to use the language, not before: a missing JDK is only a
// problem for someone writing Java.
function toolMissingFor(lg) {
  const need = LANG_TOOLS[lg] || [];
  return S.tools.filter((t) => need.includes(t.id) && !t.found);
}
function warnMissingTool(lg) {
  const miss = toolMissingFor(lg);
  if (!miss.length) return false;
  const label = (LANGS.find(([v]) => v === lg) || [, lg])[1];
  const hint = miss[0].hint ? ` — install it with: ${miss[0].hint}` : '';
  toast(`${label} needs ${miss.map((t) => t.label).join(' and ')}, which was not found${hint}. Contest menu → Setup to point at it.`, 'var(--bad)', 9000);
  return true;
}

async function run(only) {
  const p = prob();
  if (S.running || p.empty) { if (p.empty) toast('No problem open — import one with Competitive Companion or a URL', 'var(--bad)'); return; }
  if (!tests().length) { toast('No test cases — add one with + Add test', 'var(--bad)'); return; }
  if (warnMissingTool(lang())) return;
  if (typeof only !== 'number') only = -1;
  if (p.interactive && only < 0) toast('Interactive problem — sample runs talk to a grader, so this may hang', 'var(--bad)', 5000);
  S.running = true; S.runOnly = only; S.compileError = ''; S.testsCollapsed = false; S.compiling = lang() === 'cpp';
  p.attempted = true;
  for (const t of p.tests) if (only < 0 || p.tests[only] === t) { t.status = 'idle'; t.got = ''; t.ms = 0; }
  render();
  const res = await rpc('run', { id: p.id, lang: lang(), code: currentCode(), tests: testsPayload(), only });
  if (res && res.ok === false) { S.running = false; S.runOnly = -1; S.compiling = false; toast(res.error || 'Could not start', 'var(--bad)'); render(); }
}

function stopRun() {
  if (!S.running) return;
  rpc('stopRun');
  toast('Stopping the run…', 'var(--muted)');
}

// Scrolls the horizontal test strip / result list to a case so a failure is not
// hidden off-screen when there are many samples.
function revealCase(i) {
  const el = document.querySelectorAll('.ctab')[i] || document.querySelectorAll('.fcase')[i];
  if (el && el.scrollIntoView) el.scrollIntoView({ block: 'nearest', inline: 'nearest' });
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
  p.tests.push({ in: '', out: '', pre: '', custom: true, status: 'idle', got: '' });
  S.testsCollapsed = false;
  saveTestsDebounced(p.id, p.tests);
  render();
}

function delTest(i) {
  const p = prob(); if (p.empty) return;
  p.tests.splice(i, 1);
  if (S.selCase >= p.tests.length) S.selCase = Math.max(0, p.tests.length - 1);
  saveTestsDebounced(p.id, p.tests);
  render();
}

async function startDebug() {
  const p = prob(); if (p.empty || S.debugging || S.debugStarting) return;
  if (!tests().length) { toast('Add a test case first — the debugger feeds its input to your program', 'var(--bad)'); return; }
  if (warnMissingTool(lang())) return;
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
  if (S.sessionMode === 'url' && !S.sessionUrl.trim()) { toast('Paste a problem or contest URL first', 'var(--bad)'); return; }
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
  for (const p of problems()) if ((!p.statementHtml || (p.statementVersion || 0) < 7) && p.url && !S.stmtErrors[p.id]) rpc('refetchStatement', { id: p.id });
}

function applySession(session, contests, tst, activate) {
  persistTimer(prob());
  S.session = session; if (contests) S.contests = contests;
  S.active = activate || (session && session.active) || (session && session.problems[0] ? session.problems[0].id : null);
  S.selCase = 0; S.compileError = ''; S.running = false; S.compiling = false;
  // A session arriving means you are working; losing the last one puts you back home.
  S.homeOpen = !session;
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

// Removing a problem deletes its folder (code, tests, notes), so it asks first.
async function deleteProblem(id) {
  const p = problems().find((x) => x.id === id);
  if (!p) return;
  if (!confirmish('delprob:' + id, `Remove ${id}. ${p.title} and its folder?`)) return;
  const res = await rpc('deleteProblem', { id });
  if (!res || res.ok === false) { toast((res && res.error) || 'Could not remove it', 'var(--bad)'); return; }
  applySession(res.session, res.contests, { msg: `${id} removed`, color: 'var(--bad)' });
  render();
}

async function importUrl() {
  const url = S.importUrl.trim();
  if (!url) { toast('Paste a problem or contest URL first', 'var(--bad)'); return; }
  S.importOpen = false; S.importUrl = ''; S.busy = 'Fetching the problem…'; renderToast(); render();
  const res = await rpc('importUrl', { url });
  if (res && res.ok === false) { S.busy = ''; renderToast(); toast(res.error || 'Import failed', 'var(--bad)'); }
}

// Replaces the buffer through an edit rather than setValue() so Ctrl+Z still
// takes you back to what you had before Format / Reset.
function replaceEditorText(code) {
  const m = editor && editor.getModel();
  if (!m) return;
  suppressChange = true;
  editor.pushUndoStop();
  editor.executeEdits('cp', [{ range: m.getFullModelRange(), text: code, forceMoveMarkers: true }]);
  editor.pushUndoStop();
  suppressChange = false;
}

async function formatCode() {
  const p = prob(); if (p.empty) return;
  const res = await rpc('format', { lang: lang(), code: currentCode() });
  if (res && res.ok) {
    replaceEditorText(res.code);
    p.code[lang()] = res.code; saveCodeDebounced(p.id, lang(), res.code);
    toast('Formatted', 'var(--accent)');
  } else toast((res && res.message) || 'Formatter not available', 'var(--bad)');
}

function resetTemplate() {
  const p = prob(); if (p.empty) return;
  const code = S.templates[lang()] || '';
  if (!code && !confirmish('reset', 'Your template for this language is empty — Reset will clear the file. Set one in Setup → Code templates.')) return;
  replaceEditorText(code);
  p.code[lang()] = code; saveCodeDebounced(p.id, lang(), code);
  toast(code ? 'Code reset to your template' : 'Editor cleared (no template set)', 'var(--accent)');
}

// Two-step confirmation without a modal dialog: the first call warns, a second
// call of the same kind within 4 s goes ahead.
const pendingConfirm = {};
function confirmish(kind, message) {
  if (pendingConfirm[kind] && Date.now() - pendingConfirm[kind] < 4000) { delete pendingConfirm[kind]; return true; }
  pendingConfirm[kind] = Date.now();
  toast(message + ' Click again to confirm.', 'var(--bad)', 4000);
  return false;
}

function setLang(l) {
  const p = prob(); if (p.empty) return;
  p.lang = l; rpc('saveState', { id: p.id, lang: l });
  render();
}

function adoptStress() {
  const p = prob(); if (p.empty) return;
  p.tests.push({ in: S.stress.input, out: S.stress.expected, pre: '', custom: true, status: 'idle', got: '' });
  saveTestsDebounced(p.id, p.tests);
  S.stressOpen = false; S.stress = { state: 'idle', iter: 0, input: '', expected: '', got: '', message: '' }; S.testsCollapsed = false;
  toast('Counterexample added as custom test', 'var(--ok)');
  render();
}

// Turns a scratch run ("OUT") into a real expected-output test.
function acceptGot(i) {
  const p = prob(); const t = p.tests && p.tests[i];
  if (!t || !t.got) return;
  t.out = t.got; t.status = 'pass';
  saveTestsDebounced(p.id, p.tests);
  toast(`Case ${i + 1}: output saved as expected`, 'var(--ok)');
  render();
}

function toggleSolved(id) {
  const p = problems().find((x) => x.id === id);
  if (!p) return;
  p.solved = !p.solved;
  if (p.solved) p.attempted = true;
  rpc('saveState', { id, solved: p.solved, attempted: p.attempted });
  toast(`${id} marked ${p.solved ? 'solved' : 'unsolved'}`, p.solved ? 'var(--ok)' : 'var(--muted)');
  render();
}

function dupTest(i) {
  const p = prob();
  const t = p.tests && p.tests[i];
  if (!t) return;
  p.tests.splice(i + 1, 0, { in: t.in, out: t.out, pre: t.pre || '', custom: true, status: 'idle', got: '' });
  S.testsCollapsed = false;
  saveTestsDebounced(p.id, p.tests);
  toast(`Case ${i + 1} duplicated`, 'var(--accent)');
  render();
}

function openTemplates(lg) {
  S.tplOpen = true; S.tplLang = lg || 'cpp'; S.tplText = S.templates[S.tplLang] || ''; S.tplDirty = false;
  render();
}
function saveTemplateIfDirty(announce) {
  if (!S.tplDirty) { if (announce) toast('Template already saved', 'var(--muted)'); return; }
  const lg = S.tplLang, code = S.tplText;
  S.templates[lg] = code; S.tplDirty = false;
  rpc('saveTemplate', { lang: lg, code }).then((r) => {
    if (r && r.templates) S.templates = r.templates;
    if (announce) toast(`${(LANGS.find(([v]) => v === lg) || [, lg])[1]} template saved`, 'var(--ok)');
    render();
  });
}

// --------------------------------------------------------------- templates
const ICON_PLAY = '<svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor"><path d="M6 4l14 8-14 8z"></path></svg>';
const ICON_CLOCK = '<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4"><circle cx="12" cy="13" r="8"></circle><path d="M12 9v4l2.5 2.5M9 2h6"></path></svg>';
// How many saved contests each list shows before the rest are folded behind
// "Show N older" — enough that the usual few are always there, few enough that
// the dropdown stays a menu rather than a scrolling wall.
const MENU_CONTESTS = 7;   // 7 rows x 56px matches the .clist cap, so the short list never scrolls
const HOME_CONTESTS = 5;
const ICON_LAYOUT ='<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="18" height="18" rx="2"></rect><path d="M9 3v18M15 9h6M15 15h6"></path></svg>';
const ICON_STOP = '<svg width="10" height="10" viewBox="0 0 24 24" fill="currentColor"><rect x="5" y="5" width="14" height="14" rx="2"></rect></svg>';

const LANGS = [['python', 'Python 3'], ['cpp', 'C++ (g++ 15)'], ['java', 'Java'], ['js', 'JavaScript (Node)']];
function langSelectHtml() {
  return `<select class="sel" data-sel="lang">${LANGS.map(([v, l]) => `<option value="${v}"${lang() === v ? ' selected' : ''}>${l}</option>`).join('')}</select>`;
}

function graded(t) { return !!(t.out || '').trim(); }   // a test with nothing to compare against is not scored
function tSummary() {
  const T = tests();
  const G = T.filter(graded).length;
  const done = T.filter((t) => t.status === 'pass').length;
  const failed = T.some((t) => t.status === 'fail' || t.status === 'tle' || t.status === 're');
  const anyRun = T.some((t) => t.status !== 'idle');
  const maxMs = T.reduce((m, t) => Math.max(m, t.ms || 0), 0);
  const ms = maxMs ? ` · ${maxMs} ms` : '';
  const text = !anyRun ? `${T.length} case${T.length === 1 ? '' : 's'}`
    : failed ? `${done}/${G} passed${ms}`
    : !G ? `ran${ms}`
    : done === G ? `All ${done} passed ✓${ms}`
    : `${done}/${G}…`;
  const color = failed ? 'var(--bad)' : anyRun && G && done === G ? 'var(--ok)' : 'var(--muted)';
  return { text, color, anyRun, failed, done, graded: G, maxMs };
}
function statusText(t) {
  return t.status === 'pass' ? 'PASS' : t.status === 'fail' ? 'FAIL' : t.status === 'tle' ? 'TLE'
    : t.status === 're' ? 'RE' : t.status === 'out' ? 'OUT' : t.status === 'running' ? '' : '—';
}
function statusClass(t) {
  return t.status === 'pass' ? 'pass' : (t.status === 'fail' || t.status === 'tle' || t.status === 're') ? 'fail' : t.status === 'out' ? 'out' : '';
}
function showGot(t) { return (statusClass(t) === 'fail' || t.status === 'out') && !!t.got; }
function msLabel(t) { return t.ms ? `<span class="ms" title="wall-clock time for this case">${t.ms} ms</span>` : ''; }

// Compact "where did it go wrong" block under a failing case.
function diffHtml(t) {
  if (t.status !== 'fail') return '';
  const d = firstDiff(t.out, t.got);
  if (!d) return '';
  const lineNote = d.expLines !== d.gotLines ? ` · ${d.expLines} line${d.expLines === 1 ? '' : 's'} expected, ${d.gotLines} produced` : '';
  const cell = (v) => v === undefined ? '<i>(line missing)</i>' : esc(v) || '<i>(blank)</i>';
  const near = nearMiss(t.out, t.got);
  return `<div class="diff">
    <div class="dh">first difference — line ${d.line}, token ${d.token}${lineNote}</div>
    <div class="drow"><span class="k">exp</span><span class="v">${cell(d.exp)}</span></div>
    <div class="drow got"><span class="k">got</span><span class="v">${cell(d.got)}</span></div>
    ${near ? `<div class="near">${esc(near)}</div>` : ''}
  </div>`;
}

// ------------------------------------------------- contest clock (live/virtual)
// Driven by the judge's own schedule, imported with the contest.
function contestClock() {
  const s = S.session;
  if (!s || !s.durationSec) return null;
  const now = Math.floor(Date.now() / 1000);
  const start = s.startSec || 0;
  if (start && now < start) return { state: 'before', secs: start - now };
  const left = start + s.durationSec - now;
  return left > 0 ? { state: 'running', secs: left } : { state: 'over', secs: 0 };
}
function fmtHMS(t) {
  t = Math.max(0, t | 0);
  const h = Math.floor(t / 3600), m = Math.floor((t % 3600) / 60), sec = t % 60;
  return (h ? h + ':' : '') + String(m).padStart(2, '0') + ':' + String(sec).padStart(2, '0');
}
function clockText(c) {
  return c.state === 'before' ? `starts in ${fmtHMS(c.secs)}` : c.state === 'over' ? 'round ended' : `${fmtHMS(c.secs)} left`;
}
function contestClockHtml() {
  const c = contestClock();
  if (!c) return '';
  const cls = c.state === 'over' ? ' over' : c.state === 'running' && c.secs <= 15 * 60 ? ' warn' : '';
  return `<span class="cclock${cls}" id="cclock" title="From the judge's own schedule for this round">${esc(clockText(c))}</span>`;
}

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
  const tabs = problems().map((pr) => `<button class="tab${pr.id === S.active ? ' active' : ''}" data-act="tab" data-arg="${esc(pr.id)}" title="${esc(pr.id)}. ${esc(pr.title)}">${esc(pr.id)}${pr.solved || pr.attempted ? `<span class="dot" style="background:${pr.solved ? 'var(--ok)' : 'var(--bad)'}"></span>` : ''}<span class="tabx" data-act="delProblem" data-arg="${esc(pr.id)}" title="Remove this problem from the session">✕</span></button>`).join('');
  // Only the most recent handful are listed; the rest are one click away rather
  // than a long scroll. An armed delete always stays on screen, or confirming it
  // would mean hunting for a row that just moved.
  const shownContests = S.allContests ? S.contests : S.contests.slice(0, MENU_CONTESTS);
  if (S.confirmDelete && !shownContests.some((c) => c.dir === S.confirmDelete)) {
    const armed = S.contests.find((c) => c.dir === S.confirmDelete);
    if (armed) shownContests.push(armed);
  }
  const hiddenContests = S.contests.length - shownContests.length;
  // Deleting a contest removes the folder and every solution in it, so it takes two
  // clicks: the ✕ arms the row, "Delete" confirms.
  const contests = shownContests.map((c) => `<div class="contest-row${S.confirmDelete === c.dir ? ' arming' : ''}" data-act="openContest" data-arg="${esc(c.dir)}">
      <div style="flex:1;min-width:0"><div class="name" style="font-weight:${c.active ? 600 : 400}">${esc(c.name)}</div><div class="meta">${esc(c.date)} · ${esc(c.meta)}</div></div>
      ${S.confirmDelete === c.dir
        ? `<span class="warn">Delete the folder?</span><button class="btn-sm danger fit" data-act="deleteContest" data-arg="${esc(c.dir)}" data-name="${esc(c.name)}">Delete</button><button class="btn-sm fit" data-act="cancelDelete">Keep</button>`
        : `<button class="xbtn" data-act="askDelete" data-arg="${esc(c.dir)}" title="Delete contest folder${c.active ? ' (it is open now)' : ''}">✕</button>`}
    </div>`).join('');
  return `<div class="topbar">
    <div class="logo clickable" data-act="home" title="Home — contests, new session, setup"><img src="${logo}" alt="cp logo"></div>
    <div class="vsep"></div>
    <div class="rel">
      <button class="contest-btn" data-act="contestMenu">${esc(contestName)} <span class="caret">▾</span></button>
      ${S.contestMenuOpen ? `<div class="menu contest-menu" data-stop="1">
        <div class="head"><div class="lbl">Saved contests</div><div class="grow"></div><button class="btn-outline-accent" data-act="newSession">+ New session</button></div>
        <div class="clist">${contests || '<div class="empty" style="padding:12px 14px">No saved contests yet.</div>'}</div>
        ${hiddenContests > 0
          ? `<button class="more" data-act="showAllContests">Show ${hiddenContests} older contest${hiddenContests === 1 ? '' : 's'}…</button>`
          : S.allContests && S.contests.length > MENU_CONTESTS
            ? '<button class="more" data-act="showFewerContests">Show fewer</button>' : ''}
        <div class="foot" title="${esc(S.root)}\\contests">Stored on disk in cp/contests/ — deleting removes the folder. <a href="#" data-act="home">Home</a> · <a href="#" data-act="setupOpen">Setup…</a></div>
      </div>` : ''}
    </div>
    ${contestClockHtml()}
    <div class="tabs"><div class="tabscroll">${tabs}</div>
      <div class="rel"><button class="tab-add" data-act="importMenu" title="Import a problem: Competitive Companion (localhost:10045) or paste a URL">+</button>
      ${S.importOpen ? `<div class="menu import-menu" data-stop="1">
        <div class="hint">Listening for <b>Competitive Companion</b> on localhost:10045 — click a problem or contest in your browser and it appears here. Or paste a URL: a single problem, or a whole contest to join a live or virtual round.</div>
        <div class="row"><input class="inp" data-in="importUrl" value="${esc(S.importUrl)}" placeholder="https://codeforces.com/contest/2009"><button class="btn-sm acc fit" data-act="importUrl">Import</button></div>
      </div>` : ''}</div>
    </div>
    <div class="grow"></div>
    ${S.running
      ? `<button class="btn-run stop" data-act="stopRun" title="Stop the run (Ctrl+.)">${ICON_STOP}Stop</button>`
      : `<button class="btn-run" data-act="run" title="Run all tests (Ctrl+Enter)">${ICON_PLAY}Run</button>`}
    <button class="btn-submit" data-act="submit" title="Submit to the judge (Ctrl+Shift+Enter)">${S.judging ? '<span class="spin white"></span>' : ''}${S.judging ? 'Judging…' : 'Submit'}</button>
    <div class="grow"></div>
    ${timerHtml()}
    <div class="rel">
      <button class="iconbtn" data-act="layoutMenu" title="Layouts">${ICON_LAYOUT}</button>
      ${S.layoutMenuOpen ? layoutMenuHtml() : ''}
    </div>
    <button class="btn-history" data-act="history">History</button>
    <button class="iconbtn" data-act="theme" title="Toggle theme">${light() ? '◐' : '◑'}</button>
    <button class="iconbtn" data-act="shortcuts" title="Keyboard shortcuts (F1)">?</button>
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
      ${card('note', 'Note-taking', box(30) + box(40) + col(30, box(65) + box(35)))}
      ${card('debug', 'Debug', box(30) + box(40) + col(30, box(7) + box(3)))}
    </div>
    <div class="hr"></div>
    <button class="btn-focus${S.focus ? ' on' : ''}" data-act="focus">${S.focus ? 'Exit Focus Mode (Esc)' : '⌖ Focus Mode'}</button>
  </div>`;
}

// Rendered-statement cache. Keyed by contest as well as problem letter: every
// contest has an "A", so keying on the letter alone served the statement of the
// contest you were in before — you would sit down to solve the wrong problem.
const stmtCache = {};
function stmtKey(id) { return `${S.session ? S.session.dir : ''}::${id}`; }
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
    return `<div class="stmt cf" data-stmt="${esc(p.id)}">${stmtCache[stmtKey(p.id)] || sanitizeHtml(p.statementHtml)}</div>`;
  }
  if (p.statementHtml) {
    body = `<div class="stmt" data-stmt="${esc(p.id)}">${stmtCache[stmtKey(p.id)] || sanitizeHtml(p.statementHtml)}</div>`;
  } else {
    const err = S.stmtErrors[p.id];
    body = `<p style="margin:0 0 12px">${err ? `Statement could not be fetched (${esc(err)}).` : 'Fetching the statement…'} ${p.url ? `<a href="#" data-act="openUrl" data-arg="${esc(p.url)}">Open on ${esc(p.judge)}</a> · <a href="#" data-act="refetch">Retry</a>` : ''}</p>
      <div class="figbox">Statement text and images render here once the problem page has been fetched — samples below come from Competitive Companion</div>`;
  }
  // AtCoder and Codeforces statements carry their own sample blocks (with the
  // explanations and figures attached); repeating them below would be noise.
  const ex = (p.statementSamples && p.statementHtml ? [] : samples).map((s, i) => focusStyle
    ? `<div class="fex"><div class="lbl">Example ${i + 1}</div><div class="blk"><div class="l">Input:</div><pre>${esc(s.in)}</pre><div class="l">Output:</div><pre>${esc(s.out)}</pre></div></div>`
    : `<div class="example"><div class="lbl">Example ${i + 1}</div><div class="exbox"><div class="h">Input</div><pre>${esc(s.in)}</pre><div class="h out">Output</div><pre>${esc(s.out)}</pre></div></div>`).join('');
  return body + ex;
}

function chipsHtml(p) {
  return `<span class="chip mono">${p.rating ? '*' + p.rating : '*—'}</span><span class="chip">${esc(p.tl)} · ${esc(p.ml)}</span>${p.url ? `<span class="chip link" data-act="openUrl" data-arg="${esc(p.url)}" title="${esc(p.url)}">${esc(p.judge)} ↗</span>` : ''}${p.empty ? '' : `<span class="chip link" data-act="openFolder" title="Open the problem folder">folder ↗</span>`}${p.url && !p.empty ? `<span class="chip link" data-act="refetch" title="Fetch the statement from the judge again">refresh ↻</span>` : ''}`;
}

function subsHtml(p) {
  const subs = S.history.filter((h) => h.prob === p.id && (!S.session || h.contest === S.session.name));
  const judgeNames = { codeforces: 'Codeforces', atcoder: 'AtCoder', cses: 'CSES', usaco: 'USACO' };
  if (!subs.length) return `<div class="empty">No submissions for ${esc(p.id)} yet — hit Submit.${judgeNames[p.judge] ? ` <a href="#" data-act="judgeLogin" data-arg="${p.judge}">${judgeNames[p.judge]} account…</a>` : ''}</div>`;
  return `<div class="subs">${subs.map((h) => `<div class="subrow"><span class="v ${h.ok ? 'ok' : 'bad'}">${esc(h.verdict)}</span><div class="grow"></div><span class="l">${esc(h.lang)}</span><span class="t">${esc(h.at)}</span></div>`).join('')}</div>`;
}

function problemPaneHtml() {
  const p = prob();
  const w = S.paneW ? `${S.paneW}px;max-width:none;flex:0 0 ${S.paneW}px`
    : S.layout === 'default' ? '45%' : '30%';
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

function taRows(s) { return Math.min(14, Math.max(3, String(s || '').split('\n').length)); }

// ------------------------------------------------- test panel (case / result)
// One tab per case with the selected one shown in full, rather than a row of
// cards you scroll sideways through — a Codeforces sample can be a dozen cases.
function selCase() {
  const T = tests();
  if (!T.length) return -1;
  return Math.min(Math.max(0, S.selCase | 0), T.length - 1);
}
function caseMark(t) {
  return t.status === 'pass' ? '<span class="mk ok">✓</span>'
    : statusClass(t) === 'fail' ? '<span class="mk bad">✕</span>'
    : t.status === 'out' ? '<span class="mk out">•</span>'
    : t.status === 'running' ? '<span class="spin sm"></span>' : '';
}
function caseTabsHtml(withAdd) {
  const sel = selCase();
  const tabs = tests().map((t, i) => `<button class="ctab${sel === i ? ' on' : ''}" data-act="selCase" data-arg="${i}">${caseMark(t)}${t.custom ? 'Custom' : 'Case'} ${i + 1}${t.custom ? `<span class="x" data-act="delTest" data-arg="${i}" title="Delete this case">✕</span>` : ''}</button>`).join('');
  return `<div class="ctabs">${tabs}${withAdd ? '<button class="ctab add" data-act="addTest" title="Add a case (Ctrl+Shift+T)">+</button>' : ''}</div>`;
}
function boxHtml(label, body, cls) { return `<div class="fld"><div class="fl">${esc(label)}</div><div class="fbox${cls ? ' ' + cls : ''}">${body}</div></div>`; }

// Testcase tab: edit the selected case.
function caseEditHtml() {
  const i = selCase();
  const t = tests()[i];
  if (!t) return '<div class="empty" style="padding:14px 16px">No test cases yet — press <b>+</b> to add one.</div>';
  return `<div class="cbody">
    ${caseTabsHtml(true)}
    <div class="fld"><div class="fl">Input<div class="grow"></div><button class="xbtn" data-act="copyTest" data-arg="${i}" title="Copy input">⧉</button></div>
      <textarea class="fta" rows="${taRows(t.in)}" data-tin="${i}" spellcheck="false" placeholder="stdin for this case">${esc(t.in)}</textarea></div>
    <div class="fld"><div class="fl">Expected</div>
      <textarea class="fta" rows="${taRows(t.out)}" data-tout="${i}" spellcheck="false" placeholder="leave empty to just see the output">${esc(t.out)}</textarea></div>
  </div>`;
}

// A bare exit code tells you nothing; these are the ones a solution actually hits.
const EXIT_MEANING = {
  3221225477: 'access violation — read or wrote memory it does not own (out-of-bounds index, bad pointer)',
  3221225725: 'stack overflow — recursion too deep, or a huge array declared inside a function',
  3221225620: 'integer division by zero',
  3221225612: 'integer overflow trap',
  3221226356: 'heap corruption — wrote past the end of an allocation',
  139: 'segmentation fault — out-of-bounds index or bad pointer',
  138: 'bus error',
  136: 'floating point exception — division by zero',
  134: 'aborted — a failed assert, or an uncaught C++ exception',
  137: 'killed — out of memory',
};

// Pulls the line that actually says what went wrong out of a failed run's output:
// the exception from a Python traceback, whatever came out on stderr, or the
// meaning of the exit code when the program died without saying anything.
function errorText(t) {
  const raw = String((t && t.got) || '').replace(/\r/g, '');
  const all = raw.split('\n').map((l) => l.trimEnd()).filter((l) => l.trim());
  const exit = raw.match(/\(exit code (-?\d+)\)/);
  const body = all.filter((l) => !/^\(exit code -?\d+\)$/.test(l.trim()));
  let msg = '';
  const thrown = body.find((l) => /^terminate called after throwing an instance of/.test(l));
  if (body.some((l) => /^Traceback \(most recent call last\)/.test(l))) msg = body[body.length - 1];
  else if (thrown) {
    // Two lines of C++ noise carry one useful fact each: the type and the message.
    const type = (thrown.match(/'([^']+)'/) || [, ''])[1];
    const what = (body.find((l) => /^\s*what\(\):/.test(l)) || '').replace(/^\s*what\(\):\s*/, '').trim();
    msg = type && what ? `${type}: ${what}` : (type || what || thrown);
  } else {
    const i = body.findIndex((l) => l.startsWith('[stderr]'));
    if (i >= 0) msg = body.slice(i).join(' ').replace('[stderr]', '').trim();
    else if (body.length && !/^\(no output\)$/.test(body[0])) msg = body[body.length - 1];
  }
  if (exit) {
    const code = parseInt(exit[1]);
    const known = EXIT_MEANING[code >>> 0] || EXIT_MEANING[code];
    if (known) msg = msg ? `${msg}  ·  ${known}` : known;
    else if (!msg) msg = `exited with code ${code}`;
  }
  return msg;
}

// Test Result tab: the verdict, then the selected case's input / output / expected.
function resultBodyHtml() {
  const sm = tSummary();
  const i = selCase();
  const t = tests()[i];
  if (!sm.anyRun) return `<div class="cbody"><div class="empty" style="padding:14px 4px">Run your code to see results here.</div></div>`;
  const v = S.compileError ? { text: 'Compilation error', cls: 'bad' }
    : !t ? { text: '—', cls: '' }
    : t.status === 'pass' ? { text: 'Accepted', cls: 'ok' }
    : t.status === 'tle' ? { text: 'Time limit exceeded', cls: 'bad' }
    : t.status === 're' ? { text: 'Runtime error', cls: 'bad' }
    : t.status === 'fail' ? { text: 'Wrong answer', cls: 'bad' }
    : t.status === 'out' ? { text: 'Ran — nothing to compare', cls: 'out' }
    : { text: 'Running…', cls: '' };
  const ms = t && t.ms ? `<span class="rt">Runtime: ${t.ms} ms</span>` : '';
  // "Runtime error" on its own is not a diagnosis — say what the program did.
  const why = S.compileError ? String(S.compileError).replace(/\r/g, '').split('\n').filter((l) => l.trim())[0] || ''
    : t && t.status === 're' ? errorText(t)
    : t && t.status === 'tle' ? `did not finish within the ${prob().tl || 'time'} limit`
    : '';
  return `<div class="cbody">
    <div class="verdict ${v.cls}">${esc(v.text)}${ms}</div>
    ${why ? `<div class="verr">${esc(why)}</div>` : ''}
    ${caseTabsHtml(false)}
    ${t ? boxHtml('Input', `<pre>${esc(t.in) || '<i>(empty)</i>'}</pre>`) : ''}
    ${t && t.got ? boxHtml(t.status === 'out' ? 'Output' : 'Your output', `<pre>${esc(t.got)}</pre>`, statusClass(t) === 'fail' ? 'bad' : '') : ''}
    ${t && (t.out || '').trim() ? boxHtml('Expected', `<pre>${esc(t.out)}</pre>`) : ''}
    ${t ? diffHtml(t) : ''}
  </div>`;
}


function editorPaneHtml() {
  const p = prob();
  const isDebug = S.layout === 'debug';
  return `<div class="editor-pane">
    <div class="editor-head">
      <span class="code-glyph">&lt;/&gt;</span><span class="pane-title">Code</span>
      ${langSelectHtml()}
      ${p.interactive ? '<span class="warn-chip" title="Interactive problem: sample runs talk to a grader, so a local run will usually hang or fail">interactive</span>' : ''}
      <div class="grow"></div>
      <button class="sqbtn mono" data-act="format" title="Format code">{}</button>
      <button class="sqbtn" data-act="copyCode" title="Copy the whole solution (Ctrl+Alt+C)">⧉</button>
      <button class="sqbtn" data-act="reset" title="Reset to your template">↺</button>
      ${isDebug ? `<button class="btn-hd dbg${S.debugging ? ' on' : ''}" data-act="debugStart">${S.debugStarting ? '<span class="spin sm"></span>' : ''}Debug ▶</button>` : ''}
      <button class="btn-hd" data-act="stress">Stress</button>
    </div>
    <div class="editor-slot" id="editor-slot"></div>
    ${S.compileError ? `<div class="compile-error"><div class="h">Compile error<div class="grow"></div><button class="xbtn" data-act="closeCompile">✕</button></div><pre>${esc(S.compileError)}</pre></div>` : ''}
  </div>`;
}

function testsPaneHtml() {
  const sm = tSummary();
  const h = S.testsCollapsed ? '40px' : S.testsH ? `${S.testsH}px` : '25%';
  return `<div class="tests-pane" style="height:${h};flex:0 0 ${h}">
    <div class="tests-head">
      <button class="ttab${S.tcTab === 'case' ? ' on' : ''}" data-act="tcTab" data-arg="case"><span class="g">☑</span>Testcase</button>
      <span class="tsep">|</span>
      <button class="ttab${S.tcTab === 'result' ? ' on' : ''}" data-act="tcTab" data-arg="result"><span class="g">&gt;_</span>Test Result</button>
      <div class="summary" style="color:${sm.color}">${sm.text}</div>
      <div class="grow" data-act="testsToggle"></div>
      <button class="iconbtn sm" data-act="testsToggle" title="${S.testsCollapsed ? 'Expand' : 'Collapse'} the panel (Ctrl+B)">${S.testsCollapsed ? '⌃' : '⌄'}</button>
    </div>
    ${S.testsCollapsed ? '' : `<div class="tests-body">${S.tcTab === 'result' ? resultBodyHtml() : caseEditHtml()}</div>`}
  </div>`;
}

function rightColHtml() {
  const p = prob();
  const sm = tSummary();
  const lay = S.layout;
  const w = S.rightW ? `${S.rightW}px` : '30%';
  // Split between the two stacked panes, as a percentage the drag handle updates.
  const top = S.rightTop || (lay === 'note' ? 65 : 70);
  const bot = 100 - top;
  const hsplit = '<div class="split h" data-split="right" title="Drag to resize · double-click to reset"></div>';
  let inner = '';
  if (lay === 'note') {
    inner = `<div class="pane" style="flex:${top}"><div class="pane-head" style="height:38px;flex:0 0 38px;gap:8px"><span class="muted" style="font-size:12px">✎</span><div class="pane-title">Notes — ${esc(p.id)}</div></div>
      <textarea class="notes" data-in="notes" placeholder="Observations, edge cases, complexity…"${p.empty ? ' disabled' : ''}>${esc(p.notes || '')}</textarea></div>
      ${hsplit}
      <div class="pane" style="flex:${bot}"><div class="pane-head" style="height:38px;flex:0 0 38px"><div class="pane-title">Testcase · Result</div><div class="summary" style="color:${sm.color}">${sm.text}</div></div>
      <div class="mini-list">${resultBodyHtml()}</div></div>`;
  } else if (lay === 'debug') {
    const bps = p.bps || [];
    inner = `<div class="pane" style="flex:${top}"><div class="pane-head" style="height:38px;flex:0 0 38px;padding:0 12px"><div class="pane-title">Debugger</div><div class="grow"></div>
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
      ${hsplit}
      <div class="pane" style="flex:${bot}"><div class="console-head">Console</div><pre class="console" id="console">${esc(S.consoleText || (S.debugging ? '' : (tSummary().anyRun ? (tSummary().failed ? 'process exited · wrong answer on a case' : 'process exited with code 0') : 'Run or Debug to see output.')))}</pre></div>`;
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
  const sel = T[S.selCase] || T[0] || { in: '', out: '', got: '' };
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
        ${S.running ? `<button class="btn-run stop" data-act="stopRun">${ICON_STOP}Stop</button>` : `<button class="btn-run" data-act="run">${ICON_PLAY}Run</button>`}
        <button class="btn-submit" data-act="submit">${S.judging ? '<span class="spin white"></span>' : ''}${S.judging ? 'Judging…' : 'Submit'}</button></div>
      ${S.compileError ? `<div class="compile-error"><div class="h">Compile error<div class="grow"></div><button class="xbtn" data-act="closeCompile">✕</button></div><pre>${esc(S.compileError)}</pre></div>` : ''}
    </div>
    <div class="fcard tests">
      <div class="fh t"><div class="ttl">Testcase</div><div class="summary" style="color:${sm.color}">${sm.text}</div><div class="grow"></div><button class="btn-add" data-act="addTest">+ Add test</button></div>
      <div class="fcases">
        ${T.map((t, i) => `<div class="fcase${S.selCase === i ? ' on' : ''}" data-act="selCase" data-arg="${i}"><span class="n">${i + 1}</span><span class="pv">${esc((t.in || '(empty)').replace(/\n/g, ' ⏎ '))}</span><span class="st ${statusClass(t)}" style="color:${statusClass(t) === 'pass' ? 'var(--ok)' : statusClass(t) === 'fail' ? 'var(--bad)' : 'var(--muted)'}">${statusText(t)}</span></div>`).join('')}
        <div class="fdetail">
          <div class="lb">Input</div><pre>${esc(sel.in)}</pre>
          <div class="lb">Expected</div><pre style="margin-bottom:0">${esc(sel.out)}</pre>
          ${showGot(sel) ? `<div class="lb ${sel.status === 'out' ? '' : 'bad'}">${sel.status === 'out' ? 'Output' : 'Got'}${sel.ms ? ` · ${sel.ms} ms` : ''}</div><pre class="got">${esc(sel.got)}</pre>${diffHtml(sel)}` : ''}
        </div>
      </div>
    </div>
  </div></div>`;
}

// Landing screen: shown when no contest is open, and reachable any time from the
// logo or the contest menu. Everything you can start from, in one place.
function homeHtml() {
  const logo = light() ? 'assets/logo.png' : 'assets/logo-white.png';
  const cur = S.session;
  const open = cur ? S.contests.find((c) => c.dir === cur.dir) : null;
  const solved = cur ? cur.problems.filter((p) => p.solved).length : 0;
  const others = S.contests.filter((c) => !cur || c.dir !== cur.dir);
  const recents = S.allContests ? others : others.slice(0, HOME_CONTESTS);
  const hiddenRecents = others.length - recents.length;
  // Only what is actually available is listed; a language you do not use is not a
  // problem to be flagged here — Run says so if and when you try to use it.
  const found = S.tools.filter((t) => t.id !== 'gdb' && t.found);
  return `<div class="home"><div class="home-col">
    <div class="home-head"><img src="${logo}" alt="">
      <div><div class="home-title">CP IDE</div>
      <div class="home-sub">${cur ? 'Pick up where you left off, or start something new.' : 'No contest open yet — import a problem or start a session.'}</div></div>
    </div>

    ${cur ? `<button class="resume" data-act="homeClose">
      <span class="play">▶</span>
      <div style="flex:1;min-width:0"><div class="nm">${esc(cur.name)}</div>
      <div class="mt">${cur.problems.length} problem${cur.problems.length === 1 ? '' : 's'} · ${solved} solved${open ? ` · ${esc(open.date)}` : ''}</div></div>
      <span class="mt">Resume <kbd>Esc</kbd></span></button>` : ''}

    ${recents.length ? `<div class="home-sec"><div class="sh">Recent contests</div>
      ${recents.map((c) => `<div class="hrow2${S.confirmDelete === c.dir ? ' arming' : ''}" data-act="openContest" data-arg="${esc(c.dir)}">
        <span class="nm">${esc(c.name)}</span>
        ${S.confirmDelete === c.dir
          ? `<span class="warn">Delete the folder?</span><button class="btn-sm danger fit" data-act="deleteContest" data-arg="${esc(c.dir)}" data-name="${esc(c.name)}">Delete</button><button class="btn-sm fit" data-act="cancelDelete">Keep</button>`
          : `<span class="mt">${esc(c.date)} · ${esc(c.meta)}</span><button class="xbtn" data-act="askDelete" data-arg="${esc(c.dir)}" title="Delete this contest folder">✕</button>`}
      </div>`).join('')}
      ${hiddenRecents > 0
        ? `<button class="home-more" data-act="showAllContests">Show ${hiddenRecents} older contest${hiddenRecents === 1 ? '' : 's'}…</button>`
        : S.allContests && others.length > HOME_CONTESTS
          ? '<button class="home-more" data-act="showFewerContests">Show fewer</button>' : ''}</div>` : ''}

    <div class="home-sec"><div class="sh">Start</div>
      <div class="home-start">
        <button class="btn-big" data-act="newSession">+ New session</button>
        <input class="inp" data-in="importUrl" value="${esc(S.importUrl)}" placeholder="…or paste a problem or contest URL and press Enter">
        <button class="btn-sm acc fit" data-act="importUrl">Import</button>
      </div>
    </div>

    <div class="home-foot">
      <div><span class="st ok">●</span> Listening for Competitive Companion on <code>localhost:${S.port}</code></div>
      <div>${found.length
        ? `<span class="st ok">✓</span> ${found.map((t) => esc(t.label)).join(', ')} ready — <a href="#" data-act="setupOpen">Setup</a>`
        : `Set up a compiler to run your solutions — <a href="#" data-act="setupOpen">open Setup</a>`}</div>
      <div class="keyhint">Press <code>F1</code> for keyboard shortcuts.</div>
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
      <div class="setup-row"><span class="st ${Object.values(S.templates || {}).some((v) => (v || '').trim()) ? 'ok' : 'wait'}">${Object.values(S.templates || {}).some((v) => (v || '').trim()) ? '✓' : '·'}</span><span class="lbl">Code templates</span>
        <span class="val body">the boilerplate every new problem starts from — ${LANGS.filter(([v]) => (S.templates[v] || '').trim()).map(([, l]) => esc(l)).join(', ') || 'none set yet'}</span>
        <button class="btn-sm fit" data-act="tplOpen">Edit…</button></div>
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
    <div class="mh"><div class="mt">New session</div><div class="grow"></div><button class="xbtn lg" data-act="sessionClose">✕</button></div>
    <input class="inp body full" data-in="sessionName" value="${esc(S.sessionName)}" placeholder="Name it — e.g. Div 2 Round 1000, or DP practice (optional)">
    <div class="seg wide"><button class="${m === 'blank' ? 'on' : ''}" data-act="sessionMode" data-arg="blank">Blank</button><button class="${m === 'random' ? 'on' : ''}" data-act="sessionMode" data-arg="random">Random by rating</button><button class="${m === 'url' ? 'on' : ''}" data-act="sessionMode" data-arg="url">From URL</button></div>
    ${m === 'blank' ? '<div class="note">Starts empty — add problems by clicking them in your browser (Companion) or pasting URLs.</div>' : ''}
    ${m === 'random' ? `<div class="rating"><input class="inp" data-in="ratingMin" value="${esc(S.ratingMin)}"><span class="dash">–</span><input class="inp" data-in="ratingMax" value="${esc(S.ratingMax)}"><span class="lbl">3 unsolved CF problems in this range${S.config.cfHandle ? ` (for ${esc(S.config.cfHandle)})` : ''}</span></div>` : ''}
    ${m === 'url' ? `<input class="inp full mt10" data-in="sessionUrl" value="${esc(S.sessionUrl)}" placeholder="https://codeforces.com/contest/2009">` : ''}
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

// render() rebuilds the whole page, so anything the user had scrolled or was
// typing into has to be put back afterwards — otherwise a test finishing mid-run
// throws the statement back to the top and steals the caret out of a test box.
const SCROLLERS = ['.pbody', '.focus-scroll', '.tests-body', '.mini-list', '.dbg-body', '.hb', '.console', '.setup', '.tpl-list'];
function captureUi() {
  const scroll = {};
  for (const sel of SCROLLERS) {
    const el = $(sel);
    if (el) scroll[sel] = [el.scrollTop, el.scrollLeft];
  }
  let focus = null;
  const a = document.activeElement;
  if (a && a.getAttribute && a.closest && a.closest('#app')) {
    const sel = a.hasAttribute('data-in') ? `[data-in="${a.getAttribute('data-in')}"]`
      : a.hasAttribute('data-tin') ? `[data-tin="${a.getAttribute('data-tin')}"]`
      : a.hasAttribute('data-tout') ? `[data-tout="${a.getAttribute('data-tout')}"]` : null;
    if (sel) focus = { sel, start: a.selectionStart, end: a.selectionEnd, top: a.scrollTop, atEnd: a.selectionStart === (a.value || '').length };
  }
  // The editor host is moved out of the page and back on every render, which
  // blurs it — so remember that the caret was in the code and put it back.
  const editorFocus = !focus && monacoReady && editor.hasTextFocus();
  return { scroll, focus, editorFocus };
}
function restoreUi(st) {
  for (const sel of SCROLLERS) {
    const el = $(sel);
    if (el && st.scroll[sel]) { el.scrollTop = st.scroll[sel][0]; el.scrollLeft = st.scroll[sel][1]; }
  }
  if (!st.focus) return;
  const el = $(st.focus.sel);
  if (!el) return;
  el.focus();
  try {
    // A field the user has not moved the caret in (search boxes we re-render from
    // state) keeps sitting at the end; otherwise the exact selection is restored.
    if (st.focus.atEnd) el.selectionStart = el.selectionEnd = (el.value || '').length;
    else { el.selectionStart = st.focus.start; el.selectionEnd = st.focus.end; }
    el.scrollTop = st.focus.top;
  } catch (e) { /* not a text field */ }
}

const SHORTCUTS = [
  ['Ctrl + Enter', 'Run all tests'],
  ['Ctrl + Shift + Enter', 'Submit to the judge'],
  ['Ctrl + .', 'Stop the run'],
  ['Alt + ← / →', 'Previous / next problem'],
  ['Alt + 1 … 9', 'Jump to problem 1–9'],
  ['Ctrl + Alt + C', 'Copy the whole solution'],
  ['Ctrl + Shift + T', 'Add a test case'],
  ['Ctrl + B', 'Show / hide the test panel'],
  ['Ctrl + + / − / 0', 'Editor font size'],
  ['Ctrl + Shift + F', 'Focus mode'],
  ['Ctrl + S', 'Everything is already saved'],
  ['Click a line number', 'Toggle a breakpoint'],
  ['F1', 'This list'],
  ['Esc', 'Close a menu / leave Focus mode'],
];

function shortcutsModalHtml() {
  return `<div class="overlay" data-act="shortcutsClose"><div class="modal keys" data-stop="1">
    <div class="mh"><div class="mt">Keyboard shortcuts</div><div class="grow"></div><button class="xbtn lg" data-act="shortcutsClose">✕</button></div>
    <div class="keylist">${SHORTCUTS.map(([k, d]) => `<div class="keyrow"><span class="kk">${esc(k)}</span><span class="kd">${esc(d)}</span></div>`).join('')}</div>
  </div></div>`;
}

function templateModalHtml() {
  const lg = S.tplLang;
  const name = (LANGS.find(([v]) => v === lg) || [, lg])[1];
  return `<div class="overlay" data-act="tplClose"><div class="modal tpl" data-stop="1">
    <div class="mh"><div class="mt">Code templates</div><div class="grow"></div><button class="xbtn lg" data-act="tplClose">✕</button></div>
    <div class="desc">Every new problem starts from this file. Saved in <code>${esc(S.root)}\\templates</code>; existing problems are untouched — press ↺ above the editor to pull it in.</div>
    <div class="seg wide">${LANGS.map(([v, l]) => `<button class="${lg === v ? 'on' : ''}" data-act="tplLang" data-arg="${v}">${esc(l)}</button>`).join('')}</div>
    <textarea class="tpl-text" data-in="tplText" spellcheck="false" placeholder="Empty — new ${esc(name)} files start blank.">${esc(S.tplText)}</textarea>
    <div class="row" style="margin-top:12px">
      <button class="btn-sm fit" data-act="tplStarter">Insert a starter template</button>
      <button class="btn-sm danger fit" data-act="tplClear">Clear</button>
      <div class="grow"></div>
      <button class="btn-primary" style="margin-top:0" data-act="tplSave">Save</button>
    </div>
  </div></div>`;
}

// ------------------------------------------------------------- context menus
// Right-click targets that have real actions behind them. Anywhere else keeps the
// browser's own menu, so selecting statement text and copying still works, and the
// editor keeps Monaco's.
const JUDGE_NAMES = { codeforces: 'Codeforces', atcoder: 'AtCoder', cses: 'CSES', usaco: 'USACO', hackerrank: 'HackerRank' };
const SEP = { sep: true };

function ctxForProblem(id) {
  const p = problems().find((x) => x.id === id);
  if (!p) return null;
  const items = [
    { label: 'Open this problem', act: 'tab', arg: id },
    { label: 'Run all tests', act: 'ctxRun', arg: id, key: 'Ctrl+Enter' },
    { label: 'Submit', act: 'ctxSubmit', arg: id, key: 'Ctrl+Shift+Enter' },
    SEP,
  ];
  if (p.url) {
    items.push({ label: `Open on ${JUDGE_NAMES[p.judge] || p.judge}`, act: 'openUrl', arg: p.url });
    items.push({ label: 'Copy problem URL', act: 'ctxCopyUrl', arg: id });
    items.push({ label: 'Refresh statement', act: 'ctxRefetch', arg: id });
  }
  items.push({ label: 'Open problem folder', act: 'ctxFolder', arg: id });
  items.push(SEP);
  items.push({ label: p.solved ? 'Mark as unsolved' : 'Mark as solved', act: 'ctxSolved', arg: id });
  items.push({ label: 'Remove problem…', act: 'delProblem', arg: id, danger: true });
  return items;
}

function ctxForTest(i) {
  const t = tests()[i];
  if (!t) return null;
  const items = [
    { label: `Run case ${i + 1}`, act: 'runOne', arg: i },
    SEP,
    { label: 'Copy input', act: 'copyTest', arg: i },
  ];
  if (t.got) items.push({ label: 'Copy output', act: 'copyGot', arg: i });
  if (t.got && t.status === 'out') items.push({ label: 'Use output as expected', act: 'acceptGot', arg: i });
  items.push({ label: 'Duplicate case', act: 'ctxDupTest', arg: i });
  if (t.custom) items.push(SEP, { label: 'Delete case', act: 'delTest', arg: i, danger: true });
  return items;
}

function ctxForContest(dir) {
  const c = S.contests.find((x) => x.dir === dir);
  if (!c) return null;
  const open = S.session && S.session.dir === dir;
  return [
    { label: open ? 'Already open' : 'Open contest', act: open ? 'ctxNoop' : 'openContest', arg: dir },
    { label: 'Open contest folder', act: 'ctxContestFolder', arg: dir },
    SEP,
    { label: 'Delete contest…', act: 'askDelete', arg: dir, danger: true },
  ];
}

function buildCtx(target) {
  if (!target || !target.closest) return null;
  if (target.closest('#editor-host') || target.closest('.ctxmenu')) return null;
  const tab = target.closest('.tab[data-arg]');
  if (tab) return ctxForProblem(tab.getAttribute('data-arg'));
  const card = target.closest('[data-case]');
  if (card) return ctxForTest(parseInt(card.getAttribute('data-case')));
  const fcase = target.closest('.fcase[data-arg]');
  if (fcase) return ctxForTest(parseInt(fcase.getAttribute('data-arg')));
  const row = target.closest('.contest-row[data-arg], .hrow2[data-arg]');
  if (row) return ctxForContest(row.getAttribute('data-arg'));
  return null;
}

function ctxMenuHtml() {
  if (!S.ctx) return '';
  const body = S.ctx.items.map((i) => i.sep
    ? '<div class="ctxsep"></div>'
    : `<button class="ctxitem${i.danger ? ' danger' : ''}" data-act="${i.act}" data-arg="${esc(i.arg == null ? '' : String(i.arg))}">${esc(i.label)}${i.key ? `<span class="k">${esc(i.key)}</span>` : ''}</button>`).join('');
  return `<div class="ctxmenu" data-stop="1" style="left:${S.ctx.x}px;top:${S.ctx.y}px">${body}</div>`;
}

// Keeps the menu inside the window when opened near an edge.
function placeCtx() {
  const el = $('.ctxmenu');
  if (!el) return;
  const r = el.getBoundingClientRect();
  if (r.right > window.innerWidth - 6) el.style.left = Math.max(6, window.innerWidth - r.width - 6) + 'px';
  if (r.bottom > window.innerHeight - 6) el.style.top = Math.max(6, window.innerHeight - r.height - 6) + 'px';
}

document.addEventListener('contextmenu', (e) => {
  const items = buildCtx(e.target);
  if (!items || !items.length) { if (S.ctx) { S.ctx = null; render(); } return; }
  e.preventDefault();
  S.ctx = { x: e.clientX, y: e.clientY, items };
  render();
});

function render() {
  const app = $('#app');
  const host = $('#editor-host');
  if (host && host.parentElement && host.parentElement !== document.body) document.body.appendChild(host);
  const ui = captureUi();
  let html = `<div class="app">${topbarHtml()}`;
  if (S.focus) html += focusHtml();
  else {
    const showTests = S.layout === 'default';
    const rightOpen = S.layout !== 'default';
    html += `<div class="work">${problemPaneHtml()}<div class="split v" data-split="pane" title="Drag to resize · double-click to reset"></div>` +
      `<div class="center-col">${editorPaneHtml()}${showTests && !S.testsCollapsed ? '<div class="split h" data-split="tests" title="Drag to resize · double-click to reset"></div>' : ''}${showTests ? testsPaneHtml() : ''}</div>` +
      `${rightOpen ? '<div class="split v" data-split="rightw" title="Drag to resize · double-click to reset"></div>' + rightColHtml() : ''}` +
      `${S.historyOpen ? historyHtml() : ''}</div>`;
  }
  if (S.homeOpen && !S.setupOpen) html += homeHtml();
  if (S.setupOpen) html += setupHtml();
  if (S.sessionOpen) html += sessionModalHtml();
  if (S.stressOpen) html += stressModalHtml();
  if (S.tplOpen) html += templateModalHtml();
  if (S.shortcutsOpen) html += shortcutsModalHtml();
  html += ctxMenuHtml();
  html += '</div>';
  app.innerHTML = html;
  restoreUi(ui);
  placeCtx();
  mountEditor();
  const modalUp = S.setupOpen || S.sessionOpen || S.stressOpen || S.tplOpen || S.shortcutsOpen;
  if (ui.editorFocus && monacoReady && !modalUp) editor.focus();
  renderMath();
}

function renderMath() {
  const p = prob();
  const el = $('.stmt[data-stmt]');
  if (!el || stmtCache[stmtKey(p.id)] || !window.renderMathInElement) return;
  try {
    // Order matters: auto-render takes the first delimiter that matches at a given
    // position, so the longer ones must come first. Codeforces writes inline maths
    // as $$$x$$$ and display maths as $$$$$$x$$$$$$ — with only the 3-dollar rule,
    // the opening six dollars matched as an empty formula and the real content was
    // left on the page as raw LaTeX.
    renderMathInElement(el, {
      delimiters: [
        { left: '$$$$$$', right: '$$$$$$', display: true },
        { left: '$$$', right: '$$$', display: false },
        { left: '$$', right: '$$', display: true },
        { left: '\\[', right: '\\]', display: true },
        { left: '\\(', right: '\\)', display: false },
      ],
      // Codeforces writes highlights as \color{red}{1}, meaning "colour just this".
      // KaTeX's default follows LaTeX, where \color is a switch that recolours
      // everything to the end of the group — which turns the whole formula red.
      colorIsTextColor: true,
      throwOnError: false,
    });
    stmtCache[stmtKey(p.id)] = el.innerHTML;
  } catch (e) { /* offline: leave raw text */ }
}

// --------------------------------------------------------------- events UI
document.addEventListener('click', (e) => {
  const actEl = e.target.closest('[data-act]');
  const inMenu = e.target.closest('[data-stop]');
  if (!actEl) {
    if (S.ctx) { S.ctx = null; render(); return; }
    if (!inMenu && (S.contestMenuOpen || S.layoutMenuOpen || S.timerMenuOpen || S.importOpen)) { closeMenus(); render(); }
    return;
  }
  const act = actEl.getAttribute('data-act'), arg = actEl.getAttribute('data-arg');
  const p = prob();
  S.ctx = null;  // any click dismisses the context menu; the action below re-renders
  if (['sessionClose', 'stressClose', 'tplClose', 'shortcutsClose'].includes(act) && inMenu && e.target !== actEl) return;  // click inside modal body
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
    case 'openContest': if (S.confirmDelete) { S.confirmDelete = ''; render(); break; } openContest(arg); break;
    case 'askDelete': e.stopPropagation(); S.confirmDelete = arg; render(); break;
    case 'cancelDelete': e.stopPropagation(); S.confirmDelete = ''; render(); break;
    case 'deleteContest': e.stopPropagation(); S.confirmDelete = ''; deleteContest(arg, actEl.getAttribute('data-name')); break;
    case 'tab': setActive(arg); break;
    case 'tcTab': S.tcTab = arg; if (S.testsCollapsed) S.testsCollapsed = false; render(); break;
    case 'delProblem': e.stopPropagation(); deleteProblem(arg); break;
    case 'run': e.stopPropagation(); run(); break;
    case 'runOne': e.stopPropagation(); run(parseInt(arg)); break;
    case 'stopRun': stopRun(); break;
    case 'submit': submit(); break;
    case 'copyCode': copyText(currentCode(), 'Solution'); break;
    case 'copyTest': e.stopPropagation(); copyText((tests()[parseInt(arg)] || {}).in || '', 'Input'); break;
    case 'copyGot': e.stopPropagation(); copyText((tests()[parseInt(arg)] || {}).got || '', 'Output'); break;
    case 'acceptGot': e.stopPropagation(); acceptGot(parseInt(arg)); break;
    // ---- context-menu actions (they may target a problem that is not the open one)
    case 'ctxNoop': render(); break;
    case 'ctxRun': setActive(arg); run(); break;
    case 'ctxSubmit': setActive(arg); submit(); break;
    case 'ctxCopyUrl': { const q = problems().find((x) => x.id === arg); copyText(q ? q.url : '', 'Problem URL'); break; }
    case 'ctxFolder': rpc('openFolder', { id: arg }); break;
    case 'ctxContestFolder': rpc('openFolder', { dir: arg }); break;
    case 'ctxRefetch': { delete S.stmtErrors[arg]; delete stmtCache[stmtKey(arg)]; rpc('refetchStatement', { id: arg }); toast('Fetching the statement again…', 'var(--accent)'); render(); break; }
    case 'ctxSolved': toggleSolved(arg); break;
    case 'ctxDupTest': dupTest(parseInt(arg)); break;
    case 'home': e.preventDefault(); closeMenus(); S.homeOpen = true; render(); break;
    case 'homeClose': S.homeOpen = false; render(); break;
    case 'shortcuts': closeMenus(); S.shortcutsOpen = true; render(); break;
    case 'shortcutsClose': S.shortcutsOpen = false; render(); break;
    case 'showAllContests': e.preventDefault(); S.allContests = true; render(); break;
    case 'showFewerContests': e.preventDefault(); S.allContests = false; render(); break;
    case 'tplOpen': e.preventDefault(); closeMenus(); openTemplates(S.tplLang); break;
    case 'tplClose': saveTemplateIfDirty(); S.tplOpen = false; render(); break;
    case 'tplLang': saveTemplateIfDirty(); openTemplates(arg); break;
    case 'tplStarter': S.tplText = (S.starters && S.starters[S.tplLang]) || ''; S.tplDirty = true; render(); break;
    case 'tplClear': S.tplText = ''; S.tplDirty = true; render(); break;
    case 'tplSave': saveTemplateIfDirty(true); break;
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
    case 'selCase': S.selCase = parseInt(arg); render(); break;
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
    case 'refetch':
      e.preventDefault();
      delete S.stmtErrors[p.id]; delete stmtCache[stmtKey(p.id)];
      rpc('refetchStatement', { id: p.id });
      toast('Fetching the statement again…', 'var(--accent)');
      render();
      break;
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
  if (key === 'tplText') S.tplDirty = true;
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

// ---------------------------------------------------------------- shortcuts
function bumpFont(delta) {
  S.fontSize = delta === 0 ? 14 : Math.min(20, Math.max(12, S.fontSize + delta));
  rpc('saveUi', { fontSize: S.fontSize });
  if (monacoReady) editor.updateOptions({ fontSize: S.fontSize, lineHeight: Math.round(S.fontSize * 1.6) });
  toast(`Editor font ${S.fontSize} px`, 'var(--accent)');
}
function gotoProblem(delta) {
  const list = problems();
  if (!list.length) return;
  let i = list.findIndex((x) => x.id === S.active);
  if (i < 0) i = 0;
  const n = Math.min(list.length - 1, Math.max(0, i + delta));
  if (n !== i) setActive(list[n].id);
}
function gotoIndex(n) { const list = problems(); if (list[n]) setActive(list[n].id); }
function toggleTests() { S.testsCollapsed = !S.testsCollapsed; render(); }
// Esc is also how you dismiss Monaco's suggestion popup; leaving Focus mode at
// the same time would be a nasty surprise.
function editorPopupOpen() {
  return !!document.querySelector('.monaco-editor .suggest-widget.visible, .monaco-editor .parameter-hints-widget.visible, .monaco-editor .find-widget.visible, .monaco-editor .rename-box');
}
function closeTop() {
  if (S.ctx) { S.ctx = null; render(); return true; }
  if (S.shortcutsOpen) { S.shortcutsOpen = false; render(); return true; }
  if (S.homeOpen && S.session) { S.homeOpen = false; render(); return true; }  // no exit when there is nothing behind it
  if (S.tplOpen) { saveTemplateIfDirty(); S.tplOpen = false; render(); return true; }
  if (S.sessionOpen) { S.sessionOpen = false; render(); return true; }
  if (S.stressOpen) { S.stressOpen = false; if (S.stress.state === 'running') rpc('stressStop'); S.stress.state = 'idle'; render(); return true; }
  if (S.confirmDelete) { S.confirmDelete = ''; render(); return true; }
  if (S.contestMenuOpen || S.layoutMenuOpen || S.timerMenuOpen || S.importOpen) { closeMenus(); render(); return true; }
  if (S.focus && !editorPopupOpen()) { exitFocus(); return true; }
  return false;
}

// Each of these is registered with Monaco too (see initMonaco), so it works with
// the caret in the editor as well as anywhere else in the window.
const ACTIONS = {
  run: () => run(),
  submit: () => submit(),
  stop: () => stopRun(),
  prev: () => gotoProblem(-1),
  next: () => gotoProblem(1),
  copyCode: () => copyText(currentCode(), 'Solution'),
  addTest: () => { addTest(); const all = document.querySelectorAll('[data-tin]'); const el = all[all.length - 1]; if (el) { el.focus(); el.scrollIntoView({ block: 'nearest', inline: 'nearest' }); } },
  toggleTests: () => toggleTests(),
  focusMode: () => toggleFocus(),
  help: () => { S.shortcutsOpen = !S.shortcutsOpen; render(); },
  fontUp: () => bumpFont(1),
  fontDown: () => bumpFont(-1),
  fontReset: () => bumpFont(0),
  saved: () => toast('Everything is saved as you type', 'var(--accent)'),
};

document.addEventListener('keydown', (e) => {
  const mod = e.ctrlKey || e.metaKey;
  const k = (e.key || '').toLowerCase();
  if (e.key === 'Escape') { if (closeTop()) e.preventDefault(); return; }
  // A stray F5 / Ctrl+R mid-contest would reload the page and throw away the
  // editor's undo history, so it is refused rather than obeyed.
  if (e.key === 'F5' || (mod && k === 'r')) { e.preventDefault(); toast('Reload is off during a session — nothing to reload, everything is saved', 'var(--muted)'); return; }
  if (e.key === 'F1') { e.preventDefault(); ACTIONS.help(); return; }
  if (mod && e.key === 'Enter') { e.preventDefault(); (e.shiftKey ? ACTIONS.submit : ACTIONS.run)(); return; }
  if (mod && !e.shiftKey && k === '.') { e.preventDefault(); ACTIONS.stop(); return; }
  if (mod && k === 's' && !e.shiftKey && !e.altKey) { e.preventDefault(); ACTIONS.saved(); return; }
  if (mod && e.altKey && k === 'c') { e.preventDefault(); ACTIONS.copyCode(); return; }
  if (mod && e.shiftKey && k === 't') { e.preventDefault(); ACTIONS.addTest(); return; }
  if (mod && e.shiftKey && k === 'f') { e.preventDefault(); ACTIONS.focusMode(); return; }
  if (mod && !e.shiftKey && k === 'b') { e.preventDefault(); ACTIONS.toggleTests(); return; }
  if (mod && (k === '=' || k === '+')) { e.preventDefault(); ACTIONS.fontUp(); return; }
  if (mod && k === '-') { e.preventDefault(); ACTIONS.fontDown(); return; }
  if (mod && k === '0') { e.preventDefault(); ACTIONS.fontReset(); return; }
  if (e.altKey && !mod && e.key === 'ArrowLeft') { e.preventDefault(); ACTIONS.prev(); return; }
  if (e.altKey && !mod && e.key === 'ArrowRight') { e.preventDefault(); ACTIONS.next(); return; }
  if (e.altKey && !mod && /^[1-9]$/.test(e.key)) { e.preventDefault(); gotoIndex(parseInt(e.key) - 1); return; }
  if (e.key === 'Enter' && e.target.matches && e.target.matches('[data-in="importUrl"]')) importUrl();
  if (e.key === 'Enter' && e.target.matches && e.target.matches('[data-in="timerEdit"]')) $('[data-act="timerSet"]') && $('[data-act="timerSet"]').click();
  if (e.key === 'Enter' && e.target.matches && (e.target.matches('[data-in="sessionName"]') || e.target.matches('[data-in="sessionUrl"]'))) createSession();
});

window.addEventListener('resize', () => {
  // A saved pane width from a bigger window must not squeeze everything else out.
  if (S.paneW && S.paneW > window.innerWidth - 460) { S.paneW = 0; render(); }
  if (S.rightW && S.rightW > window.innerWidth - 620) { S.rightW = 0; render(); }
  if (S.testsH && S.testsH > window.innerHeight - 240) { S.testsH = 0; render(); }
  if (monacoReady) editor.layout();
});
window.addEventListener('beforeunload', () => persistTimer(prob()));

// ------------------------------------------------------------ pane splitters
// Dragged sizes are written straight onto the element (no re-render per mouse
// move) and persisted once on release.
let drag = null;
document.addEventListener('mousedown', (e) => {
  const h = e.target.closest && e.target.closest('[data-split]');
  if (!h) return;
  const kind = h.getAttribute('data-split');
  e.preventDefault();
  if (kind === 'right') {
    // Splits the two stacked panes of the right column (Debugger/Console, Notes/Tests).
    const top = h.previousElementSibling, bot = h.nextElementSibling, col = h.parentElement;
    if (!top || !bot || !col) return;
    drag = { kind, top, bot, y: e.clientY, topH: top.getBoundingClientRect().height, colH: col.getBoundingClientRect().height };
    document.body.classList.add('rowres');
    return;
  }
  const pane = kind === 'pane' ? $('.pane.problem') : kind === 'rightw' ? $('.right-col') : $('.tests-pane');
  if (!pane) return;
  const r = pane.getBoundingClientRect();
  drag = { kind, pane, x: e.clientX, y: e.clientY, w: r.width, h: r.height };
  document.body.classList.add(kind === 'tests' ? 'rowres' : 'colres');
});
document.addEventListener('mousemove', (e) => {
  if (!drag) return;
  if (drag.kind === 'pane') {
    S.paneW = Math.round(Math.max(300, Math.min(window.innerWidth - 460, drag.w + e.clientX - drag.x)));
    drag.pane.style.width = S.paneW + 'px';
    drag.pane.style.maxWidth = 'none';
    drag.pane.style.flex = `0 0 ${S.paneW}px`;
  } else if (drag.kind === 'rightw') {
    // The right column sits after the handle, so dragging left makes it wider.
    S.rightW = Math.round(Math.max(240, Math.min(window.innerWidth - 620, drag.w - e.clientX + drag.x)));
    drag.pane.style.width = S.rightW + 'px';
    drag.pane.style.flex = `0 0 ${S.rightW}px`;
  } else if (drag.kind === 'right') {
    const h = Math.max(90, Math.min(drag.colH - 110, drag.topH + e.clientY - drag.y));
    S.rightTop = Math.round(h / drag.colH * 1000) / 10;
    drag.top.style.flex = String(S.rightTop);
    drag.bot.style.flex = String(Math.round((100 - S.rightTop) * 10) / 10);
  } else {
    S.testsH = Math.round(Math.max(110, Math.min(window.innerHeight - 240, drag.h - e.clientY + drag.y)));
    drag.pane.style.height = S.testsH + 'px';
    drag.pane.style.flex = `0 0 ${S.testsH}px`;
  }
  if (monacoReady) editor.layout();
});
function saveSplits() { rpc('saveUi', { paneW: S.paneW, testsH: S.testsH, rightW: S.rightW, rightTop: S.rightTop }); }
document.addEventListener('mouseup', () => {
  if (!drag) return;
  document.body.classList.remove('colres', 'rowres');
  drag = null;
  saveSplits();
  render();
});
document.addEventListener('dblclick', (e) => {
  const h = e.target.closest && e.target.closest('[data-split]');
  if (!h) return;
  const kind = h.getAttribute('data-split');
  if (kind === 'pane') S.paneW = 0;
  else if (kind === 'rightw') S.rightW = 0;
  else if (kind === 'right') S.rightTop = 0;
  else S.testsH = 0;
  saveSplits();
  render();
});

// ---------------------------------------------------------- backend events
window.__cp = {
  event(ev) {
    switch (ev.type) {
      case 'session': S.busy = ''; renderToast(); applySession(ev.session, ev.contests, ev.toast, ev.activate); render(); break;
      case 'problem': {
        const list = problems(); const i = list.findIndex((x) => x.id === ev.problem.id);
        if (i >= 0) {
          const old = list[i];
          // Samples belong to the judge, so a refetch replaces them; the cases you
          // added yourself are yours and are kept and appended.
          const fresh = ev.problem.tests || [];
          const mine = old.tests.filter((t) => t.custom);
          const tests = fresh.length ? fresh.concat(mine) : old.tests;
          const keep = { timeSeconds: old.timeSeconds, paused: old.paused, timerMode: old.timerMode, code: old.code, tests, bps: old.bps, notes: old.notes, lang: old.lang };
          list[i] = Object.assign({}, ev.problem, keep);
          delete stmtCache[stmtKey(ev.problem.id)];
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
        const only = typeof ev.only === 'number' ? ev.only : -1;
        S.running = false; S.runOnly = -1; S.compiling = false;
        const p = problems().find((x) => x.id === ev.id);
        const badIdx = p ? p.tests.findIndex((t) => statusClass(t) === 'fail') : -1;
        if (ev.compileError) { S.compileError = ev.compileError; toast('Compilation failed — see the error panel', 'var(--bad)'); }
        else if (ev.cancelled) toast('Run stopped', 'var(--muted)');
        else if (badIdx >= 0) {
          // No toast: the Test Result panel already shows the verdict, why it
          // failed, the per-case marks and the pass count, so one would only
          // repeat it over the answer it is describing.
          S.selCase = badIdx; S.tcTab = 'result';            // focus mode opens on the case that broke
        }
        else if (ev.ok) toast(`${only >= 0 ? `Case ${only + 1} passed` : `All ${ev.total} test${ev.total === 1 ? '' : 's'} passed`} · ${ev.ms} ms`, 'var(--ok)');
        else if (ev.scratch) toast(`Ran ${ev.scratch} input${ev.scratch === 1 ? '' : 's'} · ${ev.ms} ms — no expected output to check`, 'var(--accent)');
        else toast(`Finished · ${ev.ms} ms`, 'var(--muted)');
        render();
        if (badIdx >= 0) revealCase(badIdx);
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
  S.root = init.root || ''; S.config = init.config || {};
  S.templates = Object.assign({ python: '', cpp: '', java: '', js: '' }, init.templates || {});
  S.starters = init.starters || {};
  S.contests = init.contests || []; S.history = init.history || [];
  const ui = init.ui || {};
  S.theme = ui.theme === 'light' ? 'light' : 'dark';
  S.layout = ['default', 'note', 'debug'].includes(ui.layout) ? ui.layout : 'default';
  S.fontSize = Math.min(20, Math.max(12, ui.fontSize || 14));
  S.paneW = Math.max(0, parseInt(ui.paneW) || 0);
  S.testsH = Math.max(0, parseInt(ui.testsH) || 0);
  S.rightW = Math.max(0, parseInt(ui.rightW) || 0);
  S.rightTop = Math.min(92, Math.max(0, parseFloat(ui.rightTop) || 0));
  document.documentElement.setAttribute('data-theme', S.theme);
  if (init.session) { S.session = init.session; S.active = init.session.active || (init.session.problems[0] ? init.session.problems[0].id : null); }
  S.tools = init.tools || [];
  S.cppCandidates = (init.config && init.config.cppCandidates) || [];
  S.cppFlags = (init.config && init.config.cppFlags) || '-O2 -std=c++23';
  S.port = init.port || 10045;
  S.defaultLang = ['python', 'cpp', 'java', 'js'].includes(ui.defaultLang) ? ui.defaultLang : 'python';
  S.setupOpen = !ui.setupDone;  // first run: the setup page
  S.homeOpen = !S.setupOpen;  // every launch starts at the launcher (Esc / Resume goes on)
  // No nagging about tools at startup — Run and Debug say so when a language you
  // actually use is missing its compiler.
  if (S.setupOpen) setTimeout(judgeStatusAll, 800);
  S.booted = true;
  render();
  initMonaco(() => { render(); hideSplash(); });
  refetchMissingStatements();
}

// The splash goes away once Monaco has mounted; the timer is the safety net so a
// failed editor load can never leave the window stuck behind it.
let splashGone = false;
function hideSplash() {
  if (splashGone) return;
  splashGone = true;
  const el = $('#splash');
  if (!el) return;
  el.classList.add('gone');
  setTimeout(() => el.remove(), 320);
}
setTimeout(hideSplash, 6000);

boot();
