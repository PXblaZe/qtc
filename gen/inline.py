#!/usr/bin/env python3
"""Inline values: Qt's small value types as C structs the caller holds, with no handle.

QPoint, QRect, QColor and the like are a few machine words. Through qtc_val each is a QVariant
in qtc's registry: made, looked up and freed on every use. Here each is a C struct of exactly
the type's layout (checked at compile time), passed and returned by value, and its methods are
C functions that call Qt directly:

    qtc_QColor c = qtci_QColor__int_int_int(255, 0, 0);
    int64_t r = qtci_QColor_red(c);
    qtci_QRect_moveTo__int_int(&rect, 10, 20);      // methods that change the value take a pointer

The checks stay: integers are range-checked into Qt's types, a pointer must not be null, and a
QColor must hold a color qtc made (its spec and, for HSV and HSL, its hue are valid); errors go
to qtc_last_error as for every call, and the result is then zero (or the value unchanged).
These functions touch no global state: any thread, before qtc_init too.

Methods of object classes that take such values get a second entry point beside qtc_direct.h's:
qtcmi_<method>__<types>(receiver, ...) takes them as structs (see entries.py).

Writes:
  include/qtc_inline.h            the structs and functions
  src/gen/qtc_gen_inline.cpp      their implementation
"""
import os
import re

# Qt type -> (C struct fields, its Goose name). Fields follow Qt's own members, in order.
TYPES = {
    "QPoint": ([("int32_t", "xp"), ("int32_t", "yp")], "Point", "Core"),
    "QPointF": ([("double", "xp"), ("double", "yp")], "PointF", "Core"),
    "QSize": ([("int32_t", "wd"), ("int32_t", "ht")], "Size", "Core"),
    "QSizeF": ([("double", "wd"), ("double", "ht")], "SizeF", "Core"),
    "QRect": ([("int32_t", "x1"), ("int32_t", "y1"), ("int32_t", "x2"), ("int32_t", "y2")], "Rect", "Core"),
    "QRectF": ([("double", "xp"), ("double", "yp"), ("double", "w"), ("double", "h")], "RectF", "Core"),
    "QLine": ([("int32_t", "p1x"), ("int32_t", "p1y"), ("int32_t", "p2x"), ("int32_t", "p2y")], "Line", "Core"),
    "QLineF": ([("double", "p1x"), ("double", "p1y"), ("double", "p2x"), ("double", "p2y")], "LineF", "Core"),
    "QMargins": ([("int32_t", "m_left"), ("int32_t", "m_top"), ("int32_t", "m_right"), ("int32_t", "m_bottom")],
                 "Margins", "Core"),
    "QMarginsF": ([("double", "m_left"), ("double", "m_top"), ("double", "m_right"), ("double", "m_bottom")],
                  "MarginsF", "Core"),
    # opaque: the meaning of the components depends on the spec
    "QColor": ([("int32_t", "spec"), ("uint16_t", "c0"), ("uint16_t", "c1"), ("uint16_t", "c2"), ("uint16_t", "c3"),
                ("uint16_t", "c4"), ("uint16_t", "c5")], "Color", "Gui"),
}
# a sample value and its fields, to check the layout at compile time (constexpr constructors)
LAYOUT_CHECKS = {
    "QPoint": ("QPoint(1, 2)", "{1, 2}"),
    "QPointF": ("QPointF(1.5, 2.5)", "{1.5, 2.5}"),
    "QSize": ("QSize(3, 4)", "{3, 4}"),
    "QSizeF": ("QSizeF(3.5, 4.5)", "{3.5, 4.5}"),
    "QRect": ("QRect(1, 2, 3, 4)", "{1, 2, 3, 5}"),
    "QRectF": ("QRectF(1, 2, 3, 4)", "{1, 2, 3, 4}"),
    "QLine": ("QLine(1, 2, 3, 4)", "{1, 2, 3, 4}"),
    "QLineF": ("QLineF(1, 2, 3, 4)", "{1, 2, 3, 4}"),
    "QMargins": ("QMargins(1, 2, 3, 4)", "{1, 2, 3, 4}"),
    "QMarginsF": ("QMarginsF(1, 2, 3, 4)", "{1, 2, 3, 4}"),
}

# scalar parameter types: C type, conversion into the Qt type
SCALAR_PARAMS = {
    ("int", "int"): ("int64_t", "int"),
    ("uint", "uint"): ("int64_t", "uint"),
    ("unsigned int", "uint"): ("int64_t", "uint"),
    ("QRgb", "uint"): ("int64_t", "uint"),
    ("qreal", "double"): ("double", "double"),
    ("double", "double"): ("double", "double"),
    ("float", "double"): ("double", "float"),
    ("bool", "bool"): ("bool", "bool"),
}
SCALAR_RETS = {"int": "int64_t", "unsigned int": "int64_t", "uint": "int64_t", "QRgb": "int64_t", "bool": "bool",
               "double": "double", "qreal": "double", "float": "double", "void": "void"}
ZERO = {"int64_t": "0", "bool": "false", "double": "0.0"}


def cstruct(t):
    return "qtc_" + t


def mangle_type(t):
    s = t.replace(" ", "").replace("::", "_").replace("*", "Ptr").replace("&", "Ref")
    s = s.replace("<", "Of").replace(">", "").replace(",", "And")
    return re.sub(r"[^0-9A-Za-z_]", "_", s)


def param_ok(p):
    return (p["type"], p["pk"]) in SCALAR_PARAMS or (p["pk"] == "value" and p["type"] in TYPES)


def ret_ok(m):
    rt = m.get("ret_type")
    return rt in SCALAR_RETS or rt in TYPES


def collect(manifest):
    """Per inline type: (ctors, methods), each a list of (name, params, ret_type, const, static)
    cut to every arity its defaults allow."""
    out = {}
    classes = {c["name"]: c for c in manifest["classes"]}
    for t in TYPES:
        c = classes.get(t)
        if not c:
            continue
        ctors, methods, seen = [], [], set()
        for k in c["ctors"]:
            for n in range(k["min"], len(k["params"]) + 1):
                ps = k["params"][:n]
                if not all(param_ok(p) for p in ps):
                    continue
                key = ("<ctor>", tuple(p["type"] for p in ps))
                if key in seen:
                    continue
                seen.add(key)
                ctors.append(("", ps, t, False, True))
        for m in c["methods"]:
            if m.get("deprecated") or not ret_ok(m):
                continue
            for n in range(m["min"], len(m["params"]) + 1):
                ps = m["params"][:n]
                if not all(param_ok(p) for p in ps):
                    continue
                key = (m["name"], tuple(p["type"] for p in ps), m["static"])
                if key in seen:
                    continue
                seen.add(key)
                methods.append((m["name"], ps, m["ret_type"], m["const"], m["static"]))
        out[t] = (ctors, methods)
    return out


def fname(t, name, ps):
    base = "qtci_%s" % t + ("_%s" % name if name else "")
    if ps:
        base += "__" + "_".join(mangle_type(p["type"]) for p in ps)
    return base


def cparams(ps):
    out = []
    for j, p in enumerate(ps):
        if p["pk"] == "value":
            out.append("%s a%d" % (cstruct(p["type"]), j))
        else:
            out.append("%s a%d" % (SCALAR_PARAMS[(p["type"], p["pk"])][0], j))
    return out


def cret(rt):
    return cstruct(rt) if rt in TYPES else SCALAR_RETS[rt]


def generate(manifest, out):
    api = collect(manifest)
    h = ["/* qtc_inline.h: Qt's small value types as C structs, with no handle. Generated by",
         " * gen/inline.py. Do not edit.",
         " *",
         " * Each struct has exactly the layout of its Qt type. Make values with the constructors",
         " * (qtci_QRect__int_int_int_int) or by filling the fields of the geometry types (QRect keeps",
         " * its far corner: x2 = x + width - 1); a QColor is opaque. Functions that change a value",
         " * take a pointer to it. Errors (an integer out of range, a null pointer, a QColor qtc did",
         " * not make) are reported through qtc_last_error, with a zero result; these functions use",
         " * no global state, so any thread may call them, also before qtc_init.",
         " *",
         " * qtc_arg_<T> pushes one as an argument of qtc_call, qtc_ret_<T> reads a call's result as",
         " * one, qtc_val_from_<T> and qtc_val_to_<T> convert to and from a qtc_val handle, and",
         " * qtcmi_* in qtc_direct.h are the direct entry points that take them. */",
         "#ifndef QTC_INLINE_H", "#define QTC_INLINE_H", '#include "qtc.h"', "#ifdef __cplusplus", 'extern "C" {',
         "#endif", ""]
    for t, (fields, gname, mod) in TYPES.items():
        h.append("typedef struct %s { %s } %s;" % (cstruct(t), " ".join("%s %s;" % f for f in fields), cstruct(t)))
    h.append("")
    for t in TYPES:
        cs = cstruct(t)
        h.append("QTC_API void qtc_arg_%s(%s v);" % (t, cs))
        h.append("QTC_API %s qtc_ret_%s(void);" % (cs, t))
        h.append("QTC_API qtc_val qtc_val_from_%s(%s v);" % (t, cs))
        h.append("QTC_API qtc_status qtc_val_to_%s(qtc_val v, %s *out);" % (t, cs))
    h.append("")

    cpp = ["// Generated by gen/inline.py. Do not edit.", '#include "../qtc_internal.h"', '#include "qtc_inline.h"', "",
           "#include <bit>", "#include <QtGui/QColor>", "#include <QtCore/QLine>", "#include <QtCore/QMargins>", ""]
    for t, (fields, gname, mod) in TYPES.items():
        cs = cstruct(t)
        cpp.append("static_assert(sizeof(%s) == sizeof(%s) && alignof(%s) == alignof(%s), \"%s layout\");" % (cs, t, cs, t, t))
        cpp.append("static_assert(std::is_trivially_copyable_v<%s>, \"%s is trivially copyable\");" % (t, t))
        if t in LAYOUT_CHECKS:
            sample, fields_v = LAYOUT_CHECKS[t]
            cpp.append("static_assert([] { constexpr %s c = std::bit_cast<%s>(%s); constexpr %s want%s; "
                       "return %s; }(), \"%s field order\");"
                       % (cs, cs, sample, cs, fields_v,
                          " && ".join("c.%s == want.%s" % (f[1], f[1]) for f in fields), t))
    cpp.append("")
    cpp.append("namespace {")
    cpp.append("template <class Q, class C> inline Q in(const C &c) { return std::bit_cast<Q>(c); }")
    cpp.append("template <class C, class Q> inline C out(const Q &q) { return std::bit_cast<C>(q); }")
    cpp.append("""// a QColor qtc made: Qt switches on the spec (an unknown one is undefined behaviour) and
// indexes by the hue
inline bool color_ok(const qtc_QColor &c) {
    if (c.spec < int(QColor::Invalid) || c.spec > int(QColor::ExtendedRgb)) return false;
    if ((c.spec == int(QColor::Hsv) || c.spec == int(QColor::Hsl)) && c.c1 >= 36000 && c.c1 != 0xffff) return false;
    return true;
}
template <class C> inline bool valid(const C &) { return true; }
template <> inline bool valid<qtc_QColor>(const qtc_QColor &c) { return color_ok(c); }
const char *const kBadColor = "not a QColor qtc made (its spec or hue is out of range)";
inline bool int_in(int64_t v, int &o, int i) {
    if (v < INT_MIN || v > INT_MAX) { qtc::fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("argument %1: %2 does not fit an int").arg(i).arg(v)); return false; }
    o = int(v);
    return true;
}
inline bool uint_in(int64_t v, uint &o, int i) {
    if (v < 0 || v > int64_t(UINT_MAX)) { qtc::fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("argument %1: %2 does not fit an unsigned int").arg(i).arg(v)); return false; }
    o = uint(v);
    return true;
}
inline bool float_in(double v, float &o, int i) {
    if (std::isfinite(v) && std::fabs(v) > double(std::numeric_limits<float>::max())) {
        qtc::fail(QTC_ERR_OUT_OF_RANGE, QStringLiteral("argument %1: %2 does not fit a float").arg(i).arg(v));
        return false;
    }
    o = float(v);
    return true;
}
}  // namespace
""")

    def body(t, ps, call, rt, fail_ret):
        lines = []
        for j, p in enumerate(ps):
            if p["pk"] == "value":
                pt = p["type"]
                if pt == "QColor":
                    lines.append("    if (!color_ok(a%d)) { qtc::fail(QTC_ERR_INVALID_ARG, kBadColor); return %s; }" % (j, fail_ret))
                lines.append("    const %s p%d = in<%s>(a%d);" % (pt, j, pt, j))
            else:
                conv = SCALAR_PARAMS[(p["type"], p["pk"])][1]
                if conv == "int":
                    lines.append("    int p%d; if (!int_in(a%d, p%d, %d)) return %s;" % (j, j, j, j, fail_ret))
                elif conv == "uint":
                    lines.append("    uint p%d; if (!uint_in(a%d, p%d, %d)) return %s;" % (j, j, j, j, fail_ret))
                elif conv == "float":
                    lines.append("    float p%d; if (!float_in(a%d, p%d, %d)) return %s;" % (j, j, j, j, fail_ret))
                else:
                    lines.append("    const auto p%d = a%d;" % (j, j))
        return lines

    nfun = 0
    man = {"types": {t: {"fields": [[ct, n] for ct, n in f], "goose": g, "module": mod} for t, (f, g, mod) in TYPES.items()},
           "functions": []}

    def pdesc(ps):
        return [[p["type"], "value" if p["pk"] == "value" else SCALAR_PARAMS[(p["type"], p["pk"])][1]] for p in ps]

    for t, (ctors, methods) in api.items():
        cs = cstruct(t)
        h.append("/* %s */" % t)
        for name, ps, rt, const, static in ctors:
            fn = fname(t, "", ps)
            sig = "QTC_API %s %s(%s)" % (cs, fn, ", ".join(cparams(ps)) or "void")
            h.append(sig + ";")
            man["functions"].append({"c": fn, "type": t, "kind": "ctor", "name": "", "params": pdesc(ps), "ret": t})
            cpp.append(sig + " {")
            cpp.extend(body(t, ps, None, rt, "%s{}" % cs))
            cpp.append("    qtc::clear_error();")
            cpp.append("    return out<%s>(%s(%s));" % (cs, t, ", ".join("p%d" % j for j in range(len(ps)))))
            cpp.append("}")
            nfun += 1
        for name, ps, rt, const, static in methods:
            fn = fname(t, name, ps)
            r = cret(rt)
            fail_ret = "%s{}" % r if rt in TYPES else ZERO.get(r, "")
            if static:
                params = cparams(ps)
                target = "%s::%s" % (t, name)
            elif const:
                params = ["%s self" % cs] + cparams(ps)
                target = "s.%s" % name
            else:
                params = ["%s *self" % cs] + cparams(ps)
                target = "s.%s" % name
            sig = "QTC_API %s %s(%s)" % (r, fn, ", ".join(params) or "void")
            h.append(sig + ";")
            man["functions"].append({"c": fn, "type": t, "kind": "static" if static else ("const" if const else "mut"),
                                     "name": name, "params": pdesc(ps), "ret": rt})
            cpp.append(sig + " {")
            if not static:
                if not const:
                    cpp.append("    if (!self) { qtc::fail(QTC_ERR_NULL_HANDLE, \"self is null\"); return %s; }" % fail_ret)
                    cpp.append("    if (!valid(*self)) { qtc::fail(QTC_ERR_INVALID_ARG, kBadColor); return %s; }" % fail_ret)
                    cpp.append("    %s s = in<%s>(*self);" % (t, t))
                else:
                    cpp.append("    if (!valid(self)) { qtc::fail(QTC_ERR_INVALID_ARG, kBadColor); return %s; }" % fail_ret)
                    cpp.append("    const %s s = in<%s>(self);" % (t, t))
            cpp.extend(body(t, ps, None, rt, fail_ret))
            call = "%s(%s)" % (target, ", ".join("p%d" % j for j in range(len(ps))))
            if rt == "void":
                cpp.append("    %s;" % call)
                if not static and not const:
                    cpp.append("    *self = out<%s>(s);" % cs)
                cpp.append("    qtc::clear_error();")
            elif rt in TYPES:
                cpp.append("    const %s r = out<%s>(%s(%s));" % (r, r, rt, call))
                if not static and not const:
                    cpp.append("    *self = out<%s>(s);" % cs)
                cpp.append("    qtc::clear_error();")
                cpp.append("    return r;")
            else:
                cpp.append("    const %s r = %s(%s);" % (r, r, call))
                if not static and not const:
                    cpp.append("    *self = out<%s>(s);" % cs)
                cpp.append("    qtc::clear_error();")
                cpp.append("    return r;")
            cpp.append("}")
            nfun += 1
        h.append("")
    # interop with the dynamic API
    for t in TYPES:
        cs = cstruct(t)
        cpp.append("""QTC_API void qtc_arg_%(t)s(%(cs)s v) {
    if (!valid(v)) {   // the call it is for fails, as after any argument qtc could not take
        qtc::fail(QTC_ERR_INVALID_ARG, kBadColor);
        qtc::push_arg_bad();
        return;
    }
    qtc::push_arg_variant(QVariant::fromValue(in<%(t)s>(v)));
}
QTC_API %(cs)s qtc_ret_%(t)s(void) {
    %(t)s q;
    if (!qtc::ret_as(QMetaType::fromType<%(t)s>(), &q)) return %(cs)s{};
    return out<%(cs)s>(q);
}
QTC_API qtc_val qtc_val_from_%(t)s(%(cs)s v) {
    if (!valid(v)) { qtc::fail(QTC_ERR_INVALID_ARG, kBadColor); return 0; }
    return qtc::new_value_checked(QVariant::fromValue(in<%(t)s>(v)));
}
QTC_API qtc_status qtc_val_to_%(t)s(qtc_val v, %(cs)s *o) {
    if (!o) return qtc::fail(QTC_ERR_NULL_HANDLE, "out is null");
    %(t)s q;
    if (qtc_status e = qtc::value_as(v, QMetaType::fromType<%(t)s>(), &q)) return e;
    *o = out<%(cs)s>(q);
    return QTC_OK;
}""" % {"t": t, "cs": cs})
        nfun += 4
    cpp.append("")
    h += ["#ifdef __cplusplus", "}", "#endif", "#endif", ""]
    # what the direct entry points (qtcmi_*, entries.py) use to pass them on
    conv = ["// Generated by gen/inline.py. Do not edit.", "#pragma once", '#include "../qtc_internal.h"',
            '#include "qtc_inline.h"', "#include <bit>", "#include <QtGui/QColor>", "#include <QtCore/QLine>",
            "#include <QtCore/QMargins>", "", "namespace qtc {"]
    for t in TYPES:
        cs = cstruct(t)
        if t == "QColor":
            conv.append("inline bool put_inline(Arg &x, const %s &v) {" % cs)
            conv.append("    if (v.spec < int(QColor::Invalid) || v.spec > int(QColor::ExtendedRgb) ||")
            conv.append("        ((v.spec == int(QColor::Hsv) || v.spec == int(QColor::Hsl)) && v.c1 >= 36000 && v.c1 != 0xffff)) {")
            conv.append("        fail(QTC_ERR_INVALID_ARG, \"not a QColor qtc made (its spec or hue is out of range)\");")
            conv.append("        return false;")
            conv.append("    }")
        else:
            conv.append("inline bool put_inline(Arg &x, const %s &v) {" % cs)
        conv.append("    x.kind = AK_VAL;")
        conv.append("    x.v = QVariant::fromValue(std::bit_cast<%s>(v));" % t)
        conv.append("    return true;")
        conv.append("}")
    conv += ["}  // namespace qtc", ""]
    write_if_changed(os.path.join(out, "src", "gen", "qtc_inline_conv.h"), "\n".join(conv))
    write_if_changed(os.path.join(out, "include", "qtc_inline.h"), "\n".join(h))
    import json
    write_if_changed(os.path.join(out, "manifest", "qtc_inline.json"), json.dumps(man, indent=0))
    write_if_changed(os.path.join(out, "src", "gen", "qtc_gen_inline.cpp"), "\n".join(cpp))
    return nfun


def write_if_changed(path, text):
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return
    with open(path, "w") as f:
        f.write(text)
