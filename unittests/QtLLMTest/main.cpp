#include <iostream>
#include <QApplication>
#include "QtLLM.h"
#include "tests.h"


int main(int argc, char* argv[])
{
	// QApplication installs an event dispatcher on the main thread, required by
	// QNetworkAccessManager (used internally by Client), and a GUI stack, which
	// the widget tests need in order to construct ChatDockWidget at all.
	QApplication app(argc, argv);

	QtLLM::LibraryInfo::printInfo();

	std::cout << "Running "<< UnitTest::Test::getTests().size() << " tests...\n";
	UnitTest::Test::TestResults results;
	UnitTest::Test::runAllTests(results);
	UnitTest::Test::printResults(results);

	return results.getSuccess();
}
