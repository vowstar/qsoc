#include "qsoc_test.h"
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void parseArgs() {};
};

QSOC_TEST_MAIN(Test)

#include "test_qslangdriver.moc"
