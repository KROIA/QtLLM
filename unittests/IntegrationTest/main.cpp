#include <iostream>
#include <QCoreApplication>
#include "QtLLM.h"
#include "tests.h"


int main(int argc, char* argv[])
{
	// QCoreApplication installs an event dispatcher on the main thread,
	// which is required by QNetworkAccessManager (used internally by Client).
	QCoreApplication app(argc, argv);

	QtLLM::LibraryInfo::printInfo();

	std::cout << "Running "<< UnitTest::Test::getTests().size() << " tests...\n";
	UnitTest::Test::TestResults results;
	UnitTest::Test::runAllTests(results);
	UnitTest::Test::printResults(results);

	return results.getSuccess();
}
