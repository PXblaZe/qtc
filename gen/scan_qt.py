#!/usr/bin/env python3
"""Scan the public headers of an installed Qt 6 with libclang into a JSON API model.

The model lists, for every public class in the selected Qt modules: its bases,
constructors, public methods (with canonical parameter and return types), nested
enums, and copy/abstract traits. It also lists the enums of Qt's namespaces.
generate.py turns the model into the C++ thunk tables and the API manifest.

Usage:
  scan_qt.py --qt-include /usr/include/x86_64-linux-gnu/qt6 --out model.json
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

import clang.cindex as ci

K = ci.CursorKind
T = ci.TypeKind

# Modules bound by default, in dependency order. A module is scanned when its
# umbrella header exists under the include root.
DEFAULT_MODULES = [
    "Core", "Gui", "Widgets", "Network", "Sql", "PrintSupport", "Xml", "DBus",
    "OpenGL", "OpenGLWidgets", "Svg", "SvgWidgets", "Qml", "Quick", "QuickWidgets",
    "QuickControls2", "Multimedia", "MultimediaWidgets", "SpatialAudio", "Charts",
    "SerialPort", "Positioning", "WebSockets", "WebChannel", "UiTools",
    # network add-ons (GPL-3.0 or commercial), bound when installed
    "NetworkAuth", "HttpServer", "Mqtt", "Coap",
]

# Namespaces whose enums are exported (classes inside them are not bound).
ENUM_NAMESPACES_PREFIX = "Q"   # Qt, QSsl, QAudio, QDBus ...

SKIP_PATH_PARTS = ("/private/", "/qpa/", "/rhi/", "/QtCore/q20", "/QtCore/q23", "/QtCore/q26")


def system_includes():
    out = subprocess.run(["g++", "-std=c++20", "-E", "-x", "c++", "-", "-v"], input="",
                         capture_output=True, text=True).stderr
    dirs, on = [], False
    for line in out.splitlines():
        if line.startswith("#include <...> search starts here:"):
            on = True
            continue
        if line.startswith("End of search list."):
            break
        if on:
            dirs.append(line.strip())
    return dirs


def qualified(decl):
    parts = []
    c = decl
    while c is not None and c.kind != K.TRANSLATION_UNIT:
        if c.kind in (K.NAMESPACE, K.CLASS_DECL, K.STRUCT_DECL, K.CLASS_TEMPLATE, K.ENUM_DECL,
                      K.UNION_DECL, K.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION, K.TYPEDEF_DECL,
                      K.TYPE_ALIAS_DECL):
            if c.spelling:
                parts.append(c.spelling)
            else:
                parts.append("(anonymous)")
        c = c.semantic_parent
    return "::".join(reversed(parts))


def type_info(t):
    can = t.get_canonical()
    d = {"spelling": t.spelling, "canon": can.spelling, "kind": can.kind.name,
         "const": can.is_const_qualified()}
    if can.kind in (T.POINTER, T.LVALUEREFERENCE, T.RVALUEREFERENCE):
        pt = can.get_pointee()
        pd = {"canon": pt.spelling, "kind": pt.kind.name, "const": pt.is_const_qualified()}
        decl = pt.get_declaration()
        if decl is not None and decl.kind != K.NO_DECL_FOUND:
            pd["decl"] = qualified(decl)
            pd["complete"] = pt.get_size() > 0
        if pt.kind == T.POINTER:
            ppt = pt.get_pointee()
            pd["pointee"] = {"canon": ppt.spelling, "kind": ppt.kind.name}
        d["pointee"] = pd
    elif can.kind in (T.RECORD, T.ENUM):
        decl = can.get_declaration()
        if decl is not None and decl.kind != K.NO_DECL_FOUND:
            d["decl"] = qualified(decl)
            d["complete"] = can.get_size() > 0
        n = can.get_num_template_arguments()
        if n > 0:
            args = []
            for i in range(n):
                a = can.get_template_argument_type(i)
                if a.kind == T.INVALID:
                    args.append(None)
                else:
                    args.append(type_info(a))
            d["targs"] = args
    return d


def has_default(parm):
    try:
        for tok in parm.get_tokens():
            if tok.spelling == "=":
                return True
    except Exception:
        pass
    return False


def is_deprecated(c):
    return c.availability == ci.AvailabilityKind.DEPRECATED


def has_final(c):
    try:
        return any(ch.kind == K.CXX_FINAL_ATTR for ch in c.get_children())
    except Exception:
        return False


def method_record(c, access="public"):
    params = []
    for p in c.get_arguments():
        params.append({"name": p.spelling, "type": type_info(p.type), "default": has_default(p)})
    rec = {
        "name": c.spelling,
        "static": c.is_static_method(),
        "const": c.is_const_method(),
        "virtual": c.is_virtual_method(),
        "pure": c.is_pure_virtual_method(),
        "deprecated": is_deprecated(c),
        "final": has_final(c),
        "access": access,
        "variadic": c.type.is_function_variadic() if c.type.kind == T.FUNCTIONPROTO else False,
        "ret": type_info(c.result_type),
        "params": params,
    }
    try:
        rec["deleted"] = c.is_deleted_method()
    except Exception:
        rec["deleted"] = False
    return rec


def ctor_record(c):
    params = []
    for p in c.get_arguments():
        params.append({"name": p.spelling, "type": type_info(p.type), "default": has_default(p)})
    rec = {"params": params, "deprecated": is_deprecated(c), "explicit": False}
    try:
        rec["deleted"] = c.is_deleted_method()
        rec["copy"] = c.is_copy_constructor()
        rec["move"] = c.is_move_constructor()
    except Exception:
        rec["deleted"] = rec["copy"] = rec["move"] = False
    return rec


def enum_record(c, qname):
    vals = []
    for e in c.get_children():
        if e.kind == K.ENUM_CONSTANT_DECL:
            vals.append([e.spelling, e.enum_value])
    try:
        scoped = c.is_scoped_enum()
    except Exception:
        scoped = False
    return {"name": qname, "scoped": scoped, "underlying": c.enum_type.get_canonical().spelling,
            "values": vals, "deprecated": is_deprecated(c)}


class Scanner:
    def __init__(self, root):
        self.root = os.path.realpath(root)
        self.classes = {}
        self.enums = {}
        self.seen_files = set()

    def location_ok(self, c):
        loc = c.location
        if loc.file is None:
            return None
        path = os.path.realpath(loc.file.name)
        if not path.startswith(self.root + "/"):
            return None
        rel = path[len(self.root) + 1:]
        if rel.endswith("_p.h") or any(s in "/" + rel for s in SKIP_PATH_PARTS):
            return None
        top = rel.split("/")[0]
        if not top.startswith("Qt"):
            return None
        return top[2:], rel

    def visit(self, cursor, depth=0):
        for c in cursor.get_children():
            k = c.kind
            if k == K.NAMESPACE:
                if c.spelling.startswith(ENUM_NAMESPACES_PREFIX) and depth == 0:
                    self.visit_namespace_enums(c, c.spelling)
            elif k in (K.LINKAGE_SPEC, K.UNEXPOSED_DECL):
                self.visit(c, depth)
            elif k in (K.CLASS_DECL, K.STRUCT_DECL) and c.is_definition() and depth == 0:
                self.handle_class(c, None)

    def visit_namespace_enums(self, ns, nsname):
        for c in ns.get_children():
            if c.kind == K.ENUM_DECL and c.is_definition() and c.spelling and not c.spelling.startswith("("):
                loc = self.location_ok(c)
                if loc is None:
                    continue
                q = nsname + "::" + c.spelling
                rec = enum_record(c, q)
                rec["module"], rec["header"] = loc
                self.enums.setdefault(q, rec)

    def handle_class(self, c, outer):
        name = c.spelling
        if not name:
            return
        loc = self.location_ok(c)
        if loc is None:
            return
        if outer is None and not name.startswith("Q"):
            return
        # explicit template specializations (QConcatenable<char>, QDBusPendingReply<>) are not classes
        try:
            if c.type.get_num_template_arguments() > 0 or "<" in (c.displayname or ""):
                return
        except Exception:
            pass
        # an out-of-line definition of a nested class (class QRunnable::QGenericRunnable ...)
        sp = c.semantic_parent
        if outer is None and sp is not None and sp.kind in (K.CLASS_DECL, K.STRUCT_DECL, K.CLASS_TEMPLATE):
            return
        qname = name if outer is None else outer + "::" + name
        if qname in self.classes:
            return
        module, header = loc
        rec = {"name": qname, "module": module, "header": header, "bases": [], "ctors": [],
               "methods": [], "enums": [], "abstract": False, "has_meta": False,
               "has_metaobject_fn": False, "dtor": "implicit", "nested": outer is not None,
               "deprecated": is_deprecated(c), "final": has_final(c), "protected_ctors": []}
        try:
            rec["abstract"] = c.is_abstract_record()
        except Exception:
            pass
        self.classes[qname] = rec
        for ch in c.get_children():
            k = ch.kind
            acc = ch.access_specifier
            public = acc == ci.AccessSpecifier.PUBLIC
            if k == K.CXX_BASE_SPECIFIER:
                if public:
                    rec["bases"].append(ch.type.get_canonical().spelling)
            elif k == K.CXX_METHOD:
                if ch.spelling == "metaObject":
                    rec["has_metaobject_fn"] = True
                if public:
                    rec["methods"].append(method_record(ch))
                elif acc == ci.AccessSpecifier.PROTECTED:
                    # for overridable subclasses: protected virtuals to override, the rest to call
                    rec["methods"].append(method_record(ch, "protected"))
            elif k == K.CONSTRUCTOR:
                if public:
                    rec["ctors"].append(ctor_record(ch))
                else:
                    r = ctor_record(ch)
                    if r.get("copy"):
                        rec["copy_nonpublic"] = True
                    elif acc == ci.AccessSpecifier.PROTECTED and not r.get("move"):
                        rec["protected_ctors"].append(r)
            elif k == K.DESTRUCTOR:
                rec["dtor"] = "public" if public else "nonpublic"
            elif k == K.VAR_DECL and ch.spelling == "staticMetaObject":
                rec["has_meta"] = True
            elif k == K.ENUM_DECL and public and ch.is_definition() and ch.spelling and not ch.spelling.startswith("("):
                q = qname + "::" + ch.spelling
                er = enum_record(ch, q)
                er["module"], er["header"] = module, header
                self.enums.setdefault(q, er)
                rec["enums"].append(q)
            elif k in (K.CLASS_DECL, K.STRUCT_DECL) and public and ch.is_definition():
                self.handle_class(ch, qname)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qt-include", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--modules", default=",".join(DEFAULT_MODULES))
    ap.add_argument("--extra-include", action="append", default=[])
    args = ap.parse_args()

    root = args.qt_include
    mods = [m for m in args.modules.split(",") if m]
    present = [m for m in mods if os.path.exists(os.path.join(root, "Qt" + m, "Qt" + m))]
    missing = [m for m in mods if m not in present]
    src = "".join("#include <Qt%s/Qt%s>\n" % (m, m) for m in present)

    cargs = ["-x", "c++", "-std=c++20", "-fPIC", "-DQT_NO_DEBUG", "-w",
             "-I" + root]
    for m in present:
        cargs.append("-I" + os.path.join(root, "Qt" + m))
    for d in args.extra_include:
        cargs.append("-I" + d)
    rdir = subprocess.run(["clang", "-print-resource-dir"], capture_output=True, text=True).stdout.strip()
    for d in system_includes():
        if "/lib/gcc/" in d and d.endswith("/include") and rdir:
            d = os.path.join(rdir, "include")   # clang's own builtin headers, not gcc's
        cargs.append("-isystem" + d)

    t0 = time.time()
    index = ci.Index.create()
    tu = index.parse("qtc_scan.cpp", args=cargs, unsaved_files=[("qtc_scan.cpp", src)],
                     options=ci.TranslationUnit.PARSE_SKIP_FUNCTION_BODIES
                     | ci.TranslationUnit.PARSE_INCOMPLETE)
    errors = [d for d in tu.diagnostics if d.severity >= ci.Diagnostic.Error]
    for d in errors[:20]:
        print("diag:", d, file=sys.stderr)
    t1 = time.time()
    sc = Scanner(root)
    sc.visit(tu.cursor)
    t2 = time.time()

    version = "unknown"
    vh = os.path.join(root, "QtCore", "qtcore-config.h")
    for cand in (os.path.join(root, "QtCore", "qtversion.h"), os.path.join(root, "QtCore", "qconfig.h"),
                 os.path.join(root, "QtCore", "qtcore-config.h"), os.path.join(root, "QtCore", "qglobal.h")):
        if os.path.exists(cand):
            for line in open(cand, errors="replace"):
                if line.startswith("#define QT_VERSION_STR"):
                    version = line.split('"')[1]
                    break
        if version != "unknown":
            break

    # the QT_REQUIRE_CONFIG features of each header, so generating needs no Qt headers
    header_config = {}
    for c in sc.classes.values():
        h = c.get("header")
        if not h or h in header_config:
            continue
        cfg = []
        try:
            for line in open(os.path.join(root, h), errors="replace"):
                m = re.match(r"\s*QT_REQUIRE_CONFIG\((\w+)\)", line)
                if m:
                    cfg.append(m.group(1))
        except OSError:
            pass
        header_config[h] = cfg
    model = {"qt_version": version, "include_root": os.path.realpath(root), "modules": present,
             "missing_modules": missing, "parse_errors": len(errors), "header_config": header_config,
             "classes": sc.classes, "enums": sc.enums}
    with open(args.out, "w") as f:
        json.dump(model, f, separators=(",", ":"))
    nm = sum(len(c["methods"]) for c in sc.classes.values())
    print("Qt %s: modules=%d (missing %s) classes=%d methods=%d enums=%d errors=%d parse=%.1fs walk=%.1fs"
          % (version, len(present), ",".join(missing) or "-", len(sc.classes), nm, len(sc.enums),
             len(errors), t1 - t0, t2 - t1))


if __name__ == "__main__":
    main()
