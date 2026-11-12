#!/usr/bin/env python3
"""Include-what-you-use check for standard-library symbols (portability to libstdc++).

libc++ (macOS) pulls many standard headers in transitively, libstdc++ (GCC on Linux) fewer,
so a file that uses std::optional without <optional> can build here and fail in Linux CI.
For every tracked C++ file this follows the project headers it includes, collects the
standard headers in that closure, and reports each std:: symbol whose header is not there.
A heuristic (a symbol table, comments and strings stripped), not a compiler.

  python3 tools/include_audit.py [ROOT]      exit 1 if anything is reported
"""
import re,subprocess,os,sys
root=sys.argv[1] if len(sys.argv)>1 else os.path.join(os.path.dirname(os.path.abspath(__file__)),'..'); os.chdir(root)
files=subprocess.run(['git','ls-files','*.cpp','*.hpp','*.h'],capture_output=True,text=True).stdout.split()
H={ # symbol -> header
 'optional':'optional','nullopt':'optional','function':'functional','thread':'thread','this_thread':'thread',
 'atomic':'atomic','chrono':'chrono','memcpy':'cstring','memset':'cstring','memcmp':'cstring','strerror':'cstring','strlen':'cstring',
 'numeric_limits':'limits','array':'array','span':'span','string':'string','to_string':'string','stoull':'string','stoll':'string',
 'vector':'vector','min':'algorithm','max':'algorithm','sort':'algorithm','lower_bound':'algorithm','upper_bound':'algorithm',
 'find_if':'algorithm','fill':'algorithm','copy':'algorithm','equal':'algorithm','reverse':'algorithm','clamp':'algorithm','any_of':'algorithm',
 'all_of':'algorithm','count_if':'algorithm','remove_if':'algorithm','unique_ptr':'memory','make_unique':'memory','shared_ptr':'memory',
 'make_shared':'memory','pair':'utility','move':'utility','exchange':'utility','swap':'utility','forward':'utility',
 'runtime_error':'stdexcept','invalid_argument':'stdexcept','logic_error':'stdexcept','out_of_range':'stdexcept','length_error':'stdexcept',
 'ostringstream':'sstream','istringstream':'sstream','stringstream':'sstream','setw':'iomanip','setprecision':'iomanip','setfill':'iomanip',
 'accumulate':'numeric','iota':'numeric','deque':'deque','map':'map','multimap':'map','set':'set','unordered_map':'unordered_map',
 'unordered_set':'unordered_set','variant':'variant','visit':'variant','holds_alternative':'variant','get_if':'variant','monostate':'variant',
 'string_view':'string_view','bit_ceil':'bit','countr_zero':'bit','popcount':'bit','bit_cast':'bit','from_chars':'charconv','to_chars':'charconv',
 'mutex':'mutex','lock_guard':'mutex','unique_lock':'mutex','scoped_lock':'mutex','condition_variable':'condition_variable',
 'random_device':'random','mt19937':'random','mt19937_64':'random','uniform_int_distribution':'random','filesystem':'filesystem',
 'ifstream':'fstream','ofstream':'fstream','fstream':'fstream','cout':'iostream','cerr':'iostream','printf':'cstdio','fprintf':'cstdio',
 'snprintf':'cstdio','FILE':'cstdio','fopen':'cstdio','errc':'system_error','error_code':'system_error','system_error':'system_error',
 'invoke':'functional','uint8_t':'cstdint','uint16_t':'cstdint','uint32_t':'cstdint','uint64_t':'cstdint','int64_t':'cstdint','int32_t':'cstdint',
 'size_t':'cstddef','ptrdiff_t':'cstddef','byte':'cstddef','tuple':'tuple','tie':'tuple','list':'list','queue':'queue','priority_queue':'queue',
 'initializer_list':'initializer_list','abs':'cstdlib','exit':'cstdlib','getenv':'cstdlib','strtoull':'cstdlib','sqrt':'cmath','floor':'cmath','ceil':'cmath',
 'llround':'cmath','isfinite':'cmath','pow':'cmath','log2':'cmath','hash':'functional','less':'functional','greater':'functional',
 'type_identity':'type_traits','is_same_v':'type_traits','conditional_t':'type_traits','remove_cvref_t':'type_traits','decay_t':'type_traits',
 'is_integral_v':'type_traits','underlying_type_t':'type_traits','numbers':'numbers','pmr':'memory_resource','source_location':'source_location',
 'distance':'iterator','next':'iterator','prev':'iterator','back_inserter':'iterator','begin':'iterator','end':'iterator','size':'iterator',
 'data':'iterator','advance':'iterator','ref':'functional','cref':'functional','in_place':'utility','exception':'exception',
 'exception_ptr':'exception','current_exception':'exception','rethrow_exception':'exception','terminate':'exception',
 'ios':'ios','istream':'istream','ostream':'ostream','getline':'string','barrier':'barrier','latch':'latch','jthread':'thread',
 'stop_token':'stop_token','format':'format','ranges':'ranges','strong_ordering':'compare','partial_ordering':'compare',
}
# headers that are guaranteed to provide others (standard-mandated): <iostream> -> istream/ostream/ios; <string>->string_view? (no). Keep minimal:
IMPLIES={'iostream':{'istream','ostream','ios'},'fstream':{'istream','ostream','ios'},'sstream':{'istream','ostream','ios','string'},
 'istream':{'ios','ostream'},'ostream':{'ios'},'iterator':set(),
 # begin/end/size/data/distance/next/prev are also provided by every container header (standard: [iterator.range])
}
CONTAINERS={'array','deque','forward_list','list','map','regex','set','span','string','string_view','unordered_map','unordered_set','vector','optional'}
ITER_FUNCS={'begin','end','size','data','distance','next','prev','advance'}
inc_re=re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]',re.M)
def includes(path):
    try: src=open(path,encoding='utf-8',errors='replace').read()
    except FileNotFoundError: return [],[]
    std=[];proj=[]
    for kind,name in inc_re.findall(src):
        (std if kind=='<' and not name.startswith('enginex/') and not name.startswith('gtest') and not name.startswith('nlohmann') else proj).append(name)
    return std,proj
def resolve(name, cur):
    for base in (os.path.dirname(cur),'include','tests','src','benchmarks','.'):
        p=os.path.normpath(os.path.join(base,name))
        if os.path.isfile(p): return p
    return None
def closure(path):
    seen=set(); stdh=set(); stack=[path]
    while stack:
        p=stack.pop()
        if p in seen: continue
        seen.add(p); s,pr=includes(p); stdh.update(s)
        for n in pr:
            if n.startswith('nlohmann'): stdh.update({'string','vector','map','string_view','functional','utility','memory','cstdint','cstddef','initializer_list','iterator','stdexcept'}); continue
            if n.startswith('gtest'): continue
            r=resolve(n,p)
            if r: stack.append(r)
    return stdh
use_re=re.compile(r'\bstd::(?:chrono::|this_thread::|pmr::|filesystem::)?([A-Za-z_]\w*)')
report=[]
for f in files:
    if f.startswith('build/') or '/_deps/' in f: continue
    src=open(f,encoding='utf-8',errors='replace').read()
    code=re.sub(r'//[^\n]*','',src); code=re.sub(r'/\*.*?\*/','',code,flags=re.S); code=re.sub(r'"(?:\\.|[^"\\\n])*"','""',code)
    have=closure(f); have2=set(have)
    for h in list(have): have2|=IMPLIES.get(h,set())
    for m in re.finditer(r'\bstd::(chrono|this_thread|pmr|filesystem|numbers|ranges)\b',code):
        need={'chrono':'chrono','this_thread':'thread','pmr':'memory_resource','filesystem':'filesystem','numbers':'numbers','ranges':'ranges'}[m.group(1)]
        if need not in have2: report.append((f,'std::'+m.group(1),need))
    for m in use_re.finditer(code):
        sym=m.group(1)
        if sym in ('chrono','this_thread','pmr','filesystem','numbers','ranges'): continue
        need=H.get(sym)
        if not need or need in have2: continue
        if sym in ITER_FUNCS and (have2 & CONTAINERS): continue
        report.append((f,'std::'+sym,need))
seen=set()
for r in report:
    if r in seen: continue
    seen.add(r); print(f'{r[0]}: {r[1]} needs <{r[2]}>')
print('include audit:', len(seen), 'missing standard header(s)')
sys.exit(1 if seen else 0)
