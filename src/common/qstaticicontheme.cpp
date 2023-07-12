#include "common/qstaticicontheme.h"

#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QPalette>

void QStaticIconTheme::setup()
{
    const QColor color = QApplication::palette().color(QPalette::Active, QPalette::Base);
    QIcon::setThemeSearchPaths({":icon"});
    if (color.lightness() < 127) {
        QIcon::setThemeName("dark");
    } else {
        QIcon::setThemeName("light");
    }
}
