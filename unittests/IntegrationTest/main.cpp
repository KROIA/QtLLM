#include <iostream>
#include <QApplication>
#include "QtLLM.h"
#include "tests.h"


int main(int argc, char* argv[])
{
	// QApplication: an event dispatcher for QNetworkAccessManager (used
	// internally by Client), and a GUI stack for the tests in which a real
	// model operates real widgets.
	QApplication app(argc, argv);

	QtLLM::LibraryInfo::printInfo();

	std::cout << "Running "<< UnitTest::Test::getTests().size() << " tests...\n";
	UnitTest::Test::TestResults results;
	UnitTest::Test::runAllTests(results);
	UnitTest::Test::printResults(results);

	return results.getSuccess();
}
