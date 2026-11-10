"""Overridable subclasses (src/qtc_override.h).

For a curated set of Qt classes, a C++ subclass QtcOv_<Class> whose virtual methods a program
overrides one object at a time: each overridable virtual becomes a trampoline that hands the call
to the program's handler when the object overrides it, and runs the class's own implementation
otherwise. The classes are those Qt expects programs to subclass; the methods, those whose
arguments a frame can carry (events, painters, buffers and out-parameters included).
"""
import re

WIDGET_EVENTS = ["mousePressEvent", "mouseReleaseEvent", "mouseDoubleClickEvent", "mouseMoveEvent", "wheelEvent",
                 "keyPressEvent", "keyReleaseEvent", "focusInEvent", "focusOutEvent", "enterEvent", "leaveEvent",
                 "paintEvent", "moveEvent", "resizeEvent", "closeEvent", "contextMenuEvent", "dragEnterEvent",
                 "dragMoveEvent", "dragLeaveEvent", "dropEvent", "showEvent", "hideEvent", "changeEvent"]
WIDGET_MORE = ["sizeHint", "minimumSizeHint", "heightForWidth", "hasHeightForWidth"]
WINDOW_EVENTS = ["exposeEvent", "resizeEvent", "moveEvent", "focusInEvent", "focusOutEvent", "showEvent", "hideEvent",
                 "closeEvent", "keyPressEvent", "keyReleaseEvent", "mousePressEvent", "mouseReleaseEvent",
                 "mouseDoubleClickEvent", "mouseMoveEvent", "wheelEvent"]
OBJECT = ["timerEvent", "customEvent"]
MODEL = ["index", "parent", "sibling", "rowCount", "columnCount", "hasChildren", "data", "setData", "headerData",
         "setHeaderData", "flags", "insertRows", "insertColumns", "removeRows", "removeColumns", "moveRows",
         "moveColumns", "canFetchMore", "fetchMore", "sort", "buddy", "roleNames", "mimeTypes", "supportedDropActions",
         "supportedDragActions", "itemData", "setItemData", "clearItemData"]
MODEL_EXTRA = ["createIndex", "beginInsertRows", "endInsertRows", "beginRemoveRows", "endRemoveRows", "beginMoveRows",
               "endMoveRows", "beginInsertColumns", "endInsertColumns", "beginRemoveColumns", "endRemoveColumns",
               "beginMoveColumns", "endMoveColumns", "beginResetModel", "endResetModel", "changePersistentIndex",
               "changePersistentIndexList", "persistentIndexList"]
WIDGET_EXTRA = ["updateMicroFocus", "focusNextChild", "focusPreviousChild"]

# class: (methods to make overridable, methods Qt calls from other threads, protected methods to expose)
OVERRIDABLE = {
    "QObject": (OBJECT, [], []),
    "QWidget": (OBJECT + WIDGET_EVENTS + WIDGET_MORE, [], WIDGET_EXTRA),
    "QFrame": (OBJECT + WIDGET_EVENTS + WIDGET_MORE, [], WIDGET_EXTRA),
    "QDialog": (OBJECT + WIDGET_EVENTS + WIDGET_MORE + ["accept", "reject", "done"], [], WIDGET_EXTRA),
    "QOpenGLWidget": (OBJECT + WIDGET_EVENTS + WIDGET_MORE + ["initializeGL", "paintGL", "resizeGL"], [], WIDGET_EXTRA),
    "QRhiWidget": (OBJECT + WIDGET_EVENTS + WIDGET_MORE + ["initialize", "render", "releaseResources"], [], WIDGET_EXTRA),
    "QWindow": (OBJECT + WINDOW_EVENTS, [], []),
    "QRasterWindow": (OBJECT + WINDOW_EVENTS + ["paintEvent"], [], []),
    "QOpenGLWindow": (OBJECT + WINDOW_EVENTS + ["initializeGL", "paintGL", "resizeGL", "paintUnderGL", "paintOverGL"], [], []),
    "QAbstractItemModel": (OBJECT + MODEL, [], MODEL_EXTRA),
    "QAbstractListModel": (OBJECT + MODEL, [], MODEL_EXTRA),
    "QAbstractTableModel": (OBJECT + MODEL, [], MODEL_EXTRA),
    "QSortFilterProxyModel": (OBJECT + ["filterAcceptsRow", "filterAcceptsColumn", "lessThan", "data", "headerData",
                                        "flags", "setData", "roleNames"], [],
                              ["invalidateFilter", "invalidateRowsFilter", "invalidateColumnsFilter"]),
    "QIdentityProxyModel": (OBJECT + ["data", "headerData", "flags", "roleNames"], [], []),
    "QSyntaxHighlighter": (OBJECT + ["highlightBlock"], [],
                           ["setFormat", "format", "previousBlockState", "currentBlockState", "setCurrentBlockState",
                            "currentBlock"]),
    "QValidator": (OBJECT + ["validate", "fixup"], [], []),
    "QIODevice": (OBJECT + ["readData", "writeData", "readLineData", "bytesAvailable", "bytesToWrite", "isSequential",
                            "open", "close", "size", "pos", "seek", "atEnd", "reset", "canReadLine"], [],
                  ["setOpenMode", "setErrorString"]),
    "QThread": (["run"], ["run"], []),
    "QTcpServer": (OBJECT + ["incomingConnection", "hasPendingConnections"], [], ["addPendingConnection"]),
    "QAbstractAnimation": (OBJECT + ["duration", "updateCurrentTime", "updateState", "updateDirection"], [], []),
    "QVariantAnimation": (OBJECT + ["updateCurrentTime", "updateState", "updateCurrentValue", "interpolated"], [], []),
    "QGraphicsScene": (OBJECT + ["drawBackground", "drawForeground"], [], []),
    "QGraphicsView": (OBJECT + WIDGET_EVENTS + ["drawBackground", "drawForeground"], [], WIDGET_EXTRA),
    "QStyledItemDelegate": (OBJECT + ["displayText", "setEditorData", "setModelData"], [], []),
    "QCompleter": (OBJECT + ["splitPath", "pathFromIndex"], [], []),
}

# Virtuals Qt overrides in a private section: implemented there, and not overridable from here (the
# scan records public and protected members only). They are not pure for the classes below them.
PRIVATE_OVERRIDES = {
    "QAbstractListModel": {"parent", "columnCount", "hasChildren"},
    "QAbstractTableModel": {"parent", "hasChildren"},
    "QWindow": {"surfaceHandle"},
    "QPaintDeviceWindow": {"paintEngine"},
}

PAINT = {"paintEvent", "paintGL", "paintUnderGL", "paintOverGL"}
NONNEG = {"rowCount", "columnCount"}
OWN_INDEX = {"index", "parent", "sibling", "buddy"}
SIGNED = {"signed char", "short", "int", "long", "long long"}
UNSIGNED = {"unsigned char", "unsigned short", "unsigned int", "unsigned long", "unsigned long long"}
VALUE_PKS = {"PK_STR", "PK_BYTES", "PK_CHAR", "PK_VARIANT", "PK_VALUE", "PK_LIST", "PK_MAP", "PK_INDEX"}


def bare(name):
    return name.replace("class ", "").replace("struct ", "").strip()


def chain(ctx, cn):
    """cn and its bases, most derived first."""
    out, seen = [], set()

    def walk(n):
        n = bare(n)
        if n in seen or n not in ctx.classes:
            return
        seen.add(n)
        out.append(n)
        for b in ctx.classes[n]["bases"]:
            walk(b)
    walk(cn)
    return out


def is_event(ctx, name):
    return "QEvent" in chain(ctx, name)


def mkey(m):
    return (m["name"], tuple(p["type"]["canon"] for p in m["params"]), bool(m["const"]))


def virtuals(ctx, cn):
    """[(declaring class, method)] for every virtual of cn, the most derived declaration of each."""
    found, order, private = {}, [], set()
    for d in chain(ctx, cn):
        for m in ctx.classes[d]["methods"]:
            if not m.get("virtual") or m["name"].startswith("~") or m["name"] in private:
                continue
            k = mkey(m)
            if k in found:
                continue
            found[k] = (d, m)
            order.append(k)
        private |= PRIVATE_OVERRIDES.get(d, set())
    return [found[k] for k in order]


def value_kind(ctx, name, is_ret, map_value):
    if name == "bool":
        return "OT_BOOL", None
    if name in SIGNED:
        return "OT_SINT", None
    if name in UNSIGNED:
        return "OT_UINT", None
    if name == "float":
        return "OT_FLOAT", None
    if name == "double":
        return "OT_DOUBLE", None
    if name in ctx.enums or name.startswith("QFlags<"):
        return "OT_ENUM", None
    pt, why = map_value(ctx, name)
    if pt is not None and pt.pk in VALUE_PKS:
        return "OT_META", None
    if is_ret:
        return None, "returns %s" % (why or name)
    return "OT_OPAQUE", None


def ov_kind(ctx, ti, is_ret, map_value):
    """The frame kind of a parameter or return type: (kind, None) or (None, why not)."""
    k = ti["kind"]
    canon = ti["canon"]
    if k == "VOID":
        return ("OT_VOID", None) if is_ret else (None, "void parameter")
    if k == "RVALUEREFERENCE":
        return None, "rvalue reference"
    if k in ("FUNCTIONPROTO", "MEMBERPOINTER", "CONSTANTARRAY", "INCOMPLETEARRAY", "BLOCKPOINTER", "VARIABLEARRAY"):
        return None, k.lower()
    if k == "LVALUEREFERENCE":
        pt = ti["pointee"]
        name = canon.rstrip("&").strip()
        if name.startswith("const "):
            name = name[6:].strip()
        if not pt.get("const"):
            if is_ret:
                return None, "returns a reference"
            if name in SIGNED or name in UNSIGNED or name in ("QString", "QByteArray", "bool", "double"):
                return "OT_REF", None
            return "OT_OPAQUE", None
        if is_ret:
            return None, "returns a reference"
        return value_kind(ctx, name, is_ret, map_value)
    if k == "POINTER" or canon.endswith("*"):
        pd = ti.get("pointee", {})
        pname = bare(pd.get("canon", canon[:-1]))
        pconst = bool(pd.get("const")) or pname.startswith("const ")
        if pname.startswith("const "):
            pname = pname[6:].strip()
        if is_ret:
            return None, "returns a pointer"
        if pname == "char":
            return ("OT_CBUF" if pconst else "OT_BUF"), None
        if pname == "QPainter" and not pconst:
            return "OT_PAINTER", None
        if pname in ctx.classes and is_event(ctx, pname):
            return "OT_EVENT", None
        if pname in ctx.classes and ctx.is_qobject(pname):
            return "OT_OBJ", None
        return "OT_OPAQUE", None
    name = canon[6:] if canon.startswith("const ") else canon
    if k == "ENUM":
        return "OT_ENUM", None
    return value_kind(ctx, name, is_ret, map_value)


def sig_of(m):
    """A normalized signature as qtc_override takes one."""
    from_types = []
    for p in m["params"]:
        t = p["type"]["canon"]
        t = re.sub(r"\s+", " ", t)
        if t.startswith("const ") and t.endswith("&"):
            t = t[6:-1].strip()
        from_types.append(t.replace(" *", "*").replace(" &", "&"))
    return "%s(%s)" % (m["name"], ",".join(from_types))


def safe(cn):
    return re.sub(r"\W", "_", cn)


class OvEmitter:
    def __init__(self, em, ctx, map_value, cstr, guard, Versions, need_widgets, need_gui):
        self.em, self.ctx, self.map_value, self.cstr, self.guard, self.Versions = em, ctx, map_value, cstr, guard, Versions
        self.em_need_widgets, self.em_need_gui = need_widgets, need_gui
        self.manifest = []

    def plan_class(self, cn):
        """The methods of cn's QtcOv_ class, or (None, why)."""
        ctx = self.ctx
        names, any_thread, extras = OVERRIDABLE[cn]
        c = ctx.classes.get(cn)
        if c is None:
            return None, "not in this Qt"
        if c.get("final"):
            return None, "final class"
        methods = []
        for d, m in virtuals(ctx, cn):
            pure = bool(m.get("pure"))
            if m["name"] not in names and not pure:
                continue
            if m.get("final"):
                if pure:
                    return None, "final pure virtual"
                continue
            params = []
            for p in m["params"]:
                kind, why = ov_kind(ctx, p["type"], False, self.map_value)
                if kind is None:
                    params = None
                    break
                params.append((p, kind))
            rk, why = ov_kind(ctx, m["ret"], True, self.map_value)
            if params is None or rk is None:
                if pure:
                    return None, "pure virtual %s::%s cannot be overridden from C" % (d, m["name"])
                continue
            methods.append({"decl": d, "m": m, "params": params, "ret": rk, "pure": pure,
                            "any": m["name"] in any_thread})
        if not methods:
            return None, "nothing overridable"
        return {"methods": methods, "extras": extras}, None

    def emit(self, cn, out_code, out_tables, out_defs):
        ctx, em, cstr = self.ctx, self.em, self.cstr
        plan, why = self.plan_class(cn)
        if plan is None:
            return False
        c = ctx.classes[cn]
        module = c["module"]
        cls = "QtcOv_%s" % safe(cn)
        is_model = "QAbstractItemModel" in chain(ctx, cn)
        base = "qtc::QtcModelOps<%s>" % cn if is_model else cn
        csince = em.v.since(module, "class", cn)
        cguard = self.guard(csince, em.floor)
        conds = [cguard] if cguard else []
        for x in em.header_config(c["header"]):
            conds.append("QT_CONFIG(%s)" % x)
        conds.append("__has_include(<%s>)" % c["header"])
        cond = " && ".join(conds)

        lines = ["#if " + cond, "class %s : public %s, public qtc::OvHost {" % (cls, base),
                 "    qtc::OvLink ov_link_{this, this};", "public:",
                 "    using %s::%s;" % (base, base.split("::")[-1].split("<")[0] if not is_model else "QtcModelOps"),
                 "    ~%s() override { ov_end(); }" % cls]
        base_fns, ovm = [], []
        man = []
        for i, mp in enumerate(plan["methods"]):
            m, d = mp["m"], mp["decl"]
            msince = em.v.since(ctx.classes[d]["module"], "method", self.Versions.mkey(d, m))
            mg = self.guard(msince, csince if cguard else em.floor)
            ret = m["ret"]["canon"]
            void = mp["ret"] == "OT_VOID"
            const = " const" if m["const"] else ""
            ps = ", ".join("%s a%d" % (p["type"]["canon"], k) for k, (p, _) in enumerate(mp["params"]))
            pre, slots = [], []
            for k, (p, kind) in enumerate(mp["params"]):
                t = p["type"]["canon"]
                if kind == "OT_OBJ":
                    pre.append("QObject *x%d = const_cast<std::remove_const_t<std::remove_pointer_t<%s>> *>(a%d);" % (k, t, k))
                    slots.append("&x%d" % k)
                elif kind == "OT_EVENT":
                    pre.append("QEvent *x%d = const_cast<std::remove_const_t<std::remove_pointer_t<%s>> *>(a%d);" % (k, t, k))
                    slots.append("&x%d" % k)
                else:
                    slots.append("const_cast<void *>(static_cast<const void *>(&a%d))" % k)
            argv = "void *argv[] = {%s};" % ", ".join(slots) if slots else "void **argv = nullptr;"
            callargs = ", ".join("a%d" % k for k in range(len(mp["params"])))
            self_ = "const_cast<%s *>(this)" % cls
            body = []
            if mg:
                body.append("#if " + mg)
            body.append("    %s %s(%s)%s override {" % (ret, m["name"], ps, const))
            body.append("        if (ov_on(%d)) {" % i)
            for x in pre:
                body.append("            " + x)
            body.append("            " + argv)
            if void:
                body.append("            if (qtc::ov_dispatch(%s, %s, %d, argv, nullptr)) return;" % (self_, self_, i))
            else:
                body.append("            %s r{};" % ret)
                body.append("            if (qtc::ov_dispatch(%s, %s, %d, argv, &r)) return r;" % (self_, self_, i))
            body.append("        }")
            if mp["pure"]:
                body.append("        return%s;" % ("" if void else " {}"))
            else:
                body.append("        %s%s::%s(%s);" % ("" if void else "return ", cn, m["name"], callargs))
            body.append("    }")
            if mg:
                body.append("#endif")
            lines.extend(body)
            # the class's own implementation, for qtc_ov_call_base
            bfn = "nullptr"
            if not mp["pure"]:
                bname = "ob_%s_%d" % (safe(cn), i)
                bargs = []
                for k, (p, kind) in enumerate(mp["params"]):
                    t = p["type"]["canon"]
                    if kind == "OT_OBJ":
                        bargs.append("static_cast<%s>(*static_cast<QObject **>(argv[%d]))" % (t, k))
                    elif kind == "OT_EVENT":
                        bargs.append("static_cast<%s>(*static_cast<QEvent **>(argv[%d]))" % (t, k))
                    else:
                        bargs.append("*static_cast<std::remove_reference_t<%s> *>(argv[%d])" % (t, k))
                call = "s->%s::%s(%s)" % (cn, m["name"], ", ".join(bargs))
                if void:
                    stmt = "%s;" % call
                else:
                    stmt = "*static_cast<%s *>(ret) = %s;" % (ret, call)
                # a member, so that it reaches protected implementations
                b = ["    static void %s(QObject *self, void **argv, void *ret) {" % bname,
                     "        auto *s = static_cast<%s *>(self);" % cls,
                     "        %s" % stmt, "        (void)argv; (void)ret;", "    }"]
                if mg:
                    b = ["#if " + mg] + b + ["#endif"]
                base_fns.extend(b)
                bfn = "%s::%s" % (cls, bname)
            # descriptors
            tname = "OT_%s_%d" % (safe(cn), i)
            # opaque parameters need no metatype (their type may be incomplete here)
            descs = ["qtc::ov_t<%s>(qtc::%s, %s)" % ("void" if kind == "OT_OPAQUE" else p["type"]["canon"], kind,
                                                     cstr(p["type"]["canon"]))
                     for p, kind in mp["params"]]
            flags = []
            if mp["pure"]:
                flags.append("qtc::OVF_PURE")
            if mp["any"]:
                flags.append("qtc::OVF_ANY_THREAD")
            if m["name"] in PAINT:
                flags.append("qtc::OVF_PAINT")
            if m["name"] in NONNEG:
                flags.append("qtc::OVF_NONNEG")
            if m["name"] in OWN_INDEX and is_model:
                flags.append("qtc::OVF_OWN_INDEX")
            rdesc = "qtc::ov_t<%s>(qtc::%s, %s)" % ("void" if void else ret, mp["ret"], cstr(ret))
            tab = []
            if mg:
                tab.append("#if " + mg)
            if descs:
                tab.append("static const qtc::OvType %s[] = {%s};" % (tname, ", ".join(descs)))
            if mg:
                tab.append("#endif")
            out_tables.extend(tab)
            entry = '    {%s, %s, %d, %s, %s, %s, %s},' % (
                cstr(m["name"]), cstr(sig_of(m)), len(mp["params"]), tname if descs else "nullptr", rdesc,
                bfn if bfn else "nullptr", "|".join(flags) or "0")
            if mg:
                # an older Qt without the method keeps the slot (ids stay the same), never dispatched
                entry = "#if %s\n%s\n#else\n    {%s, %s, 0, nullptr, qtc::OvType{}, nullptr, 0},\n#endif" % (
                    mg, entry, cstr(m["name"]), cstr(sig_of(m)))
            ovm.append(entry)
            man.append({"name": m["name"], "sig": sig_of(m), "params": [p["type"]["canon"] for p, _ in mp["params"]],
                        "kinds": [kind for _, kind in mp["params"]], "ret": ret, "ret_kind": mp["ret"],
                        "pure": mp["pure"], "thread": "any" if mp["any"] else "gui", "declared_in": d})
            self.em.cov[module]["ov_methods"] += 1
        # protected methods made callable, and their thunks
        extra_thunks, extra_entries, man_extra = [], [], []
        using = []
        for name in plan["extras"]:
            for d in chain(ctx, cn):
                for m in ctx.classes[d]["methods"]:
                    if m["name"] != name or m.get("access") != "protected" or m.get("virtual"):
                        continue
                    if any("void *" in p["type"]["canon"] or "void*" in p["type"]["canon"] for p in m["params"]):
                        continue   # createIndex(int, int, const void *): the quintptr one stands
                    xplan, xwhy = em.method_plan(d, ctx.classes[d], m, True)
                    if xplan is None:
                        continue
                    start = len(extra_thunks)
                    tid = em.thunk(d, ctx.classes[d], m, xplan, "obj", extra_thunks)
                    if tid is None:
                        del extra_thunks[start:]
                        continue
                    xsig = "%s(%s)" % (m["name"], ",".join(pt.sig for _, pt in xplan["params"]))
                    pdesc = em.param_desc(xplan)
                    parr = "PX%d" % tid
                    if pdesc:
                        extra_thunks.append("static const qtc::ParamDesc %s[] = {%s};" % (parr, ", ".join(pdesc)))
                    extra_entries.append('    {%s, %s, t%d<%s>, %s, %d, %d, %s, %s, %s},' % (
                        cstr(m["name"]), cstr(xsig), tid, cls, parr if pdesc else "nullptr", xplan["max"], xplan["min"],
                        em.flags_of(d, m, False), xplan["ret"][0], cstr(xplan["ret"][1])))
                    man_extra.append(xsig)
                    if not is_model or name not in MODEL_EXTRA:
                        using.append("    using %s::%s;" % (d, name))
                break_out = any(e for e in man_extra if e.startswith(name + "("))
                if break_out:
                    break
        for u in sorted(set(using)):
            lines.append(u)
        lines.extend(base_fns)
        # a runtime class's object (qtc_class_*) answers with its own meta-object
        lines.append("    const QMetaObject *metaObject() const override { return ov_dyn ? qtc::dyn_meta(ov_dyn) : %s::metaObject(); }" % cn)
        lines.append("    void *qt_metacast(const char *n) override {")
        lines.append("        if (ov_dyn && qtc::dyn_is(ov_dyn, n)) return static_cast<QObject *>(this);")
        lines.append("        return %s::qt_metacast(n);" % cn)
        lines.append("    }")
        lines.append("    int qt_metacall(QMetaObject::Call c, int id, void **a) override {")
        lines.append("        id = %s::qt_metacall(c, id, a);" % cn)
        lines.append("        if (id < 0 || !ov_dyn) return id;")
        lines.append("        return qtc::dyn_metacall(this, this, c, id, a);")
        lines.append("    }")
        if is_model:
            lines.append("    static QModelIndex ov_make_index(const QObject *o, int r, int c, quintptr id) {")
            lines.append("        return static_cast<const %s *>(o)->createIndex(r, c, id);" % cls)
            lines.append("    }")
        lines.append("};")
        lines.append("#endif")
        out_code.append("\n".join(lines))
        # constructors
        ctor_thunks, ctor_entries = [], []
        if c.get("dtor") != "nonpublic":
            for k in c["ctors"]:
                kplan, kwhy = em.method_plan(cn, c, k, True, is_ctor=True)
                if kplan is None:
                    continue
                start = len(ctor_thunks)
                tid = em.thunk(cn, c, k, kplan, "ctor_obj", ctor_thunks)
                if tid is None:
                    del ctor_thunks[start:]
                    continue
                ksig = "%s(%s)" % (cn, ",".join(pt.sig for _, pt in kplan["params"]))
                pdesc = em.param_desc(kplan)
                parr = "PK%d" % tid
                if pdesc:
                    ctor_thunks.append("static const qtc::ParamDesc %s[] = {%s};" % (parr, ", ".join(pdesc)))
                ctor_entries.append('    {%s, %s, t%d<%s>, %s, %d, %d, 0, QTC_T_OBJ, %s},' % (
                    cstr(cn), cstr(ksig), tid, cls, parr if pdesc else "nullptr", kplan["max"], kplan["min"],
                    cstr(cn + "*")))
        if not ctor_entries:
            out_code.append("// %s: no public constructor qtc can call" % cn)
            return False
        sc = safe(cn)
        block = ["#if " + cond] + extra_thunks + ctor_thunks
        block.append("static const qtc::OvMethod OVM_%s[] = {" % sc)
        block.extend(ovm)
        block.append("};")
        block.append("static const qtc::MethodDef OVC_%s[] = {" % sc)
        block.extend(ctor_entries)
        block.append("};")
        block.append("static const qtc::MethodDef OVX_%s[] = {" % sc)
        block.extend(extra_entries)
        block.append("    {nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0, nullptr}};")
        block.append("#endif")
        out_tables.append("\n".join(block))
        need = "qtc::NEED_CORE"
        if module in self.em_need_widgets:
            need = "qtc::NEED_WIDGETS"
        elif module in self.em_need_gui:
            need = "qtc::NEED_GUI"
        out_defs.append("#if %s\n    {%s, %s, %s, OVC_%s, %d, OVM_%s, %d, OVX_%s, %d, %s},\n#endif" % (
            cond, cstr(cn), cstr(module), need, sc, len(ctor_entries), sc, len(ovm), sc, len(extra_entries),
            ("%s::ov_make_index" % cls) if is_model else "nullptr"))
        self.manifest.append({"class": cn, "module": module, "methods": man, "protected": man_extra})
        self.em.cov[module]["ov_classes"] += 1
        return True
