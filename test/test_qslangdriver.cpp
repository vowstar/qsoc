#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void parseArgs() {};
};

QTEST_APPLESS_MAIN(Test)

#include "test_qslangdriver.moc"
