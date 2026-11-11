// Qt SQL's part of the ownership roles (see qtc_roles.cpp and qtc_module.h).
#include "../qtc_module.h"

#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlDriver>

namespace qtc {
namespace {

bool owned_class(QObject *o) { return qobject_cast<QSqlDriver *>(o) != nullptr; }

QString owned_by_class(QObject *x) {
    // drivers live inside QSqlDatabase's shared data (or are a static null driver)
    if (qobject_cast<QSqlDriver *>(x) && !roles_made_by_qtc(x))
        return QStringLiteral("the driver of a QSqlDatabase; use QSqlDatabase::removeDatabase");
    return QString();
}

const ModuleExt kExt = [] {
    ModuleExt e;
    e.module = "Sql";
    e.qt_owned_class = owned_class;
    e.qt_owned_by_class = owned_by_class;
    return e;
}();

}  // namespace
}  // namespace qtc

QTC_MODULE_EXT(Sql) { qtc::register_module_ext(&qtc::kExt); }
