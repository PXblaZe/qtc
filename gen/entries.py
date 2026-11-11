#!/usr/bin/env python3
"""Direct entry points: one exported C function per bound method signature and per constructor.

A direct entry point is what qtc_call does for one fixed signature, without the work that exists
only because qtc_call is dynamic: no method name to look up, no argument stack, no overload
resolution after the first call on a class. The checks are the same ones: the handle, its
thread, the class, every argument (in the thunk), ownership rules.

    qtcm_<method>__<types>(receiver, args...) -> qtc_status    any class with that signature
    qtcn_<Class>__<types>(args...) -> handle                   an object (qtc_new) or a value (qtc_val_new)

Writes:
  src/gen/qtc_gen_entries_<k>.cpp     the functions
  src/gen/qtc_gen_entry_table.cpp     the signatures they stand for (qtc::g_entries, qtc::g_ctor_entries)
  include/qtc_direct.h                their C declarations
  manifest/qtc_entries.json           signature -> function name, for language bindings

Run by generate.py, or on its own: gen/entries.py --out <qtc source tree> (reads manifest/qtc_api.json).
"""
import argparse
import json
import os
import re

CTYPE = {"bool": "bool", "int": "int64_t", "uint": "int64_t", "double": "double", "str": "qtc_str",
         "bytes": "qtc_str", "obj": "qtc_obj", "value": "qtc_val", "variant": "qtc_val", "list": "qtc_val",
         "map": "qtc_val"}
PUT = {"bool": "qtc::put_bool(a[%d], a%d);", "int": "qtc::put_int(a[%d], a%d);", "uint": "qtc::put_int(a[%d], a%d);",
       "double": "qtc::put_double(a[%d], a%d);", "obj": "qtc::put_obj(a[%d], a%d);"}
PUT_CHECKED = {"str": "qtc::put_str", "bytes": "qtc::put_bytes", "value": "qtc::put_val", "variant": "qtc::put_val",
               "list": "qtc::put_val", "map": "qtc::put_val"}
POINTER = {"out", "null"}

# methods qtc_call treats specially (ownership, quitting, threads): their entries take qtc_call's path
SLOW_NAMES = {"setParent", "blockSignals", "createWindowContainer", "quit", "exit", "exec", "moveToThread",
              "deleteLater", "destroy"}
E_SLOW, E_EXTRAS = 1, 2


def mangle_type(t):
    s = t.replace(" ", "").replace("::", "_").replace("*", "Ptr").replace("&", "Ref")
    s = s.replace("<", "Of").replace(">", "").replace(",", "And")
    return re.sub(r"[^0-9A-Za-z_]", "_", s)


def cident(s):
    return re.sub(r"[^0-9A-Za-z_]", "_", s.replace("::", "_"))


def cstr(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def collect(manifest):
    """(methods, ctors): methods maps a signature (cut to an arity) to its parameter kinds;
    ctors maps (class, signature) to parameter kinds."""
    methods, ctors = {}, {}
    for c in manifest["classes"]:
        for m in c["methods"]:
            if m["static"]:
                continue   # no receiver: qtc_call_static
            ps = m["params"]
            for k in range(m["min"], len(ps) + 1):
                sig = "%s(%s)" % (m["name"], ",".join(p["type"] for p in ps[:k]))
                kinds = [p["pk"] for p in ps[:k]]
                if any(pk not in CTYPE and pk not in POINTER for pk in kinds):
                    continue
                methods.setdefault(sig, (m["name"], [p["type"] for p in ps[:k]], kinds))
        for m in c["ctors"]:
            ps = m["params"]
            for k in range(m["min"], len(ps) + 1):
                kinds = [p["pk"] for p in ps[:k]]
                if any(pk not in CTYPE and pk not in POINTER for pk in kinds):
                    continue
                sig = "%s(%s)" % (c["name"], ",".join(p["type"] for p in ps[:k]))
                ctors.setdefault((c["name"], sig), ([p["type"] for p in ps[:k]], kinds))
    return methods, ctors


def generate(manifest, out, chunks=6):
    methods, ctors = collect(manifest)
    extras = set()
    for ov in manifest.get("overridables", []):
        for sig in ov.get("protected", []):
            extras.add(sig.split("(", 1)[0])

    used = set()

    def unique(base):
        name, n = base, 2
        while name in used:
            name = "%s_%d" % (base, n)
            n += 1
        used.add(name)
        return name

    mlist = []   # (id, sig, fname, kinds, flags)
    for i, sig in enumerate(sorted(methods)):
        name, types, kinds = methods[sig]
        fname = unique("qtcm_%s__%s" % (name, "_".join(mangle_type(t) for t in types)) if types else "qtcm_%s" % name)
        flags = (E_SLOW if name in SLOW_NAMES else 0) | (E_EXTRAS if name in extras else 0)
        mlist.append((i, sig, fname, kinds, flags))
    clist = []   # (id, class, sig, fname, kinds)
    for i, (cn, sig) in enumerate(sorted(ctors)):
        types, kinds = ctors[(cn, sig)]
        fname = unique("qtcn_%s__%s" % (cident(cn), "_".join(mangle_type(t) for t in types)) if types else "qtcn_%s" % cident(cn))
        clist.append((i, cn, sig, fname, kinds))

    def params(kinds):
        ps = []
        for j, pk in enumerate(kinds):
            if pk not in POINTER:
                ps.append("%s a%d" % (CTYPE[pk], j))
        return ps

    def body(kinds, fail_ret):
        lines = []
        if kinds:
            lines.append("    qtc::Arg a[%d];" % len(kinds))
        for j, pk in enumerate(kinds):
            if pk in POINTER:
                lines.append("    qtc::put_null(a[%d]);" % j)
            elif pk in PUT:
                lines.append("    " + PUT[pk] % (j, j))
            else:
                lines.append("    if (!%s(a[%d], a%d)) return %s;" % (PUT_CHECKED[pk], j, j, fail_ret))
        return lines

    gen_dir = os.path.join(out, "src", "gen")
    files = []
    head = ["// Generated by gen/entries.py. Do not edit.", '#include "../qtc_internal.h"', '#include "qtc_direct.h"', ""]
    per = (len(mlist) + len(clist) + chunks - 1) // chunks
    items = [("m", x) for x in mlist] + [("n", x) for x in clist]
    for k in range(chunks):
        part = items[k * per:(k + 1) * per]
        if not part:
            continue
        code = list(head)
        for kind, x in part:
            if kind == "m":
                i, sig, fname, kinds, flags = x
                ps = ["qtc_obj self"] + params(kinds)
                code.append("QTC_API qtc_status %s(%s) {" % (fname, ", ".join(ps)))
                code.append("    if (!qtc::entry_ready()) return qtc::last_error_code();")
                code.extend(body(kinds, "qtc::last_error_code()"))
                code.append("    return qtc::entry_call(self, %d, %s, %d);" % (i, "a" if kinds else "nullptr", len(kinds)))
                code.append("}")
            else:
                i, cn, sig, fname, kinds = x
                code.append("QTC_API uint64_t %s(%s) {" % (fname, ", ".join(params(kinds)) or "void"))
                code.append("    if (!qtc::entry_ready()) return 0;")
                code.extend(body(kinds, "0"))
                code.append("    return qtc::entry_new(%d, %s, %d);" % (i, "a" if kinds else "nullptr", len(kinds)))
                code.append("}")
        fn = "qtc_gen_entries_%d.cpp" % k
        write_if_changed(os.path.join(gen_dir, fn), "\n".join(code) + "\n")
        files.append(fn)

    tab = ["// Generated by gen/entries.py. Do not edit.", '#include "../qtc_internal.h"', "", "namespace {",
           "const qtc::EntryDesc ENTRIES[] = {"]
    for i, sig, fname, kinds, flags in mlist:
        tab.append("    {%s, %d, %d, %d}," % (cstr(sig), len(sig), len(kinds), flags))
    tab.append("    {nullptr, 0, 0, 0}};")
    tab.append("const qtc::CtorEntryDesc CTORS[] = {")
    for i, cn, sig, fname, kinds in clist:
        tab.append("    {%s, %d, %s, %d}," % (cstr(cn), len(cn), cstr(sig), len(kinds)))
    tab.append("    {nullptr, 0, nullptr, 0}};")
    tab.append("}  // namespace")
    tab.append("const qtc::EntryDesc *const qtc::g_entries = ENTRIES;")
    tab.append("const uint32_t qtc::g_entry_count = %d;" % len(mlist))
    tab.append("const qtc::CtorEntryDesc *const qtc::g_ctor_entries = CTORS;")
    tab.append("const uint32_t qtc::g_ctor_entry_count = %d;" % len(clist))
    write_if_changed(os.path.join(gen_dir, "qtc_gen_entry_table.cpp"), "\n".join(tab) + "\n")
    files.append("qtc_gen_entry_table.cpp")

    h = ["/* qtc_direct.h: direct entry points, one per bound method signature and constructor.",
         " * Generated by gen/entries.py. Do not edit.",
         " *",
         " * qtcm_<method>__<types>(receiver, ...) calls that exact signature on whatever class the receiver",
         " * is, like qtc_call(receiver, \"method(types)\") with those arguments: the same checks and errors,",
         " * and the result is read with qtc_ret_* as after qtc_call. It neither uses nor clears the",
         " * arguments pushed with qtc_arg_*. Pointer parameters that qtc fills (out-parameters) are",
         " * left out. qtcn_<Class>__<types>(...) makes an object (as qtc_new) or a value (as qtc_val_new).",
         " * Integers are int64_t, strings and byte arrays qtc_str, objects qtc_obj, values qtc_val. */",
         "#ifndef QTC_DIRECT_H", "#define QTC_DIRECT_H", "#include \"qtc.h\"", "#ifdef __cplusplus", 'extern "C" {',
         "#endif"]
    for i, sig, fname, kinds, flags in mlist:
        h.append("QTC_API qtc_status %s(%s); /* %s */" % (fname, ", ".join(["qtc_obj self"] + params(kinds)), sig))
    for i, cn, sig, fname, kinds in clist:
        h.append("QTC_API uint64_t %s(%s); /* %s */" % (fname, ", ".join(params(kinds)) or "void", sig))
    h += ["#ifdef __cplusplus", "}", "#endif", "#endif", ""]
    write_if_changed(os.path.join(out, "include", "qtc_direct.h"), "\n".join(h))

    man = {"methods": {sig: fname for i, sig, fname, kinds, flags in mlist},
           "ctors": {"%s|%s" % (cn, sig): fname for i, cn, sig, fname, kinds in clist}}
    write_if_changed(os.path.join(out, "manifest", "qtc_entries.json"), json.dumps(man, separators=(",", ":"), sort_keys=True))
    return files, len(mlist), len(clist)


def write_if_changed(path, text):
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return
    with open(path, "w") as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="the qtc source tree")
    args = ap.parse_args()
    with open(os.path.join(args.out, "manifest", "qtc_api.json")) as f:
        manifest = json.load(f)
    files, nm, nc = generate(manifest, args.out)
    print("entries: %d methods, %d constructors in %d files" % (nm, nc, len(files)))
    cm = os.path.join(args.out, "src", "gen", "modules.cmake")
    with open(cm) as f:
        lines = [l for l in f.read().splitlines() if not l.startswith("set(QTC_GEN_ENTRIES")]
    lines.append("set(QTC_GEN_ENTRIES %s)" % " ".join("${CMAKE_CURRENT_LIST_DIR}/" + f for f in files))
    write_if_changed(cm, "\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
