#include "common/qstaticicontheme.h"

#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QPalette>

bool QStaticIconTheme::isDarkTheme()
{
    const QColor color = QApplication::palette().color(QPalette::Active, QPalette::Base);
    return color.lightness() < 127;
}

void QStaticIconTheme::setup()
{
    /* Set theme search path */
    QIcon::setThemeSearchPaths({":icon"});
    /* Select theme by system theme */
    if (isDarkTheme()) {
        QIcon::setThemeName("dark");
    } else {
        QIcon::setThemeName("light");
    }
}
