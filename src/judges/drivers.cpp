// Site drivers for JudgeWeb: where the forms are and how to fill them.
#include <regex>
#include "judge_web.hpp"

namespace {

std::string js(const std::string& s) { return json(s).dump(); }

// Picks an <option> of `sel` whose text or value contains one of the keywords (in order).
const char* kPickOption =
    "function pick(sel,keys){if(!sel)return false;var os=sel.options;for(var k=0;k<keys.length;k++){for(var i=0;i<os.length;i++){"
    "var t=(os[i].textContent+' '+os[i].value).toLowerCase();if(t.indexOf(keys[k].toLowerCase())>=0){sel.value=os[i].value;"
    "sel.dispatchEvent(new Event('change',{bubbles:true}));if(window.jQuery){try{jQuery(sel).trigger('change');}catch(e){}}return true;}}}return false;}";
// Puts `src` into the first file input of the form as a virtual file.
const char* kAttachFile =
    "function attach(form,name){var fi=form.querySelector('input[type=\"file\"]');if(!fi)throw 'no file input on the form';"
    "var dt=new DataTransfer();dt.items.add(new File([src],name,{type:'text/plain'}));fi.files=dt.files;"
    "fi.dispatchEvent(new Event('change',{bubbles:true}));}";
const char* kClick =
    "function press(form){var btn=form.querySelector('#submit,button[type=\"submit\"],input[type=\"submit\"]');"
    "window.__cpReport(JSON.stringify({event:'clicked',url:location.href}));if(btn){btn.click();}else{form.submit();}}";

std::string fileName(const std::string& lang) {
  if (lang == "cpp") return "main.cpp";
  if (lang == "java") return "Main.java";
  if (lang == "js") return "main.js";
  return "main.py";
}

SiteDriver codeforces(const SubmitRequest& req, const json& config) {
  SiteDriver d;
  d.id = "codeforces";
  d.name = "Codeforces";
  d.loginUrl = "https://codeforces.com/enter";
  d.homeUrl = "https://codeforces.com/";
  d.loginUrlPart = "/enter";
  d.loggedInJs = "!!document.querySelector('a[href*=\"/logout\"]')";
  d.handleJs = "(function(){var a=document.querySelector('.lang-chooser a[href^=\"/profile/\"]')||document.querySelector('a[href^=\"/profile/\"]');return a?a.textContent.trim():'';})()";
  const auto& p = req.problem;
  if (!p.cfContestId.empty()) {
    std::string base = util::contains(p.url, "/gym/") ? "https://codeforces.com/gym/" : "https://codeforces.com/contest/";
    d.submitUrl = base + p.cfContestId + "/submit/" + p.cfIndex;
  }
  d.formSelector = "form.submit-form, form[action*=\"/submit\"]";
  auto cf = config.value("codeforces", json::object());
  int pt = req.lang == "cpp"    ? cf.value("cppProgramTypeId", 91)
           : req.lang == "java" ? cf.value("javaProgramTypeId", 87)
           : req.lang == "js"   ? cf.value("jsProgramTypeId", 55)
                                : cf.value("pythonProgramTypeId", 31);
  d.fillJs =
      "var idx=form.querySelector('select[name=\"submittedProblemIndex\"]');if(idx){idx.value=" + js(p.cfIndex) + ";}"
      "var pt=form.querySelector('select[name=\"programTypeId\"]');"
      "if(pt){pt.value=" + js(std::to_string(pt)) + ";if(pt.value!==" + js(std::to_string(pt)) + "){throw 'compiler id " + std::to_string(pt) + " is not offered here (change it in cp/config.json)';}}"
      "var ta=form.querySelector('textarea[name=\"source\"]')||document.getElementById('sourceCodeTextarea');if(ta){ta.value=src;}"
      "try{if(window.ace&&document.getElementById('editor')){ace.edit('editor').setValue(src,-1);}}catch(e){}"
      "var tab=form.querySelector('select[name=\"tabSize\"]');if(tab){tab.value='4';}"
      "var btn=form.querySelector('input[type=\"submit\"],button[type=\"submit\"]');"
      "if(btn){try{btn.scrollIntoView({block:'center'});}catch(e){}}"
      // Wait for Cloudflare's Turnstile token, then press; hand over if it needs a human.
      "var tries=0;var iv=setInterval(function(){tries++;"
      "var tok=form.querySelector('[name=\"turnstileToken\"],[name=\"cf-turnstile-response\"]');var has=tok&&tok.value&&tok.value.length>10;"
      "var noWidget=!form.querySelector('.cf-turnstile, [name=\"turnstileToken\"], [name=\"cf-turnstile-response\"]');"
      "if(has||(noWidget&&tries>=4)){clearInterval(iv);window.__cpReport(JSON.stringify({event:'clicked',url:location.href}));if(btn){btn.click();}else{form.submit();}return;}"
      "if(tries>=30){clearInterval(iv);window.__cpReport(JSON.stringify({event:'filled',url:location.href}));}"
      "},500);";
  d.submittedUrlPart = "/my";
  d.pollViaApi = true;
  return d;
}

SiteDriver atcoder(const SubmitRequest& req, const json& config) {
  SiteDriver d;
  d.id = "atcoder";
  d.name = "AtCoder";
  d.loginUrl = "https://atcoder.jp/login";
  d.homeUrl = "https://atcoder.jp/home";
  d.loginUrlPart = "/login";
  d.loggedInJs = "!!document.querySelector('#navbar-collapse a[href^=\"/users/\"], .navbar a[href^=\"/users/\"]')";
  d.handleJs = "(function(){var a=document.querySelector('#navbar-collapse a[href^=\"/users/\"]')||document.querySelector('.navbar a[href^=\"/users/\"]');return a?a.getAttribute('href').split('/')[2]:'';})()";
  static const std::regex re(R"(atcoder\.jp/contests/([^/?#]+)/tasks/([^/?#]+))");
  std::smatch m;
  std::string contest, task;
  if (std::regex_search(req.problem.url, m, re)) {
    contest = m[1];
    task = m[2];
    d.submitUrl = "https://atcoder.jp/contests/" + contest + "/submit?taskScreenName=" + task;
    d.statusUrl = "https://atcoder.jp/contests/" + contest + "/submissions/me";
  }
  d.formSelector = "form[action$=\"/submit\"], form.form-code-submit";
  auto ac = config.value("atcoder", json::object());
  int lid = req.lang == "cpp"    ? ac.value("cppLangId", 5028)
            : req.lang == "java" ? ac.value("javaLangId", 5005)
            : req.lang == "js"   ? ac.value("jsLangId", 5009)
                                 : ac.value("pythonLangId", 5055);
  std::string sid = std::to_string(lid);
  d.fillJs = std::string(kClick) +
      "var task=" + js(task) + ";"
      "var ts=form.querySelector('select[name=\"data.TaskScreenName\"]');if(ts){ts.value=task;ts.dispatchEvent(new Event('change',{bubbles:true}));if(window.jQuery){try{jQuery(ts).trigger('change');}catch(e){}}}"
      "var sels=form.querySelectorAll('select[name=\"data.LanguageId\"]');var set=false;"
      "sels.forEach(function(s){var box=s.closest('[id^=\"select-lang-\"]');if(sels.length>1&&box&&box.id!=='select-lang-'+task)return;"
      "s.value=" + js(sid) + ";if(s.value===" + js(sid) + "){set=true;s.dispatchEvent(new Event('change',{bubbles:true}));if(window.jQuery){try{jQuery(s).trigger('change');}catch(e){}}}});"
      "if(!set){throw 'AtCoder language id " + sid + " is not offered (edit atcoder ids in cp/config.json)';}"
      "var ta=form.querySelector('textarea[name=\"sourceCode\"]')||document.getElementById('sourceCode');if(ta){ta.value=src;}"
      "try{if(window.editor&&window.editor.setValue){window.editor.setValue(src,-1);}}catch(e){}"
      "setTimeout(function(){press(form);},300);";
  d.submittedUrlPart = "/submissions/me";
  d.verdictJs =
      "var task=" + js(task) + ";var rows=document.querySelectorAll('table tbody tr');var row=null;"
      "for(var i=0;i<rows.length;i++){if(rows[i].querySelector('a[href*=\"/tasks/'+task+'\"]')){row=rows[i];break;}}"
      "if(!row){window.__cpV={final:false,text:'Waiting for the submission to appear…'};}else{"
      "var lab=row.querySelector('td span.label');var st=lab?lab.textContent.trim():'';"
      "var judging=st===''||/^WJ$|^\\d+\\/\\d+|Judging|WR/i.test(st);"
      "var map={AC:'Accepted',WA:'Wrong Answer',TLE:'Time Limit Exceeded',MLE:'Memory Limit Exceeded',RE:'Runtime Error',CE:'Compilation Error',OLE:'Output Limit Exceeded',IE:'Internal Error',QLE:'Query Limit Exceeded'};"
      "window.__cpV={final:!judging,text:judging?('Judging: '+st):(map[st]||st),ok:st==='AC'};}";
  d.pollSeconds = 4;
  return d;
}

SiteDriver cses(const SubmitRequest& req, const json&) {
  SiteDriver d;
  d.id = "cses";
  d.name = "CSES";
  d.loginUrl = "https://cses.fi/login";
  d.homeUrl = "https://cses.fi/problemset/";
  d.loginUrlPart = "/login";
  d.loggedInJs = "!!document.querySelector('a[href=\"/logout\"], a[href^=\"/logout\"]')";
  d.handleJs = "(function(){var a=document.querySelector('a.account');return a?a.textContent.trim():'';})()";
  static const std::regex re(R"(cses\.fi/problemset/task/(\d+))");
  std::smatch m;
  if (std::regex_search(req.problem.url, m, re)) d.submitUrl = "https://cses.fi/problemset/submit/" + m[1].str();
  d.formSelector = "form[enctype=\"multipart/form-data\"], form[action*=\"send\"]";
  std::string langKeys = req.lang == "cpp" ? "['C++']" : req.lang == "java" ? "['Java']" : req.lang == "js" ? "['Node','JavaScript']" : "['Python3','Python']";
  std::string optKeys = req.lang == "cpp" ? "['C++20','C++17','C++11']" : req.lang == "python" ? "['CPython3','PyPy3']" : "[]";
  d.fillJs = std::string(kPickOption) + kAttachFile + kClick +
      "var ls=form.querySelector('select[name=\"lang\"]');if(!pick(ls," + langKeys + ")){throw 'language not found in the CSES form';}"
      "var os=form.querySelector('select[name=\"option\"]');if(os){pick(os," + optKeys + ");}"
      "attach(form," + js(fileName(req.lang)) + ");"
      "setTimeout(function(){press(form);},300);";
  d.submittedUrlPart = "/result/";
  d.statusFromSubmittedUrl = true;
  d.verdictJs =
      "var status='',result='';document.querySelectorAll('table tr').forEach(function(tr){var td=tr.querySelectorAll('td');if(td.length>=2){"
      "var k=td[0].textContent.trim();if(/^Status/i.test(k))status=td[1].textContent.trim();if(/^Result/i.test(k))result=td[1].textContent.trim();}});"
      "var fin=/READY/i.test(status);window.__cpV={final:fin,text:fin?(result||'Judged'):('Judging… '+status),ok:/ACCEPTED/i.test(result)};";
  d.pollSeconds = 3;
  return d;
}

SiteDriver usaco(const SubmitRequest& req, const json&) {
  SiteDriver d;
  d.id = "usaco";
  d.name = "USACO";
  d.loginUrl = "https://usaco.org/index.php";
  d.homeUrl = "https://usaco.org/index.php";
  d.loginUrlPart = "";
  d.loggedInJs = "/\\bLogout\\b/i.test(document.body.innerText||'')";
  d.handleJs = "(function(){var m=(document.body.innerText||'').match(/user:\\s*([^\\s|]+)/i);return m?m[1]:'';})()";
  d.submitUrl = req.problem.url;
  d.formSelector = "form[enctype=\"multipart/form-data\"], form:has(input[type=\"file\"])";
  std::string langKeys = req.lang == "cpp" ? "['C++17','C++11','C++']" : req.lang == "java" ? "['Java']" : "['Python 3','Python3','Python']";
  d.fillJs = std::string(kPickOption) + kAttachFile + kClick +
      "var ls=form.querySelector('select[name=\"language\"]')||form.querySelector('select');if(!pick(ls," + langKeys + ")){throw 'language not found in the USACO form';}"
      "attach(form," + js(fileName(req.lang)) + ");"
      "setTimeout(function(){press(form);},300);";
  d.pollInPlace = true;
  d.verdictJs =
      "var el=document.getElementById('trial-information')||document.querySelector('.trial-information, #results, .results');"
      "var t=el?el.innerText.trim():'';var judging=!t||/being judged|judging|compiling|please wait|submitted/i.test(t);"
      "var bad=/incorrect|wrong|error|exceeded|timeout|time limit|failed/i.test(t);var good=/correct/i.test(t);"
      "window.__cpV={final:!judging&&(good||bad),text:(judging?'Judging…':(bad?'Not all tests passed':'All tests correct')),ok:!judging&&good&&!bad};";
  d.pollSeconds = 4;
  return d;
}

}  // namespace

SiteDriver makeSiteDriver(const std::string& judgeId, const SubmitRequest& req, const json& config) {
  if (judgeId == "atcoder") return atcoder(req, config);
  if (judgeId == "cses") return cses(req, config);
  if (judgeId == "usaco") return usaco(req, config);
  return codeforces(req, config);
}
