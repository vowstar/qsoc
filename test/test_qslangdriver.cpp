#include <QtTest>

class test : public QObject
{
    Q_OBJECT

public:
    test();
    ~test() override;

private slots:
    void testParseArgs();

};

test::test()
{

}

test::~test()
{

}

void test::testParseArgs()
{

}

QTEST_APPLESS_MAIN(test)

#include "test_qslangdriver.moc"
