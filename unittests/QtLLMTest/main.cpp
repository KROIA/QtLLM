#include <iostream>
#include <QApplication>
#include "QtLLM.h"
#include "tests.h"

#ifdef _MSC_VER
#include <crtdbg.h>
#include <cstdlib>
#endif
#include <cstdio>


int main(int argc, char* argv[])
{
	// QApplication installs an event dispatcher on the main thread, required by
	// QNetworkAccessManager (used internally by Client), and a GUI stack, which
	// the widget tests need in order to construct ChatDockWidget at all.
#ifdef _MSC_VER
	// A failed assertion or abort() must end the run with a message on stderr.
	// The default is a modal dialog, which blocks a scripted run forever and
	// waits on a desktop nobody may be looking at.
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
	for (int type : { _CRT_ASSERT, _CRT_ERROR }) {
		_CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
		_CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
	}
#endif
	// Unbuffered, so the log up to a crash is not lost with the buffer.
	setvbuf(stdout, nullptr, _IONBF, 0);

	QApplication app(argc, argv);

	QtLLM::LibraryInfo::printInfo();

	std::cout << "Running "<< UnitTest::Test::getTests().size() << " tests...\n";
	UnitTest::Test::TestResults results;
	UnitTest::Test::runAllTests(results);
	UnitTest::Test::printResults(results);

	return results.getSuccess();
}
